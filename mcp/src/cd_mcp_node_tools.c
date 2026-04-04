/* cd_mcp_node_tools.c - Cadence Engine MCP node tool handlers
 *
 * Implements:
 *   - node.create       : Create a new node in the scene
 *   - node.delete       : Delete a node and its children
 *   - node.get          : Get full node properties
 *   - node.find         : Find nodes by name glob and/or tags
 *   - node.setTransform : Set local/world transform with partial updates
 *   - node.setParent    : Reparent a node under a new parent
 *
 * All handlers follow the cd_mcp_tool_handler_t signature.
 * Node-mutating operations work on cd_kernel_get_scene(kernel).
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_error.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_scene.h"
#include "cadence/cd_commands.h"
#include "cadence/cd_error.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Internal: Format a cd_id_t as "index:generation" string
 *
 * Writes into the provided buffer and returns a pointer to it.
 * Buffer must be at least 24 bytes (two uint32 decimals + colon + NUL).
 * ============================================================================ */

#define CD_ID_STR_BUF_SIZE 24

static const char* cd_id_format(cd_id_t id, char* buf, size_t buf_size) {
    uint32_t index = cd_id_index(id);
    uint32_t gen   = cd_id_generation(id);
    snprintf(buf, buf_size, "%u:%u", index, gen);
    return buf;
}

/* ============================================================================
 * Internal: Parse "index:generation" string to cd_id_t
 *
 * Returns CD_ID_INVALID on parse failure.
 * ============================================================================ */

static cd_id_t cd_id_parse(const char* str) {
    if (str == NULL || str[0] == '\0') {
        return CD_ID_INVALID;
    }
    unsigned int index = 0;
    unsigned int gen   = 0;
    int scanned = sscanf(str, "%u:%u", &index, &gen);
    if (scanned != 2) {
        return CD_ID_INVALID;
    }
    return cd_id_make((uint32_t)gen, (uint32_t)index);
}

/* ============================================================================
 * Internal: Build transform JSON object from cd_transform_t
 * ============================================================================ */

static cJSON* cd_transform_to_json(const cd_transform_t* t) {
    cJSON* obj = cJSON_CreateObject();
    if (obj == NULL) {
        return NULL;
    }

    /* position */
    cJSON* pos = cJSON_CreateObject();
    if (pos == NULL) { cJSON_Delete(obj); return NULL; }
    cJSON_AddNumberToObject(pos, "x", (double)t->position.x);
    cJSON_AddNumberToObject(pos, "y", (double)t->position.y);
    cJSON_AddNumberToObject(pos, "z", (double)t->position.z);
    cJSON_AddItemToObject(obj, "position", pos);

    /* rotation */
    cJSON* rot = cJSON_CreateObject();
    if (rot == NULL) { cJSON_Delete(obj); return NULL; }
    cJSON_AddNumberToObject(rot, "x", (double)t->rotation.x);
    cJSON_AddNumberToObject(rot, "y", (double)t->rotation.y);
    cJSON_AddNumberToObject(rot, "z", (double)t->rotation.z);
    cJSON_AddNumberToObject(rot, "w", (double)t->rotation.w);
    cJSON_AddItemToObject(obj, "rotation", rot);

    /* scale */
    cJSON* scl = cJSON_CreateObject();
    if (scl == NULL) { cJSON_Delete(obj); return NULL; }
    cJSON_AddNumberToObject(scl, "x", (double)t->scale.x);
    cJSON_AddNumberToObject(scl, "y", (double)t->scale.y);
    cJSON_AddNumberToObject(scl, "z", (double)t->scale.z);
    cJSON_AddItemToObject(obj, "scale", scl);

    return obj;
}

/* ============================================================================
 * Internal: Parse transform JSON object to cd_transform_t
 *
 * If transform_json is NULL, returns identity transform.
 * ============================================================================ */

static cd_transform_t cd_transform_from_json(const cJSON* transform_json) {
    cd_transform_t t = CD_TRANSFORM_IDENTITY;

    if (transform_json == NULL || !cJSON_IsObject(transform_json)) {
        return t;
    }

    /* position */
    const cJSON* pos = cJSON_GetObjectItemCaseSensitive(transform_json, "position");
    if (pos != NULL && cJSON_IsObject(pos)) {
        const cJSON* px = cJSON_GetObjectItemCaseSensitive(pos, "x");
        const cJSON* py = cJSON_GetObjectItemCaseSensitive(pos, "y");
        const cJSON* pz = cJSON_GetObjectItemCaseSensitive(pos, "z");
        if (px != NULL && cJSON_IsNumber(px)) t.position.x = (float)px->valuedouble;
        if (py != NULL && cJSON_IsNumber(py)) t.position.y = (float)py->valuedouble;
        if (pz != NULL && cJSON_IsNumber(pz)) t.position.z = (float)pz->valuedouble;
    }

    /* rotation */
    const cJSON* rot = cJSON_GetObjectItemCaseSensitive(transform_json, "rotation");
    if (rot != NULL && cJSON_IsObject(rot)) {
        const cJSON* rx = cJSON_GetObjectItemCaseSensitive(rot, "x");
        const cJSON* ry = cJSON_GetObjectItemCaseSensitive(rot, "y");
        const cJSON* rz = cJSON_GetObjectItemCaseSensitive(rot, "z");
        const cJSON* rw = cJSON_GetObjectItemCaseSensitive(rot, "w");
        if (rx != NULL && cJSON_IsNumber(rx)) t.rotation.x = (float)rx->valuedouble;
        if (ry != NULL && cJSON_IsNumber(ry)) t.rotation.y = (float)ry->valuedouble;
        if (rz != NULL && cJSON_IsNumber(rz)) t.rotation.z = (float)rz->valuedouble;
        if (rw != NULL && cJSON_IsNumber(rw)) t.rotation.w = (float)rw->valuedouble;
    }

    /* scale */
    const cJSON* scl = cJSON_GetObjectItemCaseSensitive(transform_json, "scale");
    if (scl != NULL && cJSON_IsObject(scl)) {
        const cJSON* sx = cJSON_GetObjectItemCaseSensitive(scl, "x");
        const cJSON* sy = cJSON_GetObjectItemCaseSensitive(scl, "y");
        const cJSON* sz = cJSON_GetObjectItemCaseSensitive(scl, "z");
        if (sx != NULL && cJSON_IsNumber(sx)) t.scale.x = (float)sx->valuedouble;
        if (sy != NULL && cJSON_IsNumber(sy)) t.scale.y = (float)sy->valuedouble;
        if (sz != NULL && cJSON_IsNumber(sz)) t.scale.z = (float)sz->valuedouble;
    }

    return t;
}

/* ============================================================================
 * node.create handler
 *
 * Input:  { "name": "Enemy_01", "nodeType": "MeshInstance",
 *           "parentId": "1:0",
 *           "transform": { "position": {...}, "rotation": {...}, "scale": {...} } }
 * Output: { "status": "ok", "id": "2:3" }
 *
 * Creates a new node in the active scene. name is required; parentId,
 * nodeType, and transform are optional.
 * ============================================================================ */

static cJSON* cd_mcp_handle_node_create(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    /* Validate kernel */
    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Kernel not available";
        return NULL;
    }

    /* Validate scene exists */
    if (cd_kernel_get_scene(kernel) == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("No active scene",
                          "node.create requires a scene to be loaded first.",
                          "Call scene.create3d or scene.open to load a scene.");
        return NULL;
    }

    /* Extract "name" (required) */
    const cJSON* name_item = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (name_item == NULL || !cJSON_IsString(name_item) ||
        name_item->valuestring == NULL || name_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Missing required parameter: name",
                          "node.create requires a 'name' string parameter.",
                          "Example: {\"name\": \"MyCube\", \"nodeType\": \"MeshInstance\"}");
        return NULL;
    }
    const char* node_name = name_item->valuestring;

    /* Extract "parentId" (optional, defaults to scene root) */
    cd_id_t parent_id = CD_ID_INVALID; /* Will default to root in cd_node_create */
    const cJSON* parent_item = cJSON_GetObjectItemCaseSensitive(params, "parentId");
    if (parent_item != NULL && cJSON_IsString(parent_item) &&
        parent_item->valuestring != NULL && parent_item->valuestring[0] != '\0') {
        parent_id = cd_id_parse(parent_item->valuestring);
        if (!cd_id_is_valid(parent_id)) {
            *error_code = CD_JSONRPC_INVALID_PARAMS;
            *error_msg  = cd_mcp_error_fmt(
                "Invalid parentId format",
                "parentId must be a string in 'index:generation' format (e.g. '1:0').",
                "Use node.find or node.get to discover valid node IDs.");
            return NULL;
        }
        /* Verify parent exists */
        cd_node_t* parent_node = cd_node_get(cd_kernel_get_scene(kernel), parent_id);
        if (parent_node == NULL) {
            char detail[128];
            snprintf(detail, sizeof(detail),
                     "Parent node with id '%s' does not exist in the scene.",
                     parent_item->valuestring);
            *error_code = CD_JSONRPC_INVALID_PARAMS;
            *error_msg  = cd_mcp_error_fmt("Parent node not found",
                              detail,
                              "Use node.find to list available nodes.");
            return NULL;
        }
    }

    /* Extract "nodeType" (optional, defaults to "Node3D") -- stored in node name
     * metadata; for now we store it as a convention. Since cd_node_t does not have
     * a dedicated type field, we will store nodeType as a tag prefixed with
     * "type:" so it can be retrieved later. This is a pragmatic approach until
     * a node type field is added. */
    const char* node_type = "Node3D";
    const cJSON* type_item = cJSON_GetObjectItemCaseSensitive(params, "nodeType");
    if (type_item != NULL && cJSON_IsString(type_item) &&
        type_item->valuestring != NULL && type_item->valuestring[0] != '\0') {
        node_type = type_item->valuestring;
    }

    /* Extract "transform" (optional, defaults to identity) */
    const cJSON* transform_item = cJSON_GetObjectItemCaseSensitive(params, "transform");
    cd_transform_t transform = cd_transform_from_json(transform_item);

    /* Create the node via command queue */
    cd_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CD_CMD_NODE_CREATE;
    cmd.target = CD_ID_INVALID;
    cmd.payload.node_create.name = cd_name_from_cstr(node_name);
    cmd.payload.node_create.parent = parent_id;

    cd_result_t res = cd_command_execute_sync(
        cd_kernel_get_commands(kernel), &cmd, cd_kernel_get_scene(kernel), cd_kernel_get_events(kernel));
    if (res != CD_OK || !cd_id_is_valid(cmd.payload.node_create.created_id)) {
        const cd_error_context_t* err = cd_get_last_error();
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = (err && err->message) ? err->message : "Failed to create node";
        return NULL;
    }
    cd_id_t new_id = cmd.payload.node_create.created_id;

    /* Set transform via command queue */
    cd_command_t tcmd;
    memset(&tcmd, 0, sizeof(tcmd));
    tcmd.type = CD_CMD_SET_TRANSFORM;
    tcmd.target = new_id;
    tcmd.payload.set_transform.transform = transform;
    cd_command_execute_sync(
        cd_kernel_get_commands(kernel), &tcmd, cd_kernel_get_scene(kernel), cd_kernel_get_events(kernel));

    /* Store nodeType as a "type:XXX" tag via command queue */
    char type_tag[80];
    snprintf(type_tag, sizeof(type_tag), "type:%s", node_type);
    cd_command_t tag_cmd;
    memset(&tag_cmd, 0, sizeof(tag_cmd));
    tag_cmd.type = CD_CMD_TAG_ADD;
    tag_cmd.target = new_id;
    tag_cmd.payload.tag_op.tag = cd_name_from_cstr(type_tag);
    cd_command_execute_sync(
        cd_kernel_get_commands(kernel), &tag_cmd, cd_kernel_get_scene(kernel), cd_kernel_get_events(kernel));

    /* Build the response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");

    char id_buf[CD_ID_STR_BUF_SIZE];
    cd_id_format(new_id, id_buf, sizeof(id_buf));
    cJSON_AddStringToObject(result, "id", id_buf);

    return result;
}

/* ============================================================================
 * node.delete handler
 *
 * Input:  { "id": "2:3" }
 * Output: { "status": "ok" }
 *
 * Deletes node and all children recursively.
 * ============================================================================ */

static cJSON* cd_mcp_handle_node_delete(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    /* Validate kernel */
    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Kernel not available";
        return NULL;
    }

    /* Validate scene exists */
    if (cd_kernel_get_scene(kernel) == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("No active scene",
                          "node.delete requires a scene to be loaded.",
                          "Call scene.create3d or scene.open to load a scene.");
        return NULL;
    }

    /* Extract "id" (required) */
    const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (id_item == NULL || !cJSON_IsString(id_item) ||
        id_item->valuestring == NULL || id_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Missing required parameter: id",
                          "node.delete requires an 'id' string in 'index:generation' format.",
                          "Use node.find to discover node IDs.");
        return NULL;
    }

    cd_id_t node_id = cd_id_parse(id_item->valuestring);
    if (!cd_id_is_valid(node_id)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Invalid id format",
                          "id must be a string in 'index:generation' format (e.g. '2:3').",
                          "Use node.find to discover valid node IDs.");
        return NULL;
    }

    /* Delete the node via command queue */
    cd_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CD_CMD_NODE_DELETE;
    cmd.target = node_id;

    cd_result_t res = cd_command_execute_sync(
        cd_kernel_get_commands(kernel), &cmd, cd_kernel_get_scene(kernel), cd_kernel_get_events(kernel));
    if (res == CD_ERR_INVALID) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Cannot delete root node",
                          "The scene root node cannot be deleted.",
                          "Delete child nodes instead, or create a new scene.");
        return NULL;
    }
    if (res == CD_ERR_NOTFOUND) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "Node with id '%s' does not exist in the scene.",
                 id_item->valuestring);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Node not found", detail,
                          "Use node.find to list available nodes.");
        return NULL;
    }
    if (res != CD_OK) {
        const cd_error_context_t* err = cd_get_last_error();
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = (err && err->message) ? err->message : "Failed to delete node";
        return NULL;
    }

    /* Build the response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    return result;
}

/* ============================================================================
 * Internal: Extract nodeType from a node's tags
 *
 * Looks for a tag prefixed with "type:" and returns the suffix.
 * Returns "Node3D" if no type tag is found.
 * ============================================================================ */

static const char* cd_node_get_type(cd_node_t* node) {
    for (uint32_t i = 0; i < node->tag_count; i++) {
        if (strncmp(node->tag_names[i].buf, "type:", 5) == 0) {
            return node->tag_names[i].buf + 5;
        }
    }
    return "Node3D";
}

/* ============================================================================
 * node.get handler
 *
 * Input:  { "id": "2:3" }
 * Output: { "id": "2:3", "name": "Enemy_01", "type": "MeshInstance",
 *           "parent": "1:0", "children": [], "enabled": true,
 *           "tags": ["enemy"], "transform": {...},
 *           "components": {}, "scripts": [] }
 * ============================================================================ */

static cJSON* cd_mcp_handle_node_get(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    /* Validate kernel */
    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Kernel not available";
        return NULL;
    }

    /* Validate scene exists */
    if (cd_kernel_get_scene(kernel) == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("No active scene",
                          "node.get requires a scene to be loaded.",
                          "Call scene.create3d or scene.open to load a scene.");
        return NULL;
    }

    /* Extract "id" (required) */
    const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (id_item == NULL || !cJSON_IsString(id_item) ||
        id_item->valuestring == NULL || id_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Missing required parameter: id",
                          "node.get requires an 'id' string in 'index:generation' format.",
                          "Use node.find to discover node IDs.");
        return NULL;
    }

    cd_id_t node_id = cd_id_parse(id_item->valuestring);
    if (!cd_id_is_valid(node_id)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Invalid id format",
                          "id must be a string in 'index:generation' format (e.g. '2:3').",
                          "Use node.find to discover valid node IDs.");
        return NULL;
    }

    /* Look up node */
    cd_node_t* node = cd_node_get(cd_kernel_get_scene(kernel), node_id);
    if (node == NULL) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "Node with id '%s' does not exist in the scene.",
                 id_item->valuestring);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Node not found", detail,
                          "Use node.find to list available nodes.");
        return NULL;
    }

    /* Build the response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    char id_buf[CD_ID_STR_BUF_SIZE];

    /* id */
    cd_id_format(node->id, id_buf, sizeof(id_buf));
    cJSON_AddStringToObject(result, "id", id_buf);

    /* name */
    cJSON_AddStringToObject(result, "name", node->name.buf);

    /* type (extracted from type: tag) */
    cJSON_AddStringToObject(result, "type", cd_node_get_type(node));

    /* parent */
    if (cd_id_is_valid(node->parent)) {
        cd_id_format(node->parent, id_buf, sizeof(id_buf));
        cJSON_AddStringToObject(result, "parent", id_buf);
    } else {
        cJSON_AddNullToObject(result, "parent");
    }

    /* children */
    cJSON* children = cJSON_CreateArray();
    if (children != NULL) {
        for (uint32_t i = 0; i < node->child_count; i++) {
            cd_id_format(node->children[i], id_buf, sizeof(id_buf));
            cJSON_AddItemToArray(children, cJSON_CreateString(id_buf));
        }
    }
    cJSON_AddItemToObject(result, "children", children);

    /* enabled */
    cJSON_AddBoolToObject(result, "enabled", node->enabled);

    /* tags (exclude type: prefixed tags) */
    cJSON* tags = cJSON_CreateArray();
    if (tags != NULL) {
        for (uint32_t i = 0; i < node->tag_count; i++) {
            if (strncmp(node->tag_names[i].buf, "type:", 5) != 0) {
                cJSON_AddItemToArray(tags, cJSON_CreateString(node->tag_names[i].buf));
            }
        }
    }
    cJSON_AddItemToObject(result, "tags", tags);

    /* transform */
    cJSON* transform = cd_transform_to_json(&node->local_transform);
    if (transform != NULL) {
        cJSON_AddItemToObject(result, "transform", transform);
    }

    /* components (empty object for now -- component subsystem comes later) */
    cJSON_AddItemToObject(result, "components", cJSON_CreateObject());

    /* scripts (empty array for now -- script subsystem comes later) */
    cJSON_AddItemToObject(result, "scripts", cJSON_CreateArray());

    return result;
}

/* ============================================================================
 * node.find handler
 *
 * Input:  { "nameGlob": "Enemy*", "nodeTypes": ["MeshInstance"],
 *           "tags": ["damageable"] }
 * Output: { "nodes": [ { "id": "2:3", "name": "Enemy_01",
 *            "type": "MeshInstance" }, ... ] }
 *
 * All input fields are optional and combined with AND logic.
 * ============================================================================ */

static cJSON* cd_mcp_handle_node_find(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    /* Validate kernel */
    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Kernel not available";
        return NULL;
    }

    /* Validate scene exists */
    if (cd_kernel_get_scene(kernel) == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("No active scene",
                          "node.find requires a scene to be loaded.",
                          "Call scene.create3d or scene.open to load a scene.");
        return NULL;
    }

    /* Extract optional filters */
    const cJSON* name_glob_item = NULL;
    const cJSON* node_types_item = NULL;
    const cJSON* tags_item = NULL;

    if (params != NULL && cJSON_IsObject(params)) {
        name_glob_item  = cJSON_GetObjectItemCaseSensitive(params, "nameGlob");
        node_types_item = cJSON_GetObjectItemCaseSensitive(params, "nodeTypes");
        tags_item       = cJSON_GetObjectItemCaseSensitive(params, "tags");
    }

    const char* name_glob = NULL;
    if (name_glob_item != NULL && cJSON_IsString(name_glob_item) &&
        name_glob_item->valuestring != NULL) {
        name_glob = name_glob_item->valuestring;
    }

    /* Determine candidate set: either from glob match or all nodes */
    cd_id_t candidates[CD_SCENE_MAX_QUERY_RESULTS];
    uint32_t candidate_count = 0;

    if (name_glob != NULL) {
        /* Use glob matching */
        cd_scene_find_by_name_glob(cd_kernel_get_scene(kernel), name_glob,
                                   candidates, &candidate_count,
                                   CD_SCENE_MAX_QUERY_RESULTS);
    } else {
        /* All live nodes -- iterate through the gen array */
        cd_scene_t* scene = cd_kernel_get_scene(kernel);
        for (uint32_t i = 0; i < scene->nodes.capacity; i++) {
            uint32_t gen = scene->nodes.generations[i];
            if (gen == 0) {
                continue;
            }
            cd_id_t id = cd_id_make(gen, i);
            cd_node_t* node = cd_node_get(scene, id);
            if (node != NULL && candidate_count < CD_SCENE_MAX_QUERY_RESULTS) {
                candidates[candidate_count++] = node->id;
            }
        }
    }

    /* Filter by tags (AND logic: node must have ALL specified tags) */
    if (tags_item != NULL && cJSON_IsArray(tags_item)) {
        uint32_t filtered_count = 0;
        for (uint32_t i = 0; i < candidate_count; i++) {
            bool has_all_tags = true;
            const cJSON* tag = NULL;
            cJSON_ArrayForEach(tag, tags_item) {
                if (cJSON_IsString(tag) && tag->valuestring != NULL) {
                    if (!cd_node_has_tag(cd_kernel_get_scene(kernel), candidates[i], tag->valuestring)) {
                        has_all_tags = false;
                        break;
                    }
                }
            }
            if (has_all_tags) {
                candidates[filtered_count++] = candidates[i];
            }
        }
        candidate_count = filtered_count;
    }

    /* Filter by nodeTypes (AND with OR within types: node must be one of them) */
    if (node_types_item != NULL && cJSON_IsArray(node_types_item) &&
        cJSON_GetArraySize(node_types_item) > 0) {
        uint32_t filtered_count = 0;
        for (uint32_t i = 0; i < candidate_count; i++) {
            cd_node_t* node = cd_node_get(cd_kernel_get_scene(kernel), candidates[i]);
            if (node == NULL) continue;

            const char* node_type = cd_node_get_type(node);
            bool type_match = false;
            const cJSON* type_item = NULL;
            cJSON_ArrayForEach(type_item, node_types_item) {
                if (cJSON_IsString(type_item) && type_item->valuestring != NULL) {
                    if (strcmp(node_type, type_item->valuestring) == 0) {
                        type_match = true;
                        break;
                    }
                }
            }
            if (type_match) {
                candidates[filtered_count++] = candidates[i];
            }
        }
        candidate_count = filtered_count;
    }

    /* Build the response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON* nodes_array = cJSON_CreateArray();
    if (nodes_array == NULL) {
        cJSON_Delete(result);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON array";
        return NULL;
    }

    char id_buf[CD_ID_STR_BUF_SIZE];
    for (uint32_t i = 0; i < candidate_count; i++) {
        cd_node_t* node = cd_node_get(cd_kernel_get_scene(kernel), candidates[i]);
        if (node == NULL) continue;

        cJSON* entry = cJSON_CreateObject();
        if (entry == NULL) continue;

        cd_id_format(node->id, id_buf, sizeof(id_buf));
        cJSON_AddStringToObject(entry, "id", id_buf);
        cJSON_AddStringToObject(entry, "name", node->name.buf);
        cJSON_AddStringToObject(entry, "type", cd_node_get_type(node));

        cJSON_AddItemToArray(nodes_array, entry);
    }

    cJSON_AddItemToObject(result, "nodes", nodes_array);
    return result;
}

/* ============================================================================
 * Internal: Overlay transform JSON onto an existing cd_transform_t
 *
 * Reads only the fields present in transform_json and overwrites them in
 * the given transform. Fields not present in the JSON are left unchanged.
 * This supports partial updates (e.g., only position.x changes).
 * ============================================================================ */

static void cd_transform_overlay_json(cd_transform_t* t, const cJSON* transform_json) {
    if (transform_json == NULL || !cJSON_IsObject(transform_json)) {
        return;
    }

    /* position */
    const cJSON* pos = cJSON_GetObjectItemCaseSensitive(transform_json, "position");
    if (pos != NULL && cJSON_IsObject(pos)) {
        const cJSON* px = cJSON_GetObjectItemCaseSensitive(pos, "x");
        const cJSON* py = cJSON_GetObjectItemCaseSensitive(pos, "y");
        const cJSON* pz = cJSON_GetObjectItemCaseSensitive(pos, "z");
        if (px != NULL && cJSON_IsNumber(px)) t->position.x = (float)px->valuedouble;
        if (py != NULL && cJSON_IsNumber(py)) t->position.y = (float)py->valuedouble;
        if (pz != NULL && cJSON_IsNumber(pz)) t->position.z = (float)pz->valuedouble;
    }

    /* rotation */
    const cJSON* rot = cJSON_GetObjectItemCaseSensitive(transform_json, "rotation");
    if (rot != NULL && cJSON_IsObject(rot)) {
        const cJSON* rx = cJSON_GetObjectItemCaseSensitive(rot, "x");
        const cJSON* ry = cJSON_GetObjectItemCaseSensitive(rot, "y");
        const cJSON* rz = cJSON_GetObjectItemCaseSensitive(rot, "z");
        const cJSON* rw = cJSON_GetObjectItemCaseSensitive(rot, "w");
        if (rx != NULL && cJSON_IsNumber(rx)) t->rotation.x = (float)rx->valuedouble;
        if (ry != NULL && cJSON_IsNumber(ry)) t->rotation.y = (float)ry->valuedouble;
        if (rz != NULL && cJSON_IsNumber(rz)) t->rotation.z = (float)rz->valuedouble;
        if (rw != NULL && cJSON_IsNumber(rw)) t->rotation.w = (float)rw->valuedouble;
    }

    /* scale */
    const cJSON* scl = cJSON_GetObjectItemCaseSensitive(transform_json, "scale");
    if (scl != NULL && cJSON_IsObject(scl)) {
        const cJSON* sx = cJSON_GetObjectItemCaseSensitive(scl, "x");
        const cJSON* sy = cJSON_GetObjectItemCaseSensitive(scl, "y");
        const cJSON* sz = cJSON_GetObjectItemCaseSensitive(scl, "z");
        if (sx != NULL && cJSON_IsNumber(sx)) t->scale.x = (float)sx->valuedouble;
        if (sy != NULL && cJSON_IsNumber(sy)) t->scale.y = (float)sy->valuedouble;
        if (sz != NULL && cJSON_IsNumber(sz)) t->scale.z = (float)sz->valuedouble;
    }
}

/* ============================================================================
 * node.setTransform handler
 *
 * Input:  { "id": "2:3",
 *           "transform": { "position": { "x": 10.0 } },
 *           "space": "local" }
 * Output: { "status": "ok" }
 *
 * Sets the node's transform with partial updates. Only fields present in the
 * transform JSON are changed; omitted fields keep their current values.
 *
 * The "space" parameter is optional and defaults to "local". When set to
 * "world", the transform is applied as-is for nodes without a parent (or
 * parented under root). Full world-to-local inverse computation is reserved
 * for a future enhancement.
 * ============================================================================ */

static cJSON* cd_mcp_handle_node_set_transform(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    /* Validate kernel */
    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Kernel not available";
        return NULL;
    }

    /* Validate scene exists */
    if (cd_kernel_get_scene(kernel) == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("No active scene",
                          "node.setTransform requires a scene to be loaded.",
                          "Call scene.create3d or scene.open to load a scene.");
        return NULL;
    }

    /* Extract "id" (required) */
    const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (id_item == NULL || !cJSON_IsString(id_item) ||
        id_item->valuestring == NULL || id_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Missing required parameter: id",
                          "node.setTransform requires an 'id' string.",
                          "Use node.find to discover node IDs.");
        return NULL;
    }

    cd_id_t node_id = cd_id_parse(id_item->valuestring);
    if (!cd_id_is_valid(node_id)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Invalid id format",
                          "id must be a string in 'index:generation' format (e.g. '2:3').",
                          "Use node.find to discover valid node IDs.");
        return NULL;
    }

    /* Extract "transform" (required) */
    const cJSON* transform_item = cJSON_GetObjectItemCaseSensitive(params, "transform");
    if (transform_item == NULL || !cJSON_IsObject(transform_item)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Missing required parameter: transform",
                          "transform must be an object with optional position, rotation, scale.",
                          "Example: {\"transform\": {\"position\": {\"x\": 1, \"y\": 2, \"z\": 3}}}");
        return NULL;
    }

    /* Extract "space" (optional, defaults to "local") */
    const char* space = "local";
    const cJSON* space_item = cJSON_GetObjectItemCaseSensitive(params, "space");
    if (space_item != NULL && cJSON_IsString(space_item) &&
        space_item->valuestring != NULL) {
        space = space_item->valuestring;
    }

    /* Validate space value */
    if (strcmp(space, "local") != 0 && strcmp(space, "world") != 0) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "Got space='%s', but only 'local' and 'world' are valid.", space);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Invalid space value", detail, NULL);
        return NULL;
    }

    /* Look up the node */
    cd_node_t* node = cd_node_get(cd_kernel_get_scene(kernel), node_id);
    if (node == NULL) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "Node with id '%s' does not exist in the scene.",
                 id_item->valuestring);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Node not found", detail,
                          "Use node.find to list available nodes.");
        return NULL;
    }

    /* Read the current local transform and overlay provided fields */
    cd_transform_t t = node->local_transform;
    cd_transform_overlay_json(&t, transform_item);

    /*
     * For space "world": ideally we would compute the inverse parent world
     * transform and derive the local transform. For now, we apply the
     * transform directly as a local transform. This is correct when the node
     * has no parent or its parent is the root with identity transform.
     * Full world-space support is a future enhancement.
     */

    /* Apply the updated transform via command queue */
    cd_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CD_CMD_SET_TRANSFORM;
    cmd.target = node_id;
    cmd.payload.set_transform.transform = t;

    cd_result_t res = cd_command_execute_sync(
        cd_kernel_get_commands(kernel), &cmd, cd_kernel_get_scene(kernel), cd_kernel_get_events(kernel));
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to set transform";
        return NULL;
    }

    /* Build the response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    return result;
}

/* ============================================================================
 * node.setParent handler
 *
 * Input:  { "id": "2:3", "newParentId": "1:5",
 *           "preserveWorldTransform": true }
 * Output: { "status": "ok" }
 *
 * Reparents a node under a new parent. If preserveWorldTransform is true,
 * adjusts the local transform so the world position stays the same.
 * ============================================================================ */

static cJSON* cd_mcp_handle_node_set_parent(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    /* Validate kernel */
    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Kernel not available";
        return NULL;
    }

    /* Validate scene exists */
    if (cd_kernel_get_scene(kernel) == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("No active scene",
                          "node.setParent requires a scene to be loaded.",
                          "Call scene.create3d or scene.open to load a scene.");
        return NULL;
    }

    /* Extract "id" (required) */
    const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (id_item == NULL || !cJSON_IsString(id_item) ||
        id_item->valuestring == NULL || id_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Missing required parameter: id",
                          "node.setParent requires an 'id' string.",
                          "Use node.find to discover node IDs.");
        return NULL;
    }

    cd_id_t node_id = cd_id_parse(id_item->valuestring);
    if (!cd_id_is_valid(node_id)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Invalid id format",
                          "id must be a string in 'index:generation' format (e.g. '2:3').",
                          "Use node.find to discover valid node IDs.");
        return NULL;
    }

    /* Extract "newParentId" (required) */
    const cJSON* parent_item = cJSON_GetObjectItemCaseSensitive(params, "newParentId");
    if (parent_item == NULL || !cJSON_IsString(parent_item) ||
        parent_item->valuestring == NULL || parent_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Missing required parameter: newParentId",
                          "node.setParent requires a 'newParentId' string.",
                          "Use node.find to discover node IDs.");
        return NULL;
    }

    cd_id_t new_parent_id = cd_id_parse(parent_item->valuestring);
    if (!cd_id_is_valid(new_parent_id)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Invalid newParentId format",
                          "newParentId must be in 'index:generation' format (e.g. '1:0').",
                          "Use node.find to discover valid node IDs.");
        return NULL;
    }

    /* Extract "preserveWorldTransform" (optional, defaults to false) */
    bool preserve = false;
    const cJSON* preserve_item = cJSON_GetObjectItemCaseSensitive(params, "preserveWorldTransform");
    if (preserve_item != NULL && cJSON_IsBool(preserve_item)) {
        preserve = cJSON_IsTrue(preserve_item);
    }

    /* Verify both nodes exist before calling set_parent */
    cd_node_t* node = cd_node_get(cd_kernel_get_scene(kernel), node_id);
    if (node == NULL) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "Node with id '%s' does not exist.", id_item->valuestring);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Node not found", detail,
                          "Use node.find to list available nodes.");
        return NULL;
    }

    cd_node_t* new_parent = cd_node_get(cd_kernel_get_scene(kernel), new_parent_id);
    if (new_parent == NULL) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "New parent node with id '%s' does not exist.",
                 parent_item->valuestring);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("New parent node not found", detail,
                          "Use node.find to list available nodes.");
        return NULL;
    }

    /* Reparent the node via command queue */
    cd_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CD_CMD_NODE_SET_PARENT;
    cmd.target = node_id;
    cmd.payload.set_parent.new_parent = new_parent_id;
    cmd.payload.set_parent.preserve = preserve;

    cd_result_t res = cd_command_execute_sync(
        cd_kernel_get_commands(kernel), &cmd, cd_kernel_get_scene(kernel), cd_kernel_get_events(kernel));
    if (res == CD_ERR_INVALID) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Invalid reparent operation (cannot reparent root or create cycle)";
        return NULL;
    }
    if (res == CD_ERR_NOTFOUND) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Node or parent not found";
        return NULL;
    }
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to reparent node";
        return NULL;
    }

    /* Build the response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    return result;
}

/* ============================================================================
 * node.setPersistent handler (P1-4)
 *
 * Input:  { "id": "2:1", "persistent": true }
 * Output: { "status": "ok" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_node_set_persistent(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    if (kernel == NULL || cd_kernel_get_scene(kernel) == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "No active scene";
        return NULL;
    }

    const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (id_item == NULL || !cJSON_IsString(id_item)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing required parameter: id";
        return NULL;
    }

    cd_id_t node_id = cd_id_parse(id_item->valuestring);
    if (!cd_id_is_valid(node_id)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Invalid node id";
        return NULL;
    }

    const cJSON* persistent_item = cJSON_GetObjectItemCaseSensitive(params, "persistent");
    bool persistent = true;
    if (persistent_item != NULL) {
        persistent = cJSON_IsTrue(persistent_item) ? true : false;
    }

    /* Execute via command for undo support */
    cd_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CD_CMD_SET_PERSISTENT;
    cmd.target = node_id;
    cmd.payload.set_persistent.persistent = persistent;

    cd_result_t res = cd_command_execute_sync(cd_kernel_get_commands(kernel), &cmd,
                                               cd_kernel_get_scene(kernel), cd_kernel_get_events(kernel));
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = (res == CD_ERR_NOTFOUND) ? "Node not found" : "Failed to set persistent";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    return result;
}

/* ============================================================================
 * node.getPersistent handler (P1-4)
 *
 * Input:  { "id": "2:1" }
 * Output: { "persistent": true }
 * ============================================================================ */

static cJSON* cd_mcp_handle_node_get_persistent(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    if (kernel == NULL || cd_kernel_get_scene(kernel) == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "No active scene";
        return NULL;
    }

    const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (id_item == NULL || !cJSON_IsString(id_item)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing required parameter: id";
        return NULL;
    }

    cd_id_t node_id = cd_id_parse(id_item->valuestring);
    if (!cd_id_is_valid(node_id)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Invalid node id";
        return NULL;
    }

    cd_node_t* node = cd_node_get(cd_kernel_get_scene(kernel), node_id);
    if (node == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Node not found";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddBoolToObject(result, "persistent", node->persistent);
    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_node_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "node.create",
        cd_mcp_handle_node_create,
        "Create a new node in the scene",
        "{\"type\":\"object\",\"properties\":{"
        "\"name\":{\"type\":\"string\",\"description\":\"Node name\"},"
        "\"parentId\":{\"type\":\"string\",\"description\":\"Parent node ID (index:generation), defaults to scene root\"},"
        "\"nodeType\":{\"type\":\"string\",\"description\":\"Node type tag, defaults to Node3D\"},"
        "\"transform\":{\"type\":\"object\",\"description\":\"Initial transform with optional position, rotation, scale\"}"
        "},\"required\":[\"name\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "node.delete",
        cd_mcp_handle_node_delete,
        "Delete a node and its children from the scene",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\",\"description\":\"Node ID (index:generation)\"}"
        "},\"required\":[\"id\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "node.get",
        cd_mcp_handle_node_get,
        "Get full properties of a node including transform, tags, and children",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\",\"description\":\"Node ID (index:generation)\"}"
        "},\"required\":[\"id\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "node.find",
        cd_mcp_handle_node_find,
        "Find nodes by name glob pattern and/or tags",
        "{\"type\":\"object\",\"properties\":{"
        "\"nameGlob\":{\"type\":\"string\",\"description\":\"Glob pattern to match node names\"},"
        "\"nodeTypes\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Filter by node type tags\"},"
        "\"tags\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Filter by tags (all must match)\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "node.setTransform",
        cd_mcp_handle_node_set_transform,
        "Set local or world transform on a node with partial updates",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\",\"description\":\"Node ID (index:generation)\"},"
        "\"transform\":{\"type\":\"object\",\"description\":\"Transform with optional position, rotation, scale\"},"
        "\"space\":{\"type\":\"string\",\"enum\":[\"local\",\"world\"],\"description\":\"Coordinate space, defaults to local\"}"
        "},\"required\":[\"id\",\"transform\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "node.setParent",
        cd_mcp_handle_node_set_parent,
        "Reparent a node under a new parent node",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\",\"description\":\"Node ID to reparent (index:generation)\"},"
        "\"newParentId\":{\"type\":\"string\",\"description\":\"New parent node ID (index:generation)\"},"
        "\"preserveWorldTransform\":{\"type\":\"boolean\",\"description\":\"Keep world-space transform after reparenting\"}"
        "},\"required\":[\"id\",\"newParentId\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "node.setPersistent",
        cd_mcp_handle_node_set_persistent,
        "Mark a node as persistent so it survives scene transitions",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\",\"description\":\"Node ID (index:generation)\"},"
        "\"persistent\":{\"type\":\"boolean\",\"description\":\"Whether the node is persistent, defaults to true\"}"
        "},\"required\":[\"id\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "node.getPersistent",
        cd_mcp_handle_node_get_persistent,
        "Check whether a node is marked as persistent",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\",\"description\":\"Node ID (index:generation)\"}"
        "},\"required\":[\"id\"]}");
    if (res != CD_OK) return res;

    return CD_OK;
}
