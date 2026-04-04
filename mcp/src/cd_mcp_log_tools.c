/* cd_mcp_log_tools.c - Cadence Engine MCP log tool handlers
 *
 * Task 19.3: Implements log.stream and log.query MCP tools.
 *
 *   - log.stream : Poll for new log entries since last call (per-agent cursor)
 *   - log.query  : Query log entries with time/level/module filters
 *
 * All handlers follow the cd_mcp_tool_handler_t signature.
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_log.h"
#include "cadence/cd_mcp_agent.h"
#include "cadence/cd_mcp_tool_state.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cJSON.h"

#include <string.h>

/* ============================================================================
 * Per-agent stream cursors
 *
 * PB-3: Moved from static array into cd_kernel_get_mcp_tool_state(kernel)->log_tools.
 * ============================================================================ */

#define CD_MCP_LOG_MAX_RESULTS 256

/** Static fallback for tests without kernel tool state. */
static cd_mcp_log_tools_state_t s_log_tools_fallback;

/** Get stream cursors array from kernel tool state, with fallback. */
static uint64_t* get_stream_cursors(struct cd_kernel_t* kernel) {
    if (kernel && cd_kernel_get_mcp_tool_state(kernel)) {
        return cd_kernel_get_mcp_tool_state(kernel)->log_tools.stream_cursors;
    }
    return s_log_tools_fallback.stream_cursors;
}

/* ============================================================================
 * Internal: build a JSON array from log entries
 * ============================================================================ */

static cJSON* cd_mcp_log_entries_to_json(const cd_mcp_log_entry_t* entries,
                                          uint32_t count) {
    cJSON* arr = cJSON_CreateArray();
    if (arr == NULL) return NULL;

    for (uint32_t i = 0; i < count; i++) {
        const cd_mcp_log_entry_t* e = &entries[i];
        cJSON* obj = cJSON_CreateObject();
        if (obj == NULL) continue;

        cJSON_AddNumberToObject(obj, "time", e->timestamp);
        cJSON_AddStringToObject(obj, "level", cd_log_level_to_str(e->level));
        cJSON_AddStringToObject(obj, "module", e->module);
        cJSON_AddStringToObject(obj, "message", e->message);
        cJSON_AddNumberToObject(obj, "seq", (double)e->sequence);

        cJSON_AddItemToArray(arr, obj);
    }

    return arr;
}

/* ============================================================================
 * log.stream handler
 *
 * Input:  { "level": "warn", "source": "physics", "filter": "renderer" }
 *         All params optional. "source" and "filter" are aliases (source
 *         takes precedence if both provided).
 * Output: { "entries": [...], "cursor": 42, "count": 5 }
 *
 * Returns entries since the agent's last stream cursor. Updates the cursor
 * to the latest sequence number.
 * ============================================================================ */

static cJSON* cd_mcp_handle_log_stream(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    /* Parse optional level filter */
    cd_log_level_t min_level = CD_LOG_DEBUG;
    if (params != NULL) {
        const cJSON* level_item = cJSON_GetObjectItemCaseSensitive(params, "level");
        if (level_item != NULL && cJSON_IsString(level_item)) {
            cd_log_level_from_str(level_item->valuestring, &min_level);
        }
    }

    /* Parse optional module filter ("source" preferred, "filter" as fallback) */
    const char* module_filter = NULL;
    if (params != NULL) {
        const cJSON* source_item = cJSON_GetObjectItemCaseSensitive(params, "source");
        if (source_item != NULL && cJSON_IsString(source_item)) {
            module_filter = source_item->valuestring;
        } else {
            const cJSON* filter_item = cJSON_GetObjectItemCaseSensitive(params, "filter");
            if (filter_item != NULL && cJSON_IsString(filter_item)) {
                module_filter = filter_item->valuestring;
            }
        }
    }

    /* Get the per-agent cursor from tool state */
    uint32_t agent = cd_mcp_get_current_agent();
    if (agent >= CD_MCP_MAX_AGENTS) agent = 0;
    uint64_t* cursors = get_stream_cursors(kernel);
    uint64_t since_seq = cursors[agent];

    /* Query entries since cursor */
    cd_mcp_log_entry_t results[CD_MCP_LOG_MAX_RESULTS];
    uint32_t count = cd_mcp_log_stream(since_seq, min_level, module_filter,
                                        results, CD_MCP_LOG_MAX_RESULTS);

    /* Update cursor to latest sequence */
    uint64_t new_cursor = since_seq;
    if (count > 0) {
        new_cursor = results[count - 1].sequence;
    }
    cursors[agent] = new_cursor;

    /* Build response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON* entries = cd_mcp_log_entries_to_json(results, count);
    if (entries != NULL) {
        cJSON_AddItemToObject(result, "entries", entries);
    }
    cJSON_AddNumberToObject(result, "cursor", (double)new_cursor);
    cJSON_AddNumberToObject(result, "count", (double)count);

    return result;
}

/* ============================================================================
 * log.query handler
 *
 * Input:  { "since": 10.5, "level": "error", "limit": 50, "offset": 0,
 *           "text": "NaN", "source": "renderer", "filter": "renderer" }
 *         All params optional. "source" and "filter" are aliases (source
 *         takes precedence).
 * Output: { "entries": [...], "count": 3, "total": 1024, "matched": 5 }
 *
 * "total" = total entries in the ring buffer.
 * "matched" = entries matching filters (before limit/offset applied).
 * "count" = entries returned in this response.
 * ============================================================================ */

static cJSON* cd_mcp_handle_log_query(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel;

    /* Parse optional "since" timestamp */
    double since_time = 0.0;
    if (params != NULL) {
        const cJSON* since_item = cJSON_GetObjectItemCaseSensitive(params, "since");
        if (since_item != NULL && cJSON_IsNumber(since_item)) {
            since_time = since_item->valuedouble;
        }
    }

    /* Parse optional "level" filter */
    cd_log_level_t min_level = CD_LOG_DEBUG;
    if (params != NULL) {
        const cJSON* level_item = cJSON_GetObjectItemCaseSensitive(params, "level");
        if (level_item != NULL && cJSON_IsString(level_item)) {
            cd_log_level_from_str(level_item->valuestring, &min_level);
        }
    }

    /* Parse optional "limit" (default 50) */
    uint32_t limit = 50;
    if (params != NULL) {
        const cJSON* limit_item = cJSON_GetObjectItemCaseSensitive(params, "limit");
        if (limit_item != NULL && cJSON_IsNumber(limit_item)) {
            uint32_t requested = (uint32_t)limit_item->valuedouble;
            if (requested > 0) {
                limit = requested < CD_MCP_LOG_MAX_RESULTS ? requested : CD_MCP_LOG_MAX_RESULTS;
            }
        }
    }

    /* Parse optional "offset" for pagination (default 0) */
    uint32_t offset = 0;
    if (params != NULL) {
        const cJSON* offset_item = cJSON_GetObjectItemCaseSensitive(params, "offset");
        if (offset_item != NULL && cJSON_IsNumber(offset_item)) {
            offset = (uint32_t)offset_item->valuedouble;
        }
    }

    /* Parse optional "source" / "filter" (module substring) */
    const char* module_filter = NULL;
    if (params != NULL) {
        const cJSON* source_item = cJSON_GetObjectItemCaseSensitive(params, "source");
        if (source_item != NULL && cJSON_IsString(source_item)) {
            module_filter = source_item->valuestring;
        } else {
            const cJSON* filter_item = cJSON_GetObjectItemCaseSensitive(params, "filter");
            if (filter_item != NULL && cJSON_IsString(filter_item)) {
                module_filter = filter_item->valuestring;
            }
        }
    }

    /* Parse optional "text" (message substring search) */
    const char* text_filter = NULL;
    if (params != NULL) {
        const cJSON* text_item = cJSON_GetObjectItemCaseSensitive(params, "text");
        if (text_item != NULL && cJSON_IsString(text_item)) {
            text_filter = text_item->valuestring;
        }
    }

    /* Query the buffer */
    cd_mcp_log_entry_t results[CD_MCP_LOG_MAX_RESULTS];
    uint32_t cap = limit < CD_MCP_LOG_MAX_RESULTS ? limit : CD_MCP_LOG_MAX_RESULTS;
    uint32_t count = cd_mcp_log_query(since_time, min_level, module_filter,
                                       text_filter, cap, offset, results, cap);

    /* Count total matches (for pagination metadata) */
    uint32_t matched = cd_mcp_log_query_count(since_time, min_level,
                                               module_filter, text_filter);

    /* Build response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON* entries = cd_mcp_log_entries_to_json(results, count);
    if (entries != NULL) {
        cJSON_AddItemToObject(result, "entries", entries);
    }
    cJSON_AddNumberToObject(result, "count", (double)count);
    cJSON_AddNumberToObject(result, "total", (double)cd_mcp_log_count());
    cJSON_AddNumberToObject(result, "matched", (double)matched);
    cJSON_AddNumberToObject(result, "offset", (double)offset);

    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_log_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "log.stream",
        cd_mcp_handle_log_stream,
        "Poll for new log entries since last call using per-agent cursor",
        "{\"type\":\"object\",\"properties\":{"
        "\"level\":{\"type\":\"string\",\"description\":\"Minimum log level filter\","
        "\"enum\":[\"debug\",\"info\",\"warn\",\"error\"]},"
        "\"source\":{\"type\":\"string\",\"description\":\"Module name substring filter\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "log.query",
        cd_mcp_handle_log_query,
        "Query log entries with time, level, module, and text filters",
        "{\"type\":\"object\",\"properties\":{"
        "\"since\":{\"type\":\"number\",\"description\":\"Minimum timestamp to include\"},"
        "\"level\":{\"type\":\"string\",\"description\":\"Minimum log level filter\","
        "\"enum\":[\"debug\",\"info\",\"warn\",\"error\"]},"
        "\"limit\":{\"type\":\"number\",\"description\":\"Max entries to return (default 50)\"},"
        "\"offset\":{\"type\":\"number\",\"description\":\"Number of entries to skip for pagination\"},"
        "\"source\":{\"type\":\"string\",\"description\":\"Module name substring filter\"},"
        "\"text\":{\"type\":\"string\",\"description\":\"Message text substring search\"}"
        "}}");
    if (res != CD_OK) return res;

    return CD_OK;
}

/* ============================================================================
 * Reset (for testing)
 * ============================================================================ */

void cd_mcp_log_tools_reset(void) {
    /* Reset the fallback state (used by tests without mcp_tool_state) */
    memset(&s_log_tools_fallback, 0, sizeof(s_log_tools_fallback));
}
