/* cd_mcp_mesh_tools.c - Cadence Engine MCP mesh/component attachment tools
 *
 * Implements:
 *   - mesh.attach_primitive : Attach a primitive mesh (cube/sphere/plane/cylinder) to a node
 *   - mesh.set_material     : Update material properties on a node's MeshRenderer
 *   - mesh.remove           : Remove MeshRenderer component from a node
 *
 * All handlers follow the cd_mcp_tool_handler_t signature.
 * Component mutations go through cd_command_execute_sync().
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_error.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_scene.h"
#include "cadence/cd_commands.h"
#include "cadence/cd_type_registry.h"
#include "cadence/cd_memory.h"
#include "cadence/cd_renderer_types.h"
#include "cd_gltf_import.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

#define CD_ID_STR_BUF_SIZE 24

static cd_id_t cd_id_parse(const char* str) {
    if (!str) return CD_ID_INVALID;
    uint32_t index = 0, gen = 0;
    int scanned = sscanf(str, "%u:%u", &index, &gen);
    if (scanned != 2) return CD_ID_INVALID;
    return cd_id_make(gen, index);
}

static const char* cd_id_format(cd_id_t id, char* buf, size_t buf_size) {
    uint32_t index = cd_id_index(id);
    uint32_t gen   = cd_id_generation(id);
    snprintf(buf, buf_size, "%u:%u", index, gen);
    return buf;
}

/* Map primitive name string to uint32_t type value */
static uint32_t primitive_name_to_type(const char* name) {
    if (!name) return 0;
    if (strcmp(name, "sphere") == 0)   return 2;
    if (strcmp(name, "plane") == 0)    return 3;
    if (strcmp(name, "cylinder") == 0) return 4;
    return 1; /* "cube" or default */
}

/* Look up MeshRenderer type_id via the type registry (no extern globals) */
static cd_id_t find_mesh_renderer_type(cd_kernel_t* kernel) {
    if (!kernel || !cd_kernel_get_types(kernel)) return CD_ID_INVALID;
    return cd_type_find(cd_kernel_get_types(kernel), "MeshRenderer");
}

/* ============================================================================
 * mesh.attach_primitive handler
 *
 * Input:  { "id": "2:3", "primitive": "cube"|"sphere"|"plane"|"cylinder",
 *           "color": [r,g,b,a], "roughness": 0.5, "metallic": 0.0 }
 * Output: { "status": "ok" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_mesh_attach_primitive(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    if (!kernel || !cd_kernel_get_scene(kernel)) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("No active scene",
                          "mesh.attach_primitive requires a scene.",
                          "Call scene.create3d or scene.open first.");
        return NULL;
    }

    /* Parse required "id" */
    const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (!id_item || !cJSON_IsString(id_item) || !id_item->valuestring) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing required parameter: id";
        return NULL;
    }
    cd_id_t node_id = cd_id_parse(id_item->valuestring);
    if (!cd_id_is_valid(node_id)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Invalid node id format (expected index:generation)";
        return NULL;
    }

    /* Validate node exists */
    cd_node_t* node = cd_node_get(cd_kernel_get_scene(kernel), node_id);
    if (!node) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Node not found";
        return NULL;
    }

    /* Look up MeshRenderer type */
    cd_id_t mr_type_id = find_mesh_renderer_type(kernel);
    if (!cd_id_is_valid(mr_type_id)) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "MeshRenderer type not registered";
        return NULL;
    }

    /* Parse optional "primitive" (default: "cube") */
    const char* prim_str = "cube";
    const cJSON* prim_item = cJSON_GetObjectItemCaseSensitive(params, "primitive");
    if (prim_item && cJSON_IsString(prim_item) && prim_item->valuestring)
        prim_str = prim_item->valuestring;

    /* Build MeshRenderer component data with defaults */
    cd_mesh_renderer_data_t mr_data;
    memset(&mr_data, 0, sizeof(mr_data));
    mr_data.mesh_asset    = CD_ID_INVALID;
    mr_data.material_ref  = CD_ID_INVALID;
    mr_data.albedo_color[0] = 1.0f;
    mr_data.albedo_color[1] = 1.0f;
    mr_data.albedo_color[2] = 1.0f;
    mr_data.albedo_color[3] = 1.0f;
    mr_data.roughness       = 0.5f;
    mr_data.metallic        = 0.0f;
    mr_data.cast_shadows    = true;
    mr_data.receive_shadows = true;
    mr_data.transparent     = false;
    mr_data.bounding_radius = 1.0f;
    mr_data.primitive_type  = primitive_name_to_type(prim_str);

    /* Parse optional "mesh_uri" — if set, overrides primitive_type */
    const cJSON* uri_item = cJSON_GetObjectItemCaseSensitive(params, "mesh_uri");
    if (uri_item && cJSON_IsString(uri_item) && uri_item->valuestring &&
        uri_item->valuestring[0] != '\0') {
        size_t uri_len = strlen(uri_item->valuestring);
        char* uri_copy = (char*)cd_mem_alloc_tagged(uri_len + 1, CD_MEM_MCP);
        if (uri_copy) {
            memcpy(uri_copy, uri_item->valuestring, uri_len + 1);
            mr_data.mesh_uri.data = uri_copy;
            mr_data.mesh_uri.length = (uint32_t)uri_len;
            mr_data.mesh_uri.hash = cd_fnv1a(uri_copy, uri_len);
        }
    }

    /* Parse optional "color" */
    const cJSON* color_item = cJSON_GetObjectItemCaseSensitive(params, "color");
    if (color_item && cJSON_IsArray(color_item) && cJSON_GetArraySize(color_item) >= 3) {
        mr_data.albedo_color[0] = (float)cJSON_GetArrayItem(color_item, 0)->valuedouble;
        mr_data.albedo_color[1] = (float)cJSON_GetArrayItem(color_item, 1)->valuedouble;
        mr_data.albedo_color[2] = (float)cJSON_GetArrayItem(color_item, 2)->valuedouble;
        if (cJSON_GetArraySize(color_item) >= 4)
            mr_data.albedo_color[3] = (float)cJSON_GetArrayItem(color_item, 3)->valuedouble;
    }

    /* Parse optional "roughness" and "metallic" */
    const cJSON* rough_item = cJSON_GetObjectItemCaseSensitive(params, "roughness");
    if (rough_item && cJSON_IsNumber(rough_item))
        mr_data.roughness = (float)rough_item->valuedouble;

    const cJSON* metal_item = cJSON_GetObjectItemCaseSensitive(params, "metallic");
    if (metal_item && cJSON_IsNumber(metal_item))
        mr_data.metallic = (float)metal_item->valuedouble;

    /* Execute CD_CMD_COMPONENT_ADD via command queue */
    cd_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CD_CMD_COMPONENT_ADD;
    cmd.target = node_id;
    cmd.payload.component_add.type_id = mr_type_id;
    cmd.payload.component_add.data = &mr_data;
    cmd.payload.component_add.data_size = (uint32_t)sizeof(mr_data);

    cd_result_t res = cd_command_execute_sync(
        cd_kernel_get_commands(kernel), &cmd, cd_kernel_get_scene(kernel), cd_kernel_get_events(kernel));
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to add MeshRenderer component";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    return result;
}

/* ============================================================================
 * mesh.set_material handler
 *
 * Input:  { "id": "2:3", "color": [r,g,b,a], "roughness": 0.8,
 *           "metallic": 1.0 }
 * Output: { "status": "ok" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_mesh_set_material(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    if (!kernel || !cd_kernel_get_scene(kernel)) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("No active scene",
                          "mesh.set_material requires a scene.",
                          "Call scene.create3d or scene.open first.");
        return NULL;
    }

    /* Parse required "id" */
    const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (!id_item || !cJSON_IsString(id_item) || !id_item->valuestring) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing required parameter: id";
        return NULL;
    }
    cd_id_t node_id = cd_id_parse(id_item->valuestring);
    if (!cd_id_is_valid(node_id)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Invalid node id format";
        return NULL;
    }

    /* Look up MeshRenderer type and find component on node */
    cd_id_t mr_type_id = find_mesh_renderer_type(kernel);
    if (!cd_id_is_valid(mr_type_id)) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "MeshRenderer type not registered";
        return NULL;
    }

    cd_mesh_renderer_data_t* mr = (cd_mesh_renderer_data_t*)
        cd_node_get_component(cd_kernel_get_scene(kernel), node_id, mr_type_id);
    if (!mr) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("No MeshRenderer on node",
                          "Node does not have a MeshRenderer component.",
                          "Call mesh.attach_primitive first.");
        return NULL;
    }

    /* Apply optional "color" directly (component data is writable) */
    const cJSON* color_item = cJSON_GetObjectItemCaseSensitive(params, "color");
    if (color_item && cJSON_IsArray(color_item) && cJSON_GetArraySize(color_item) >= 3) {
        mr->albedo_color[0] = (float)cJSON_GetArrayItem(color_item, 0)->valuedouble;
        mr->albedo_color[1] = (float)cJSON_GetArrayItem(color_item, 1)->valuedouble;
        mr->albedo_color[2] = (float)cJSON_GetArrayItem(color_item, 2)->valuedouble;
        if (cJSON_GetArraySize(color_item) >= 4)
            mr->albedo_color[3] = (float)cJSON_GetArrayItem(color_item, 3)->valuedouble;
    }

    const cJSON* rough_item = cJSON_GetObjectItemCaseSensitive(params, "roughness");
    if (rough_item && cJSON_IsNumber(rough_item))
        mr->roughness = (float)rough_item->valuedouble;

    const cJSON* metal_item = cJSON_GetObjectItemCaseSensitive(params, "metallic");
    if (metal_item && cJSON_IsNumber(metal_item))
        mr->metallic = (float)metal_item->valuedouble;

    /* Apply optional "primitive" to change shape */
    const cJSON* prim_item = cJSON_GetObjectItemCaseSensitive(params, "primitive");
    if (prim_item && cJSON_IsString(prim_item) && prim_item->valuestring)
        mr->primitive_type = primitive_name_to_type(prim_item->valuestring);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    return result;
}

/* ============================================================================
 * mesh.remove handler
 *
 * Input:  { "id": "2:3" }
 * Output: { "status": "ok" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_mesh_remove(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    if (!kernel || !cd_kernel_get_scene(kernel)) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("No active scene",
                          "mesh.remove requires a scene.",
                          "Call scene.create3d or scene.open first.");
        return NULL;
    }

    /* Parse required "id" */
    const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (!id_item || !cJSON_IsString(id_item) || !id_item->valuestring) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing required parameter: id";
        return NULL;
    }
    cd_id_t node_id = cd_id_parse(id_item->valuestring);
    if (!cd_id_is_valid(node_id)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Invalid node id format";
        return NULL;
    }

    cd_id_t mr_type_id = find_mesh_renderer_type(kernel);
    if (!cd_id_is_valid(mr_type_id)) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "MeshRenderer type not registered";
        return NULL;
    }

    /* Execute CD_CMD_COMPONENT_REMOVE */
    cd_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CD_CMD_COMPONENT_REMOVE;
    cmd.target = node_id;
    cmd.payload.component_remove.type_id = mr_type_id;

    cd_result_t res = cd_command_execute_sync(
        cd_kernel_get_commands(kernel), &cmd, cd_kernel_get_scene(kernel), cd_kernel_get_events(kernel));
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to remove MeshRenderer component";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    return result;
}

/* ============================================================================
 * mesh.import_gltf handler
 *
 * Import a glTF/GLB file into the live scene as child nodes.
 *
 * Input:
 *   { "path": "/absolute/path/to/model.glb",
 *     "parent": "1:1" }         (optional, defaults to root)
 *
 * Output:
 *   { "status": "ok", "root_node_id": "5:1",
 *     "node_count": 3, "mesh_count": 1, "material_count": 1 }
 * ============================================================================ */

static cJSON* cd_mcp_handle_mesh_import_gltf(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    if (!kernel || !cd_kernel_get_scene(kernel)) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = "No scene loaded";
        return NULL;
    }

    const cJSON* j_path = cJSON_GetObjectItemCaseSensitive(params, "path");
    if (!j_path || !cJSON_IsString(j_path) || !j_path->valuestring[0]) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = "Missing required param: path (absolute path to .glb/.gltf)";
        return NULL;
    }

    /* Optional parent node */
    cd_id_t parent = cd_kernel_get_scene(kernel)->root;
    const cJSON* j_parent = cJSON_GetObjectItemCaseSensitive(params, "parent");
    if (j_parent && cJSON_IsString(j_parent)) {
        cd_id_t parsed = cd_id_parse(j_parent->valuestring);
        if (cd_id_is_valid(parsed)) parent = parsed;
    }

    /* Import glTF */
    cd_gltf_scene_t gltf_scene;
    memset(&gltf_scene, 0, sizeof(gltf_scene));
    cd_result_t res = cd_gltf_import_file(&gltf_scene, j_path->valuestring);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = cd_mcp_error_fmt("glTF import failed",
            "Could not parse the glTF file.",
            "Ensure the file is valid glTF 2.0 (.glb or .gltf).");
        return NULL;
    }

    /* Instantiate into scene */
    cd_id_t root_id = cd_gltf_instantiate(&gltf_scene, cd_kernel_get_scene(kernel),
                                           parent, cd_kernel_get_types(kernel));

    /* Build result */
    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");

    char id_buf[32];
    snprintf(id_buf, sizeof(id_buf), "%u:%u",
             cd_id_index(root_id), cd_id_generation(root_id));
    cJSON_AddStringToObject(result, "root_node_id", id_buf);
    cJSON_AddNumberToObject(result, "node_count", gltf_scene.node_count);
    cJSON_AddNumberToObject(result, "mesh_count", gltf_scene.mesh_count);
    cJSON_AddNumberToObject(result, "material_count", gltf_scene.material_count);
    cJSON_AddNumberToObject(result, "animation_count", gltf_scene.animation_count);

    cd_gltf_scene_free(&gltf_scene);
    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_mesh_tools(cd_mcp_server_t* server) {
    if (!server) return CD_ERR_NULL;

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "mesh.attach_primitive",
        cd_mcp_handle_mesh_attach_primitive,
        "Attach a primitive mesh (cube/sphere/plane/cylinder) with material to a node",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\",\"description\":\"Node ID (index:generation)\"},"
        "\"primitive\":{\"type\":\"string\",\"enum\":[\"cube\",\"sphere\",\"plane\",\"cylinder\"],\"description\":\"Primitive shape, defaults to cube\"},"
        "\"color\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":3,\"maxItems\":4,\"description\":\"RGBA color [r,g,b,a], defaults to white\"},"
        "\"roughness\":{\"type\":\"number\",\"description\":\"PBR roughness 0-1, defaults to 0.5\"},"
        "\"metallic\":{\"type\":\"number\",\"description\":\"PBR metallic 0-1, defaults to 0.0\"},"
        "\"mesh_uri\":{\"type\":\"string\",\"description\":\"res:// URI to a .glb/.gltf/.cdmesh file (overrides primitive)\"}"
        "},\"required\":[\"id\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "mesh.set_material",
        cd_mcp_handle_mesh_set_material,
        "Update material properties (color, roughness, metallic) on a node's MeshRenderer",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\",\"description\":\"Node ID (index:generation)\"},"
        "\"color\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":3,\"maxItems\":4,\"description\":\"RGBA color [r,g,b,a]\"},"
        "\"roughness\":{\"type\":\"number\",\"description\":\"PBR roughness 0-1\"},"
        "\"metallic\":{\"type\":\"number\",\"description\":\"PBR metallic 0-1\"},"
        "\"primitive\":{\"type\":\"string\",\"enum\":[\"cube\",\"sphere\",\"plane\",\"cylinder\"],\"description\":\"Change primitive shape\"}"
        "},\"required\":[\"id\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "mesh.import_gltf",
        cd_mcp_handle_mesh_import_gltf,
        "Import a glTF/GLB file into the live scene as child nodes",
        "{\"type\":\"object\",\"properties\":{"
        "\"path\":{\"type\":\"string\",\"description\":\"Absolute path to the .glb/.gltf file\"},"
        "\"parent\":{\"type\":\"string\",\"description\":\"Parent node ID (index:generation), defaults to root\"}"
        "},\"required\":[\"path\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "mesh.remove",
        cd_mcp_handle_mesh_remove,
        "Remove the MeshRenderer component from a node",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\",\"description\":\"Node ID (index:generation)\"}"
        "},\"required\":[\"id\"]}");
    if (res != CD_OK) return res;

    return CD_OK;
}
