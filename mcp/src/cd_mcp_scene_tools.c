/* cd_mcp_scene_tools.c - Cadence Engine MCP scene tool handlers
 *
 * Implements:
 *   - scene.create3d : Create a new 3D scene with a root node
 *
 * All handlers follow the cd_mcp_tool_handler_t signature.
 * Scene-mutating operations allocate and store the scene in cd_kernel_get_scene(kernel).
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_error.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_scene.h"
#include "cadence/cd_events.h"
#include "cadence/cd_game_loop.h"
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
 * Internal: Build scene URI from scene name
 *
 * Format: "res://scenes/{name}.toml"
 * ============================================================================ */

#define CD_SCENE_URI_BUF_SIZE 256

static const char* cd_scene_uri_format(const char* name, char* buf, size_t buf_size) {
    snprintf(buf, buf_size, "res://scenes/%s.toml", name);
    return buf;
}

/* ============================================================================
 * scene.create3d handler
 *
 * Input:  { "name": "MainLevel", "rootName": "Root" }
 * Output: { "status": "ok", "scene_uri": "res://scenes/MainLevel.toml",
 *           "root_id": "0:1" }
 *
 * Creates a new cd_scene_t, sets the root node name, and stores the scene
 * in cd_kernel_get_scene(kernel). If a scene already exists, it is shut down and freed
 * before the new one is created.
 * ============================================================================ */

static cJSON* cd_mcp_handle_scene_create3d(
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

    /* Extract "name" (required) */
    const cJSON* name_item = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (name_item == NULL || !cJSON_IsString(name_item) ||
        name_item->valuestring == NULL || name_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: name";
        return NULL;
    }
    const char* scene_name = name_item->valuestring;

    /* Extract "rootName" (optional, defaults to "Root") */
    const char* root_name = "Root";
    const cJSON* root_name_item = cJSON_GetObjectItemCaseSensitive(params, "rootName");
    if (root_name_item != NULL && cJSON_IsString(root_name_item) &&
        root_name_item->valuestring != NULL && root_name_item->valuestring[0] != '\0') {
        root_name = root_name_item->valuestring;
    }

    /* If a scene already exists, shut it down and free it */
    if (cd_kernel_get_scene(kernel) != NULL) {
        cd_scene_shutdown(cd_kernel_get_scene(kernel));
        free(cd_kernel_get_scene(kernel));
        cd_kernel_set_scene(kernel, NULL);
    }

    /* Allocate and initialize a new scene */
    cd_scene_t* scene = (cd_scene_t*)calloc(1, sizeof(cd_scene_t));
    if (scene == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate scene";
        return NULL;
    }

    cd_result_t res = cd_scene_init(scene);
    if (res != CD_OK) {
        free(scene);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to initialize scene";
        return NULL;
    }

    /* Set the root node's name to rootName */
    cd_node_t* root_node = cd_node_get(scene, scene->root);
    if (root_node != NULL) {
        root_node->name = cd_name_from_cstr(root_name);
    }

    /* Store the scene in the kernel */
    cd_kernel_set_scene(kernel, scene);

    /* Wire type registry and kernel pointer so the component system works */
    scene->type_registry = cd_kernel_get_types(kernel);
    scene->kernel = kernel;

    /* Emit scene-loaded event so plugins (scripting, etc.) initialize for
     * this scene.  Use immediate dispatch so script.attach works in the
     * same MCP batch. */
    cd_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CD_EVT_SCENE_LOADED;
    cd_event_emit_immediate(cd_kernel_get_events(kernel), kernel, &evt);

    /* Build the response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");

    /* Format scene URI */
    char uri_buf[CD_SCENE_URI_BUF_SIZE];
    cd_scene_uri_format(scene_name, uri_buf, sizeof(uri_buf));
    cJSON_AddStringToObject(result, "scene_uri", uri_buf);

    /* Format root ID */
    char id_buf[CD_ID_STR_BUF_SIZE];
    cd_id_format(scene->root, id_buf, sizeof(id_buf));
    cJSON_AddStringToObject(result, "root_id", id_buf);

    return result;
}

/* ============================================================================
 * scene.loadAdditive handler (P1-4)
 *
 * Input:  { "path": "res://scenes/level2.toml" }
 * Output: { "status": "ok" }
 *
 * Schedules an additive scene load at the start of the next frame tick.
 * The loaded scene's nodes are merged into the current scene.
 * ============================================================================ */

static cJSON* cd_mcp_handle_scene_load_additive(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Kernel not available";
        return NULL;
    }

    const cJSON* path_item = cJSON_GetObjectItemCaseSensitive(params, "path");
    if (path_item == NULL || !cJSON_IsString(path_item) ||
        path_item->valuestring == NULL || path_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: path";
        return NULL;
    }

    cd_result_t res = cd_engine_load_scene_additive(kernel, path_item->valuestring);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to schedule additive scene load";
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
 * scene.queryAABB handler (P1-5)
 *
 * Input:  { "min": [x,y,z], "max": [x,y,z], "maxResults": 100 }
 * Output: { "status": "ok", "count": N, "node_ids": ["idx:gen", ...] }
 * ============================================================================ */

static cJSON* cd_mcp_handle_scene_query_aabb(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    if (kernel == NULL || cd_kernel_get_scene(kernel) == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("No active scene",
                          "scene.queryAABB requires a scene to be loaded.",
                          "Call scene.create3d or scene.open to load a scene.");
        return NULL;
    }

    const cJSON* min_arr = cJSON_GetObjectItemCaseSensitive(params, "min");
    const cJSON* max_arr = cJSON_GetObjectItemCaseSensitive(params, "max");
    if (!min_arr || !cJSON_IsArray(min_arr) || cJSON_GetArraySize(min_arr) != 3 ||
        !max_arr || !cJSON_IsArray(max_arr) || cJSON_GetArraySize(max_arr) != 3) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Missing or invalid AABB parameters",
                          "Both 'min' and 'max' must be arrays of 3 numbers [x,y,z].",
                          "Example: {\"min\": [-5,0,-5], \"max\": [5,10,5]}");
        return NULL;
    }

    cd_vec3_t qmin, qmax;
    qmin.x = (float)cJSON_GetArrayItem(min_arr, 0)->valuedouble;
    qmin.y = (float)cJSON_GetArrayItem(min_arr, 1)->valuedouble;
    qmin.z = (float)cJSON_GetArrayItem(min_arr, 2)->valuedouble;
    qmax.x = (float)cJSON_GetArrayItem(max_arr, 0)->valuedouble;
    qmax.y = (float)cJSON_GetArrayItem(max_arr, 1)->valuedouble;
    qmax.z = (float)cJSON_GetArrayItem(max_arr, 2)->valuedouble;

    int max_results = 100;
    const cJSON* mr = cJSON_GetObjectItemCaseSensitive(params, "maxResults");
    if (mr && cJSON_IsNumber(mr)) max_results = mr->valueint;
    if (max_results > 512) max_results = 512;

    cd_id_t* ids = (cd_id_t*)malloc((size_t)max_results * sizeof(cd_id_t));
    if (!ids) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Allocation failed";
        return NULL;
    }

    int count = cd_scene_query_aabb(cd_kernel_get_scene(kernel), &qmin, &qmax, ids, max_results);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddNumberToObject(result, "count", count);

    cJSON* arr = cJSON_AddArrayToObject(result, "node_ids");
    for (int i = 0; i < count; i++) {
        char buf[CD_ID_STR_BUF_SIZE];
        cd_id_format(ids[i], buf, sizeof(buf));
        cJSON_AddItemToArray(arr, cJSON_CreateString(buf));
    }

    free(ids);
    return result;
}

/* ============================================================================
 * scene.querySphere handler (P1-5)
 *
 * Input:  { "center": [x,y,z], "radius": R, "maxResults": 100 }
 * Output: { "status": "ok", "count": N, "node_ids": ["idx:gen", ...] }
 * ============================================================================ */

static cJSON* cd_mcp_handle_scene_query_sphere(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    if (kernel == NULL || cd_kernel_get_scene(kernel) == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("No active scene",
                          "scene.querySphere requires a scene to be loaded.",
                          "Call scene.create3d or scene.open to load a scene.");
        return NULL;
    }

    const cJSON* center_arr = cJSON_GetObjectItemCaseSensitive(params, "center");
    const cJSON* radius_item = cJSON_GetObjectItemCaseSensitive(params, "radius");
    if (!center_arr || !cJSON_IsArray(center_arr) || cJSON_GetArraySize(center_arr) != 3 ||
        !radius_item || !cJSON_IsNumber(radius_item)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Missing or invalid sphere query parameters",
                          "Both 'center' (array of 3 numbers) and 'radius' (number) are required.",
                          "Example: {\"center\": [0,5,0], \"radius\": 10.0}");
        return NULL;
    }

    cd_vec3_t center;
    center.x = (float)cJSON_GetArrayItem(center_arr, 0)->valuedouble;
    center.y = (float)cJSON_GetArrayItem(center_arr, 1)->valuedouble;
    center.z = (float)cJSON_GetArrayItem(center_arr, 2)->valuedouble;
    float radius = (float)radius_item->valuedouble;

    int max_results = 100;
    const cJSON* mr = cJSON_GetObjectItemCaseSensitive(params, "maxResults");
    if (mr && cJSON_IsNumber(mr)) max_results = mr->valueint;
    if (max_results > 512) max_results = 512;

    cd_id_t* ids = (cd_id_t*)malloc((size_t)max_results * sizeof(cd_id_t));
    if (!ids) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Allocation failed";
        return NULL;
    }

    int count = cd_scene_query_sphere(cd_kernel_get_scene(kernel), &center, radius, ids, max_results);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddNumberToObject(result, "count", count);

    cJSON* arr = cJSON_AddArrayToObject(result, "node_ids");
    for (int i = 0; i < count; i++) {
        char buf[CD_ID_STR_BUF_SIZE];
        cd_id_format(ids[i], buf, sizeof(buf));
        cJSON_AddItemToArray(arr, cJSON_CreateString(buf));
    }

    free(ids);
    return result;
}

/* ============================================================================
 * scene.raycast handler (P1-5)
 *
 * Input:  { "origin": [x,y,z], "direction": [x,y,z], "maxDistance": D }
 * Output: { "status": "ok", "hit": true/false, "node_id": "idx:gen",
 *           "distance": D }
 * ============================================================================ */

static cJSON* cd_mcp_handle_scene_raycast(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    if (kernel == NULL || cd_kernel_get_scene(kernel) == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("No active scene",
                          "scene.raycast requires a scene to be loaded.",
                          "Call scene.create3d or scene.open to load a scene.");
        return NULL;
    }

    const cJSON* origin_arr = cJSON_GetObjectItemCaseSensitive(params, "origin");
    const cJSON* dir_arr = cJSON_GetObjectItemCaseSensitive(params, "direction");
    if (!origin_arr || !cJSON_IsArray(origin_arr) || cJSON_GetArraySize(origin_arr) != 3 ||
        !dir_arr || !cJSON_IsArray(dir_arr) || cJSON_GetArraySize(dir_arr) != 3) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Missing or invalid raycast parameters",
                          "Both 'origin' and 'direction' must be arrays of 3 numbers [x,y,z].",
                          "Example: {\"origin\": [0,5,0], \"direction\": [0,-1,0]}");
        return NULL;
    }

    cd_vec3_t origin, dir;
    origin.x = (float)cJSON_GetArrayItem(origin_arr, 0)->valuedouble;
    origin.y = (float)cJSON_GetArrayItem(origin_arr, 1)->valuedouble;
    origin.z = (float)cJSON_GetArrayItem(origin_arr, 2)->valuedouble;
    dir.x = (float)cJSON_GetArrayItem(dir_arr, 0)->valuedouble;
    dir.y = (float)cJSON_GetArrayItem(dir_arr, 1)->valuedouble;
    dir.z = (float)cJSON_GetArrayItem(dir_arr, 2)->valuedouble;

    float max_dist = 1000.0f;
    const cJSON* md = cJSON_GetObjectItemCaseSensitive(params, "maxDistance");
    if (md && cJSON_IsNumber(md)) max_dist = (float)md->valuedouble;

    cd_id_t hit_id;
    float hit_dist;
    bool hit = cd_scene_raycast(cd_kernel_get_scene(kernel), &origin, &dir, max_dist,
                                 &hit_id, &hit_dist);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddBoolToObject(result, "hit", hit);

    if (hit) {
        char buf[CD_ID_STR_BUF_SIZE];
        cd_id_format(hit_id, buf, sizeof(buf));
        cJSON_AddStringToObject(result, "node_id", buf);
        cJSON_AddNumberToObject(result, "distance", (double)hit_dist);
    }

    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_scene_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "scene.create3d",
        cd_mcp_handle_scene_create3d,
        "Create a new 3D scene with a root node",
        "{\"type\":\"object\",\"properties\":{"
        "\"name\":{\"type\":\"string\",\"description\":\"Scene name\"},"
        "\"rootName\":{\"type\":\"string\",\"description\":\"Root node name, defaults to Root\"}"
        "},\"required\":[\"name\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "scene.loadAdditive",
        cd_mcp_handle_scene_load_additive,
        "Load a scene additively into the current scene",
        "{\"type\":\"object\",\"properties\":{"
        "\"path\":{\"type\":\"string\",\"description\":\"Scene resource path (e.g. res://scenes/level2.toml)\"}"
        "},\"required\":[\"path\"]}");
    if (res != CD_OK) return res;

    /* Spatial query tools (P1-5) */
    res = cd_mcp_register_tool_ex(server, "scene.queryAABB",
        cd_mcp_handle_scene_query_aabb,
        "Find nodes within an axis-aligned bounding box",
        "{\"type\":\"object\",\"properties\":{"
        "\"min\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":3,\"maxItems\":3,\"description\":\"Min corner [x,y,z]\"},"
        "\"max\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":3,\"maxItems\":3,\"description\":\"Max corner [x,y,z]\"},"
        "\"maxResults\":{\"type\":\"integer\",\"description\":\"Maximum results to return, defaults to 100\"}"
        "},\"required\":[\"min\",\"max\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "scene.querySphere",
        cd_mcp_handle_scene_query_sphere,
        "Find nodes within a sphere region",
        "{\"type\":\"object\",\"properties\":{"
        "\"center\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":3,\"maxItems\":3,\"description\":\"Sphere center [x,y,z]\"},"
        "\"radius\":{\"type\":\"number\",\"description\":\"Sphere radius\"},"
        "\"maxResults\":{\"type\":\"integer\",\"description\":\"Maximum results to return, defaults to 100\"}"
        "},\"required\":[\"center\",\"radius\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "scene.raycast",
        cd_mcp_handle_scene_raycast,
        "Cast a ray and return the first hit node",
        "{\"type\":\"object\",\"properties\":{"
        "\"origin\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":3,\"maxItems\":3,\"description\":\"Ray origin [x,y,z]\"},"
        "\"direction\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":3,\"maxItems\":3,\"description\":\"Ray direction [x,y,z]\"},"
        "\"maxDistance\":{\"type\":\"number\",\"description\":\"Maximum ray distance, defaults to 1000\"}"
        "},\"required\":[\"origin\",\"direction\"]}");
    if (res != CD_OK) return res;

    return CD_OK;
}
