/* cd_mcp_debug_tools.c - Cadence Engine MCP debug tool handlers
 *
 * Implements:
 *   - debug.setBreakpoint    : Set a breakpoint at file:line
 *   - debug.removeBreakpoint : Remove a breakpoint by ID
 *   - debug.listBreakpoints  : List all active breakpoints
 *   - debug.continue         : Resume execution
 *   - debug.step             : Step into (next line, any depth)
 *   - debug.stepOver         : Step over (next line, same depth)
 *   - debug.stepOut          : Step out (return from current function)
 *   - debug.eval             : Evaluate expression in paused context
 *   - debug.stackTrace       : Get stack trace at pause point
 *   - debug.locals           : Get local variables at current frame
 *
 * All handlers follow the cd_mcp_tool_handler_t signature.
 * The debug state is accessed via cd_lua_debug_get_global() which
 * returns the singleton debugger attached to the Lua VM.
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_debug_api.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

/* ============================================================================
 * Internal: Get the debug state or return an error
 * ============================================================================ */

static const cd_debug_api_t* s_debug_api = NULL;

static void* get_debug_or_error(const cd_debug_api_t** out_api,
                                 int* error_code, const char** error_msg) {
    if (!s_debug_api || !s_debug_api->get_instance) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Debug API not registered";
        return NULL;
    }
    void* dbg = s_debug_api->get_instance();
    if (!dbg) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Lua debugger not initialized";
        return NULL;
    }
    *out_api = s_debug_api;
    return dbg;
}

void cd_mcp_debug_tools_set_api(const cd_debug_api_t* api) {
    s_debug_api = api;
}

/* ============================================================================
 * debug.setBreakpoint handler
 *
 * Input:  { "file": "scripts/player.lua", "line": 10 }
 * Output: { "id": 1, "file": "scripts/player.lua", "line": 10 }
 * ============================================================================ */

static cJSON* cd_mcp_handle_debug_set_breakpoint(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel;

    const cd_debug_api_t* api = NULL;
    void* dbg = get_debug_or_error(&api, error_code, error_msg);
    if (!dbg) return NULL;

    if (!params) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing parameters";
        return NULL;
    }

    const cJSON* file_item = cJSON_GetObjectItemCaseSensitive(params, "file");
    if (!file_item || !cJSON_IsString(file_item) ||
        !file_item->valuestring || file_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: file";
        return NULL;
    }

    const cJSON* line_item = cJSON_GetObjectItemCaseSensitive(params, "line");
    if (!line_item || !cJSON_IsNumber(line_item)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or invalid required parameter: line";
        return NULL;
    }

    int line = (int)line_item->valuedouble;
    if (line <= 0) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Parameter 'line' must be > 0";
        return NULL;
    }

    uint32_t bp_id = api->set_breakpoint(dbg, file_item->valuestring, line);
    if (bp_id == 0) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to set breakpoint (table may be full)";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    if (!result) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddNumberToObject(result, "id", (double)bp_id);
    cJSON_AddStringToObject(result, "file", file_item->valuestring);
    cJSON_AddNumberToObject(result, "line", (double)line);

    return result;
}

/* ============================================================================
 * debug.removeBreakpoint handler
 *
 * Input:  { "id": 1 }
 * Output: { "success": true }
 * ============================================================================ */

static cJSON* cd_mcp_handle_debug_remove_breakpoint(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel;

    const cd_debug_api_t* api = NULL;
    void* dbg = get_debug_or_error(&api, error_code, error_msg);
    if (!dbg) return NULL;

    if (!params) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing parameters";
        return NULL;
    }

    const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (!id_item || !cJSON_IsNumber(id_item)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or invalid required parameter: id";
        return NULL;
    }

    uint32_t bp_id = (uint32_t)id_item->valuedouble;
    cd_result_t res = api->remove_breakpoint(dbg, bp_id);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Breakpoint not found";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    if (!result) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddBoolToObject(result, "success", 1);
    return result;
}

/* ============================================================================
 * debug.listBreakpoints handler
 *
 * Input:  {} (no params needed)
 * Output: { "breakpoints": [ { "id": 1, "file": "...", "line": 10, "enabled": true } ] }
 * ============================================================================ */

static cJSON* cd_mcp_handle_debug_list_breakpoints(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel;
    (void)params;

    const cd_debug_api_t* api = NULL;
    void* dbg = get_debug_or_error(&api, error_code, error_msg);
    if (!dbg) return NULL;

    cd_debug_breakpoint_t bps[CD_DEBUG_MAX_BREAKPOINTS];
    uint32_t count = 0;
    api->list_breakpoints(dbg, bps, CD_DEBUG_MAX_BREAKPOINTS, &count);

    cJSON* result = cJSON_CreateObject();
    if (!result) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON* bp_array = cJSON_AddArrayToObject(result, "breakpoints");
    for (uint32_t i = 0; i < count; i++) {
        cJSON* bp_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(bp_obj, "id", (double)bps[i].id);
        cJSON_AddStringToObject(bp_obj, "file", bps[i].file);
        cJSON_AddNumberToObject(bp_obj, "line", (double)bps[i].line);
        cJSON_AddBoolToObject(bp_obj, "enabled", bps[i].enabled ? 1 : 0);
        cJSON_AddItemToArray(bp_array, bp_obj);
    }

    return result;
}

/* ============================================================================
 * debug.continue handler
 *
 * Input:  {} (no params needed)
 * Output: { "status": "ok" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_debug_continue(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel;
    (void)params;

    const cd_debug_api_t* api = NULL;
    void* dbg = get_debug_or_error(&api, error_code, error_msg);
    if (!dbg) return NULL;

    cd_result_t res = api->continue_exec(dbg);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Not paused";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    if (!result) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    return result;
}

/* ============================================================================
 * debug.step handler
 *
 * Input:  {} (no params needed)
 * Output: { "status": "ok" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_debug_step(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel;
    (void)params;

    const cd_debug_api_t* api = NULL;
    void* dbg = get_debug_or_error(&api, error_code, error_msg);
    if (!dbg) return NULL;

    cd_result_t res = api->step(dbg);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Not paused";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    if (!result) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    return result;
}

/* ============================================================================
 * debug.stepOver handler
 *
 * Input:  {} (no params needed)
 * Output: { "status": "ok" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_debug_step_over(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel;
    (void)params;

    const cd_debug_api_t* api = NULL;
    void* dbg = get_debug_or_error(&api, error_code, error_msg);
    if (!dbg) return NULL;

    cd_result_t res = api->step_over(dbg);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Not paused";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    if (!result) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    return result;
}

/* ============================================================================
 * debug.stepOut handler
 *
 * Input:  {} (no params needed)
 * Output: { "status": "ok" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_debug_step_out(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel;
    (void)params;

    const cd_debug_api_t* api = NULL;
    void* dbg = get_debug_or_error(&api, error_code, error_msg);
    if (!dbg) return NULL;

    cd_result_t res = api->step_out(dbg);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Not paused";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    if (!result) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    return result;
}

/* ============================================================================
 * debug.eval handler
 *
 * Input:  { "expression": "player.health" }
 * Output: { "result": "100" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_debug_eval(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel;

    const cd_debug_api_t* api = NULL;
    void* dbg = get_debug_or_error(&api, error_code, error_msg);
    if (!dbg) return NULL;

    if (!params) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing parameters";
        return NULL;
    }

    const cJSON* expr_item = cJSON_GetObjectItemCaseSensitive(params, "expression");
    if (!expr_item || !cJSON_IsString(expr_item) ||
        !expr_item->valuestring || expr_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: expression";
        return NULL;
    }

    const char* eval_result = api->eval(dbg, expr_item->valuestring);
    if (!eval_result) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Eval failed: debugger not paused";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    if (!result) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "result", eval_result);
    return result;
}

/* ============================================================================
 * debug.stackTrace handler
 *
 * Input:  {} (no params needed)
 * Output: { "stackTrace": "...", "file": "...", "line": 10 }
 * ============================================================================ */

static cJSON* cd_mcp_handle_debug_stack_trace(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel;
    (void)params;

    const cd_debug_api_t* api = NULL;
    void* dbg = get_debug_or_error(&api, error_code, error_msg);
    if (!dbg) return NULL;

    if (!api->is_paused(dbg)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Not paused";
        return NULL;
    }

    const char* trace = api->get_stack_trace(dbg);
    const cd_debug_pause_ctx_t* ctx = api->get_pause_ctx(dbg);

    cJSON* result = cJSON_CreateObject();
    if (!result) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "stackTrace", trace ? trace : "(unavailable)");
    if (ctx) {
        cJSON_AddStringToObject(result, "file", ctx->file);
        cJSON_AddNumberToObject(result, "line", (double)ctx->line);
    }

    return result;
}

/* ============================================================================
 * debug.locals handler
 *
 * Input:  {} (no params needed)
 * Output: { "locals": "name = value\n...", "file": "...", "line": 10 }
 * ============================================================================ */

static cJSON* cd_mcp_handle_debug_locals(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel;
    (void)params;

    const cd_debug_api_t* api = NULL;
    void* dbg = get_debug_or_error(&api, error_code, error_msg);
    if (!dbg) return NULL;

    if (!api->is_paused(dbg)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Not paused";
        return NULL;
    }

    const char* locals = api->get_locals(dbg);
    const cd_debug_pause_ctx_t* ctx = api->get_pause_ctx(dbg);

    cJSON* result = cJSON_CreateObject();
    if (!result) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "locals", locals ? locals : "(unavailable)");
    if (ctx) {
        cJSON_AddStringToObject(result, "file", ctx->file);
        cJSON_AddNumberToObject(result, "line", (double)ctx->line);
    }

    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_debug_tools(cd_mcp_server_t* server) {
    if (!server) {
        return CD_ERR_NULL;
    }

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "debug.setBreakpoint",
        cd_mcp_handle_debug_set_breakpoint,
        "Set a Lua breakpoint at a file and line",
        "{\"type\":\"object\",\"properties\":{"
        "\"file\":{\"type\":\"string\",\"description\":\"Script file path\"},"
        "\"line\":{\"type\":\"number\",\"description\":\"Line number (must be > 0)\"}"
        "},\"required\":[\"file\",\"line\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "debug.removeBreakpoint",
        cd_mcp_handle_debug_remove_breakpoint,
        "Remove a breakpoint by its ID",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"number\",\"description\":\"Breakpoint ID to remove\"}"
        "},\"required\":[\"id\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "debug.listBreakpoints",
        cd_mcp_handle_debug_list_breakpoints,
        "List all active breakpoints",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "debug.continue",
        cd_mcp_handle_debug_continue,
        "Resume Lua execution from a paused state",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "debug.step",
        cd_mcp_handle_debug_step,
        "Step into the next Lua line at any call depth",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "debug.stepOver",
        cd_mcp_handle_debug_step_over,
        "Step over to the next Lua line at the same depth",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "debug.stepOut",
        cd_mcp_handle_debug_step_out,
        "Step out of the current Lua function",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "debug.eval",
        cd_mcp_handle_debug_eval,
        "Evaluate a Lua expression in the paused context",
        "{\"type\":\"object\",\"properties\":{"
        "\"expression\":{\"type\":\"string\",\"description\":\"Lua expression to evaluate\"}"
        "},\"required\":[\"expression\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "debug.stackTrace",
        cd_mcp_handle_debug_stack_trace,
        "Get the Lua stack trace at the current pause point",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "debug.locals",
        cd_mcp_handle_debug_locals,
        "Get local variables at the current Lua stack frame",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    return CD_OK;
}
