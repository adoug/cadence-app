/* cd_mcp_scene_stream_tools.c - Cadence Engine MCP scene streaming tools (Sprint 8.4)
 *
 * Implements:
 *   - scene.stream.add_chunk : Add a streaming chunk definition
 *   - scene.stream.status    : Report all chunks and their load status
 *   - scene.stream.load      : Force-load a specific chunk
 *   - scene.stream.unload    : Force-unload a specific chunk
 *
 * The streamer is lazily initialized on first use and stored in the
 * MCP tool state (static global here, as is the convention for MCP tools).
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_scene_streamer.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Static streamer instance
 *
 * Lazily initialized on first tool call.  This follows the same pattern
 * used by other MCP tool modules (static state per module).
 * ============================================================================ */

static cd_scene_streamer_t s_streamer;
static bool s_streamer_initialized = false;

static cd_scene_streamer_t* cd_get_streamer(void) {
    if (!s_streamer_initialized) {
        if (cd_scene_streamer_init(&s_streamer) == CD_OK) {
            s_streamer_initialized = true;
        } else {
            return NULL;
        }
    }
    return &s_streamer;
}

/* ============================================================================
 * scene.stream.add_chunk handler
 *
 * Params:
 *   scene_path      (string, required) Path to scene file
 *   bounds_min      (array[3], required) AABB min corner [x, y, z]
 *   bounds_max      (array[3], required) AABB max corner [x, y, z]
 *   load_distance   (number, required) Distance to trigger load
 *   unload_distance (number, required) Distance to trigger unload
 * ============================================================================ */

static cJSON* cd_mcp_handle_stream_add_chunk(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel;

    cd_scene_streamer_t* streamer = cd_get_streamer();
    if (!streamer) {
        *error_code = -32603;
        *error_msg = "Failed to initialize scene streamer";
        return NULL;
    }

    /* Parse params */
    const cJSON* j_path = cJSON_GetObjectItemCaseSensitive(params, "scene_path");
    const cJSON* j_min = cJSON_GetObjectItemCaseSensitive(params, "bounds_min");
    const cJSON* j_max = cJSON_GetObjectItemCaseSensitive(params, "bounds_max");
    const cJSON* j_load = cJSON_GetObjectItemCaseSensitive(params, "load_distance");
    const cJSON* j_unload = cJSON_GetObjectItemCaseSensitive(params, "unload_distance");

    if (!j_path || !cJSON_IsString(j_path)) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: scene_path (string)";
        return NULL;
    }
    if (!j_min || !cJSON_IsArray(j_min) || cJSON_GetArraySize(j_min) != 3) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: bounds_min (array of 3 numbers)";
        return NULL;
    }
    if (!j_max || !cJSON_IsArray(j_max) || cJSON_GetArraySize(j_max) != 3) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: bounds_max (array of 3 numbers)";
        return NULL;
    }
    if (!j_load || !cJSON_IsNumber(j_load)) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: load_distance (number)";
        return NULL;
    }
    if (!j_unload || !cJSON_IsNumber(j_unload)) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: unload_distance (number)";
        return NULL;
    }

    float bmin[3], bmax[3];
    for (int i = 0; i < 3; i++) {
        bmin[i] = (float)cJSON_GetArrayItem(j_min, i)->valuedouble;
        bmax[i] = (float)cJSON_GetArrayItem(j_max, i)->valuedouble;
    }

    cd_result_t res = cd_scene_streamer_add_chunk(streamer,
        j_path->valuestring, bmin, bmax,
        (float)j_load->valuedouble,
        (float)j_unload->valuedouble);

    if (res != CD_OK) {
        *error_code = -32603;
        *error_msg = (res == CD_ERR_FULL)
            ? "Maximum number of scene chunks reached"
            : "Failed to add scene chunk";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", 1);
    cJSON_AddNumberToObject(result, "index", (double)(streamer->chunk_count - 1));
    cJSON_AddNumberToObject(result, "total_chunks", (double)streamer->chunk_count);
    return result;
}

/* ============================================================================
 * scene.stream.status handler
 *
 * Returns status of all registered chunks.
 * ============================================================================ */

static const char* cd_load_status_str(cd_load_status_t s) {
    switch (s) {
    case CD_LOAD_PENDING:     return "pending";
    case CD_LOAD_IN_PROGRESS: return "loading";
    case CD_LOAD_COMPLETE:    return "loaded";
    case CD_LOAD_FAILED:      return "failed";
    default:                  return "unknown";
    }
}

static cJSON* cd_mcp_handle_stream_status(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel;
    (void)params;

    cd_scene_streamer_t* streamer = cd_get_streamer();
    if (!streamer) {
        *error_code = -32603;
        *error_msg = "Failed to initialize scene streamer";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "chunk_count", (double)streamer->chunk_count);

    cJSON* chunks_arr = cJSON_AddArrayToObject(result, "chunks");
    for (uint32_t i = 0; i < streamer->chunk_count; i++) {
        const cd_scene_chunk_t* c = &streamer->chunks[i];
        cJSON* obj = cJSON_CreateObject();

        cJSON_AddNumberToObject(obj, "index", (double)i);
        cJSON_AddStringToObject(obj, "scene_path", c->scene_path);
        cJSON_AddStringToObject(obj, "status", cd_load_status_str(c->status));

        cJSON* min_arr = cJSON_AddArrayToObject(obj, "bounds_min");
        cJSON* max_arr = cJSON_AddArrayToObject(obj, "bounds_max");
        for (int j = 0; j < 3; j++) {
            cJSON_AddItemToArray(min_arr, cJSON_CreateNumber((double)c->bounds_min[j]));
            cJSON_AddItemToArray(max_arr, cJSON_CreateNumber((double)c->bounds_max[j]));
        }

        cJSON_AddNumberToObject(obj, "load_distance", (double)c->load_distance);
        cJSON_AddNumberToObject(obj, "unload_distance", (double)c->unload_distance);

        if (c->root_node != CD_ID_INVALID) {
            char id_str[32];
            snprintf(id_str, sizeof(id_str), "%llu", (unsigned long long)c->root_node);
            cJSON_AddStringToObject(obj, "root_node", id_str);
        } else {
            cJSON_AddNullToObject(obj, "root_node");
        }

        cJSON_AddBoolToObject(obj, "force_loaded", c->force_loaded);
        cJSON_AddItemToArray(chunks_arr, obj);
    }

    (void)error_code;
    (void)error_msg;
    return result;
}

/* ============================================================================
 * scene.stream.load handler
 *
 * Params:
 *   index (number, required) Chunk index to force-load
 * ============================================================================ */

static cJSON* cd_mcp_handle_stream_load(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel;

    cd_scene_streamer_t* streamer = cd_get_streamer();
    if (!streamer) {
        *error_code = -32603;
        *error_msg = "Failed to initialize scene streamer";
        return NULL;
    }

    const cJSON* j_idx = cJSON_GetObjectItemCaseSensitive(params, "index");
    if (!j_idx || !cJSON_IsNumber(j_idx)) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: index (number)";
        return NULL;
    }

    uint32_t idx = (uint32_t)j_idx->valuedouble;
    cd_result_t res = cd_scene_streamer_force_load(streamer, idx);

    if (res == CD_ERR_NOTFOUND) {
        *error_code = -32602;
        *error_msg = "Chunk index out of range";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", 1);
    cJSON_AddNumberToObject(result, "index", (double)idx);
    cJSON_AddStringToObject(result, "status", "force_load_queued");
    return result;
}

/* ============================================================================
 * scene.stream.unload handler
 *
 * Params:
 *   index (number, required) Chunk index to unload
 * ============================================================================ */

static cJSON* cd_mcp_handle_stream_unload(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    cd_scene_streamer_t* streamer = cd_get_streamer();
    if (!streamer) {
        *error_code = -32603;
        *error_msg = "Failed to initialize scene streamer";
        return NULL;
    }

    const cJSON* j_idx = cJSON_GetObjectItemCaseSensitive(params, "index");
    if (!j_idx || !cJSON_IsNumber(j_idx)) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: index (number)";
        return NULL;
    }

    uint32_t idx = (uint32_t)j_idx->valuedouble;
    cd_result_t res = cd_scene_streamer_force_unload(streamer, idx, kernel);

    if (res == CD_ERR_NOTFOUND) {
        *error_code = -32602;
        *error_msg = "Chunk index out of range";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", 1);
    cJSON_AddNumberToObject(result, "index", (double)idx);
    cJSON_AddStringToObject(result, "status", "unloaded");
    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_scene_stream_tools(cd_mcp_server_t* server) {
    if (!server) return CD_ERR_NULL;

    cd_result_t r;

    r = cd_mcp_register_tool_ex(server, "scene.stream.add_chunk",
        cd_mcp_handle_stream_add_chunk,
        "Add a streaming chunk definition with bounds and load distances",
        "{\"type\":\"object\",\"properties\":{"
        "\"scene_path\":{\"type\":\"string\",\"description\":\"Path to the scene file\"},"
        "\"bounds_min\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"description\":\"AABB min corner [x,y,z]\"},"
        "\"bounds_max\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"description\":\"AABB max corner [x,y,z]\"},"
        "\"load_distance\":{\"type\":\"number\",\"description\":\"Distance to trigger chunk load\"},"
        "\"unload_distance\":{\"type\":\"number\",\"description\":\"Distance to trigger chunk unload\"}"
        "},\"required\":[\"scene_path\",\"bounds_min\",\"bounds_max\",\"load_distance\",\"unload_distance\"]}");
    if (r != CD_OK) return r;

    r = cd_mcp_register_tool_ex(server, "scene.stream.status",
        cd_mcp_handle_stream_status,
        "Report all registered streaming chunks and their load status",
        "{\"type\":\"object\",\"properties\":{}}");
    if (r != CD_OK) return r;

    r = cd_mcp_register_tool_ex(server, "scene.stream.load",
        cd_mcp_handle_stream_load,
        "Force-load a specific streaming chunk by index",
        "{\"type\":\"object\",\"properties\":{"
        "\"index\":{\"type\":\"number\",\"description\":\"Chunk index to force-load\"}"
        "},\"required\":[\"index\"]}");
    if (r != CD_OK) return r;

    r = cd_mcp_register_tool_ex(server, "scene.stream.unload",
        cd_mcp_handle_stream_unload,
        "Force-unload a specific streaming chunk by index",
        "{\"type\":\"object\",\"properties\":{"
        "\"index\":{\"type\":\"number\",\"description\":\"Chunk index to unload\"}"
        "},\"required\":[\"index\"]}");
    if (r != CD_OK) return r;

    return CD_OK;
}
