/* cd_mcp_scene_save_tools.c - Cadence Engine MCP scene save/open tool handlers
 *
 * Implements:
 *   - scene.save : Save the active scene to a TOML file
 *   - scene.open : Load a scene from a TOML file (replacing the active scene)
 *
 * All handlers follow the cd_mcp_tool_handler_t signature.
 * These tools wrap the kernel scene serialization APIs from Tasks 8.5/8.6.
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_scene.h"
#include "cadence/cd_scene_io_api.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * scene.save handler
 *
 * Input:  { "filepath": "scenes/level1.toml" }
 * Output: { "status": "ok", "filepath": "<filepath>" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_scene_save(
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

    if (cd_kernel_get_scene(kernel) == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "No active scene";
        return NULL;
    }

    /* Extract filepath parameter */
    const cJSON* filepath_item = NULL;
    if (params != NULL) {
        filepath_item = cJSON_GetObjectItemCaseSensitive(params, "filepath");
    }

    if (filepath_item == NULL || !cJSON_IsString(filepath_item) ||
        filepath_item->valuestring == NULL || filepath_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty 'filepath' parameter";
        return NULL;
    }

    const char* filepath = filepath_item->valuestring;

    /* Save scene via scene I/O vtable */
    const cd_scene_io_api_t* sio = cd_kernel_get_scene_io_api(kernel);
    if (!sio || !sio->save_scene) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Scene I/O plugin not loaded";
        return NULL;
    }
    cd_result_t res = sio->save_scene(cd_kernel_get_scene(kernel), cd_kernel_get_types(kernel), filepath,
                                     sio->userdata);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to save scene";
        return NULL;
    }

    /* Build success response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "filepath", filepath);

    return result;
}

/* ============================================================================
 * scene.open handler
 *
 * Input:  { "filepath": "scenes/level1.toml" }
 * Output: { "status": "ok", "filepath": "<filepath>", "nodeCount": <N> }
 * ============================================================================ */

static cJSON* cd_mcp_handle_scene_open(
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

    /* Extract filepath parameter */
    const cJSON* filepath_item = NULL;
    if (params != NULL) {
        filepath_item = cJSON_GetObjectItemCaseSensitive(params, "filepath");
    }

    if (filepath_item == NULL || !cJSON_IsString(filepath_item) ||
        filepath_item->valuestring == NULL || filepath_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty 'filepath' parameter";
        return NULL;
    }

    const char* filepath = filepath_item->valuestring;

    /* Shut down and free the current scene if one exists */
    if (cd_kernel_get_scene(kernel) != NULL) {
        cd_scene_shutdown(cd_kernel_get_scene(kernel));
        free(cd_kernel_get_scene(kernel));
        cd_kernel_set_scene(kernel, NULL);
    }

    /* Allocate a fresh scene struct */
    cd_scene_t* scene = (cd_scene_t*)calloc(1, sizeof(cd_scene_t));
    if (scene == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate scene";
        return NULL;
    }

    /* Load scene from file via scene I/O vtable */
    const cd_scene_io_api_t* sio2 = cd_kernel_get_scene_io_api(kernel);
    if (!sio2 || !sio2->load_scene) {
        free(scene);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Scene I/O plugin not loaded";
        return NULL;
    }
    cd_result_t res = sio2->load_scene(scene, cd_kernel_get_types(kernel), filepath,
                                      sio2->userdata);
    if (res != CD_OK) {
        free(scene);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        if (res == CD_ERR_IO) {
            *error_msg = "File not found or cannot be read";
        } else if (res == CD_ERR_PARSE) {
            *error_msg = "Failed to parse scene file";
        } else {
            *error_msg = "Failed to load scene";
        }
        return NULL;
    }

    /* Assign the loaded scene to the kernel */
    cd_kernel_set_scene(kernel, scene);

    /* Count nodes in the loaded scene */
    uint32_t node_count = scene->nodes.count;

    /* Build success response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "filepath", filepath);
    cJSON_AddNumberToObject(result, "nodeCount", (double)node_count);

    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_scene_save_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "scene.save",
        cd_mcp_handle_scene_save,
        "Save the active scene to a TOML file",
        "{\"type\":\"object\",\"properties\":{"
        "\"filepath\":{\"type\":\"string\",\"description\":\"Output file path for the scene\"}"
        "},\"required\":[\"filepath\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "scene.open",
        cd_mcp_handle_scene_open,
        "Load a scene from a TOML file, replacing the active scene",
        "{\"type\":\"object\",\"properties\":{"
        "\"filepath\":{\"type\":\"string\",\"description\":\"Path to the scene file to load\"}"
        "},\"required\":[\"filepath\"]}");
    if (res != CD_OK) return res;

    return CD_OK;
}
