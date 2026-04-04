/* cd_mcp_play_tools.c - Cadence Engine MCP play mode tool handlers
 *
 * Implements:
 *   - play.start  : Enter play mode, snapshot scene
 *   - play.stop   : Stop play, restore scene
 *   - play.pause  : Pause play
 *   - play.resume : Resume from pause
 *   - play.step   : Advance N fixed-update frames while paused
 *
 * All handlers follow the cd_mcp_tool_handler_t signature.
 * Play mode transitions delegate to cd_engine_play/stop/pause/resume/step
 * from cd_game_loop.h.
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_tool_state.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_game_loop.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

/* ============================================================================
 * Session ID generation
 *
 * Simple incrementing counter. The session ID is mainly for protocol
 * compatibility -- we do not need complex session management yet.
 *
 * PB-3: State moved from static globals into get_play_state(kernel)->
 * ============================================================================ */

/** Fallback for tests that don't set up mcp_tool_state. */
static cd_mcp_play_state_t s_play_fallback;

static cd_mcp_play_state_t* get_play_state(struct cd_kernel_t* kernel) {
    if (kernel && cd_kernel_get_mcp_tool_state(kernel)) {
        return &cd_kernel_get_mcp_tool_state(kernel)->play;
    }
    return &s_play_fallback;
}

#define CD_SESSION_ID_BUF_SIZE 32

static const char* cd_session_id_format(uint32_t id, char* buf, size_t buf_size) {
    snprintf(buf, buf_size, "session_%03u", id);
    return buf;
}

/* ============================================================================
 * Reset play tools state (for tests)
 * ============================================================================ */

void cd_mcp_play_tools_reset_state(void) {
    /* Reset the fallback state (used by tests without mcp_tool_state) */
    memset(&s_play_fallback, 0, sizeof(s_play_fallback));
}

/* ============================================================================
 * play.start handler
 *
 * Input:  { "mode": "in_editor" }   // optional
 * Output: { "status": "ok", "playSessionId": "session_001" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_play_start(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)params; /* "mode" param is accepted but not acted upon yet */

    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Kernel not available";
        return NULL;
    }

    cd_result_t res = cd_engine_play(kernel);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Cannot start play: not in edit mode";
        return NULL;
    }

    /* Generate a new session ID */
    get_play_state(kernel)->session_counter++;
    get_play_state(kernel)->active_session = get_play_state(kernel)->session_counter;

    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");

    char session_buf[CD_SESSION_ID_BUF_SIZE];
    cd_session_id_format(get_play_state(kernel)->active_session, session_buf, sizeof(session_buf));
    cJSON_AddStringToObject(result, "playSessionId", session_buf);

    return result;
}

/* ============================================================================
 * play.stop handler
 *
 * Input:  { "playSessionId": "session_001" }
 * Output: { "status": "ok" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_play_stop(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)params; /* playSessionId accepted but not validated yet */

    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Kernel not available";
        return NULL;
    }

    cd_result_t res = cd_engine_stop(kernel);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Cannot stop play: not in play or pause mode";
        return NULL;
    }

    get_play_state(kernel)->active_session = 0;

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
 * play.pause handler
 *
 * Input:  { "playSessionId": "session_001" }
 * Output: { "status": "ok" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_play_pause(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)params;

    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Kernel not available";
        return NULL;
    }

    cd_result_t res = cd_engine_pause(kernel);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Cannot pause: not in play mode";
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
 * play.resume handler
 *
 * Input:  { "playSessionId": "session_001" }
 * Output: { "status": "ok" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_play_resume(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)params;

    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Kernel not available";
        return NULL;
    }

    cd_result_t res = cd_engine_resume(kernel);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Cannot resume: not in pause mode";
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
 * play.step handler
 *
 * Input:  { "playSessionId": "session_001", "frames": 1 }
 * Output: { "status": "ok" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_play_step(
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

    /* Parse "frames" (optional, default 1) */
    int frames = 1;
    if (params != NULL) {
        const cJSON* frames_item = cJSON_GetObjectItemCaseSensitive(params, "frames");
        if (frames_item != NULL && cJSON_IsNumber(frames_item)) {
            frames = (int)frames_item->valuedouble;
            if (frames < 1) {
                *error_code = CD_JSONRPC_INVALID_PARAMS;
                *error_msg  = "Parameter 'frames' must be >= 1";
                return NULL;
            }
        }
    }

    /* Execute N fixed-update steps */
    for (int i = 0; i < frames; i++) {
        cd_result_t res = cd_engine_step(kernel);
        if (res != CD_OK) {
            *error_code = CD_JSONRPC_INVALID_PARAMS;
            *error_msg  = "Cannot step: not in pause mode";
            return NULL;
        }
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
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_play_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "play.start",
        cd_mcp_handle_play_start,
        "Enter play mode and snapshot the current scene.",
        "{\"type\":\"object\",\"properties\":{"
        "\"mode\":{\"type\":\"string\",\"description\":\"Play mode hint (e.g. in_editor)\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "play.stop",
        cd_mcp_handle_play_stop,
        "Stop play mode and restore the scene to its pre-play state.",
        "{\"type\":\"object\",\"properties\":{"
        "\"playSessionId\":{\"type\":\"string\",\"description\":\"Session ID from play.start\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "play.pause",
        cd_mcp_handle_play_pause,
        "Pause the currently running play session.",
        "{\"type\":\"object\",\"properties\":{"
        "\"playSessionId\":{\"type\":\"string\",\"description\":\"Session ID from play.start\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "play.resume",
        cd_mcp_handle_play_resume,
        "Resume a paused play session.",
        "{\"type\":\"object\",\"properties\":{"
        "\"playSessionId\":{\"type\":\"string\",\"description\":\"Session ID from play.start\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "play.step",
        cd_mcp_handle_play_step,
        "Advance N fixed-update frames while paused.",
        "{\"type\":\"object\",\"properties\":{"
        "\"playSessionId\":{\"type\":\"string\",\"description\":\"Session ID from play.start\"},"
        "\"frames\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"Number of frames to step (default 1)\"}"
        "}}");
    if (res != CD_OK) return res;

    return CD_OK;
}
