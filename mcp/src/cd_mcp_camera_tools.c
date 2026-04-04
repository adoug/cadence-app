/* cd_mcp_camera_tools.c - Cadence Engine MCP camera tool handlers (S2.6-S2.7)
 *
 * Implements:
 *   - camera.set_mode  : Set camera mode and optional target
 *   - camera.configure : Set mode-specific parameters
 *   - camera.shake     : Trigger shake effect
 *   - camera.query     : Get current camera state
 *
 * All handlers follow the cd_mcp_tool_handler_t signature.
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_error.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_camera.h"
#include "cadence/cd_scene.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

/* ============================================================================
 * camera.set_mode
 *
 * params:
 *   mode   : string ("fixed", "follow", "orbit", "first_person", "path")
 *   target : uint64 node ID (optional)
 * ============================================================================ */

static cJSON* handle_camera_set_mode(
        cd_kernel_t* kernel, const cJSON* params,
        int* error_code, const char** error_msg) {
    if (!kernel) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("Kernel not available",
            "Camera tools require the engine to be initialized.", NULL);
        return NULL;
    }

    const cJSON* mode_j = cJSON_GetObjectItemCaseSensitive(params, "mode");
    if (!cJSON_IsString(mode_j)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Missing required parameter: mode",
            "camera.set_mode requires a 'mode' string parameter.",
            "Valid modes: fixed, follow, orbit, first_person, path.");
        return NULL;
    }

    const char* mode_str = mode_j->valuestring;
    cd_camera_mode_t mode;

    if (strcmp(mode_str, "fixed") == 0)             mode = CD_CAMERA_FIXED;
    else if (strcmp(mode_str, "follow") == 0)        mode = CD_CAMERA_FOLLOW;
    else if (strcmp(mode_str, "orbit") == 0)         mode = CD_CAMERA_ORBIT;
    else if (strcmp(mode_str, "first_person") == 0)  mode = CD_CAMERA_FIRST_PERSON;
    else if (strcmp(mode_str, "path") == 0)          mode = CD_CAMERA_PATH;
    else {
        char detail[256];
        snprintf(detail, sizeof(detail),
            "Unknown camera mode '%s'.", mode_str);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Unknown camera mode", detail,
            "Valid modes: fixed, follow, orbit, first_person, path.");
        return NULL;
    }

    cd_kernel_get_camera(kernel)->mode = mode;

    /* Optional target node */
    const cJSON* target_j = cJSON_GetObjectItemCaseSensitive(params, "target");
    if (target_j && cJSON_IsNumber(target_j)) {
        cd_kernel_get_camera(kernel)->target_node = (cd_id_t)(uint64_t)target_j->valuedouble;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", 1);
    cJSON_AddStringToObject(result, "mode", mode_str);
    return result;
}

/* ============================================================================
 * camera.configure
 *
 * params (all optional):
 *   offset          : [x, y, z]
 *   follow_distance : float
 *   follow_height   : float
 *   smoothing       : float
 *   orbit_distance  : float
 *   orbit_yaw       : float (radians)
 *   orbit_pitch     : float (radians)
 *   orbit_sensitivity : float
 *   fp_sensitivity  : float
 *   pitch_min       : float (degrees)
 *   pitch_max       : float (degrees)
 *   dead_zone       : float
 *   fov             : float (degrees)
 * ============================================================================ */

static cJSON* handle_camera_configure(
        cd_kernel_t* kernel, const cJSON* params,
        int* error_code, const char** error_msg) {
    if (!kernel) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("Kernel not available",
            "Camera tools require the engine to be initialized.", NULL);
        return NULL;
    }

    cd_camera_controller_t* cam = cd_kernel_get_camera(kernel);
    const cJSON* item;

    item = cJSON_GetObjectItemCaseSensitive(params, "offset");
    if (item && cJSON_IsArray(item) && cJSON_GetArraySize(item) >= 3) {
        cam->target_offset.x = (float)cJSON_GetArrayItem(item, 0)->valuedouble;
        cam->target_offset.y = (float)cJSON_GetArrayItem(item, 1)->valuedouble;
        cam->target_offset.z = (float)cJSON_GetArrayItem(item, 2)->valuedouble;
    }

    item = cJSON_GetObjectItemCaseSensitive(params, "follow_distance");
    if (item && cJSON_IsNumber(item)) cam->follow_distance = (float)item->valuedouble;

    item = cJSON_GetObjectItemCaseSensitive(params, "follow_height");
    if (item && cJSON_IsNumber(item)) cam->follow_height = (float)item->valuedouble;

    item = cJSON_GetObjectItemCaseSensitive(params, "smoothing");
    if (item && cJSON_IsNumber(item)) cam->follow_smoothing = (float)item->valuedouble;

    item = cJSON_GetObjectItemCaseSensitive(params, "orbit_distance");
    if (item && cJSON_IsNumber(item)) cam->orbit_distance = (float)item->valuedouble;

    item = cJSON_GetObjectItemCaseSensitive(params, "orbit_yaw");
    if (item && cJSON_IsNumber(item)) cam->orbit_yaw = (float)item->valuedouble;

    item = cJSON_GetObjectItemCaseSensitive(params, "orbit_pitch");
    if (item && cJSON_IsNumber(item)) cam->orbit_pitch = (float)item->valuedouble;

    item = cJSON_GetObjectItemCaseSensitive(params, "orbit_sensitivity");
    if (item && cJSON_IsNumber(item)) cam->orbit_sensitivity = (float)item->valuedouble;

    item = cJSON_GetObjectItemCaseSensitive(params, "fp_sensitivity");
    if (item && cJSON_IsNumber(item)) cam->fp_sensitivity = (float)item->valuedouble;

    item = cJSON_GetObjectItemCaseSensitive(params, "pitch_min");
    if (item && cJSON_IsNumber(item)) {
        float rad = cd_deg_to_rad((float)item->valuedouble);
        cam->fp_pitch_min    = rad;
        cam->orbit_pitch_min = rad;
    }

    item = cJSON_GetObjectItemCaseSensitive(params, "pitch_max");
    if (item && cJSON_IsNumber(item)) {
        float rad = cd_deg_to_rad((float)item->valuedouble);
        cam->fp_pitch_max    = rad;
        cam->orbit_pitch_max = rad;
    }

    item = cJSON_GetObjectItemCaseSensitive(params, "dead_zone");
    if (item && cJSON_IsNumber(item)) cam->dead_zone = (float)item->valuedouble;

    item = cJSON_GetObjectItemCaseSensitive(params, "fov");
    if (item && cJSON_IsNumber(item)) {
        float fov = (float)item->valuedouble;
        cam->zoom_fov_default = fov;
        cam->zoom_fov_target  = fov;
        cam->zoom_fov_current = fov;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", 1);
    return result;
}

/* ============================================================================
 * camera.shake
 *
 * params:
 *   intensity : float
 *   duration  : float (seconds)
 * ============================================================================ */

static cJSON* handle_camera_shake(
        cd_kernel_t* kernel, const cJSON* params,
        int* error_code, const char** error_msg) {
    if (!kernel) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("Kernel not available",
            "Camera tools require the engine to be initialized.", NULL);
        return NULL;
    }

    const cJSON* intensity_j = cJSON_GetObjectItemCaseSensitive(params, "intensity");
    const cJSON* duration_j  = cJSON_GetObjectItemCaseSensitive(params, "duration");

    if (!cJSON_IsNumber(intensity_j) || !cJSON_IsNumber(duration_j)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing 'intensity' and 'duration' numbers";
        return NULL;
    }

    const cd_renderer_ext_api_t* rext = cd_kernel_get_renderer_ext_api(kernel);
    if (rext && rext->camera_shake)
        rext->camera_shake(cd_kernel_get_camera(kernel),
                           (float)intensity_j->valuedouble,
                           (float)duration_j->valuedouble, rext->userdata);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", 1);
    return result;
}

/* ============================================================================
 * camera.query
 *
 * Returns current camera state.
 * ============================================================================ */

static const char* mode_to_string(cd_camera_mode_t mode) {
    switch (mode) {
        case CD_CAMERA_FIXED:        return "fixed";
        case CD_CAMERA_FOLLOW:       return "follow";
        case CD_CAMERA_ORBIT:        return "orbit";
        case CD_CAMERA_FIRST_PERSON: return "first_person";
        case CD_CAMERA_PATH:         return "path";
        default:                     return "unknown";
    }
}

static cJSON* handle_camera_query(
        cd_kernel_t* kernel, const cJSON* params,
        int* error_code, const char** error_msg) {
    (void)params;
    if (!kernel) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("Kernel not available",
            "Camera tools require the engine to be initialized.", NULL);
        return NULL;
    }

    cd_camera_controller_t* cam = cd_kernel_get_camera(kernel);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "mode", mode_to_string(cam->mode));
    cJSON_AddNumberToObject(result, "fov", (double)cam->computed_fov);

    cJSON* pos = cJSON_CreateObject();
    cJSON_AddNumberToObject(pos, "x", (double)cam->computed_position.x);
    cJSON_AddNumberToObject(pos, "y", (double)cam->computed_position.y);
    cJSON_AddNumberToObject(pos, "z", (double)cam->computed_position.z);
    cJSON_AddItemToObject(result, "position", pos);

    cJSON* tgt = cJSON_CreateObject();
    cJSON_AddNumberToObject(tgt, "x", (double)cam->computed_target.x);
    cJSON_AddNumberToObject(tgt, "y", (double)cam->computed_target.y);
    cJSON_AddNumberToObject(tgt, "z", (double)cam->computed_target.z);
    cJSON_AddItemToObject(result, "target", tgt);

    cJSON_AddBoolToObject(result, "shaking", cam->shake_timer > 0.0f ? 1 : 0);
    cJSON_AddNumberToObject(result, "shake_intensity", (double)cam->shake_intensity);

    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_camera_tools(cd_mcp_server_t* server) {
    if (!server) return CD_ERR_NULL;

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "camera.set_mode", handle_camera_set_mode,
        "Set camera controller mode and optional target node.",
        "{\"type\":\"object\",\"properties\":{"
        "\"mode\":{\"type\":\"string\",\"enum\":[\"fixed\",\"follow\",\"orbit\",\"first_person\",\"path\"]},"
        "\"target\":{\"type\":\"number\",\"description\":\"Node ID to track\"}"
        "},\"required\":[\"mode\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "camera.configure", handle_camera_configure,
        "Set mode-specific camera parameters such as offset, distance, and FOV.",
        "{\"type\":\"object\",\"properties\":{"
        "\"offset\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":3,\"maxItems\":3,\"description\":\"Target offset [x,y,z]\"},"
        "\"follow_distance\":{\"type\":\"number\"},"
        "\"follow_height\":{\"type\":\"number\"},"
        "\"smoothing\":{\"type\":\"number\"},"
        "\"orbit_distance\":{\"type\":\"number\"},"
        "\"orbit_yaw\":{\"type\":\"number\",\"description\":\"Radians\"},"
        "\"orbit_pitch\":{\"type\":\"number\",\"description\":\"Radians\"},"
        "\"orbit_sensitivity\":{\"type\":\"number\"},"
        "\"fp_sensitivity\":{\"type\":\"number\"},"
        "\"pitch_min\":{\"type\":\"number\",\"description\":\"Degrees\"},"
        "\"pitch_max\":{\"type\":\"number\",\"description\":\"Degrees\"},"
        "\"dead_zone\":{\"type\":\"number\"},"
        "\"fov\":{\"type\":\"number\",\"description\":\"Degrees\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "camera.shake", handle_camera_shake,
        "Trigger a camera shake effect with given intensity and duration.",
        "{\"type\":\"object\",\"properties\":{"
        "\"intensity\":{\"type\":\"number\"},"
        "\"duration\":{\"type\":\"number\",\"description\":\"Seconds\"}"
        "},\"required\":[\"intensity\",\"duration\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "camera.query", handle_camera_query,
        "Get current camera state including mode, position, and FOV.",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    return CD_OK;
}
