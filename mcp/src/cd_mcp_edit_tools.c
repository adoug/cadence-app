/* cd_mcp_edit_tools.c - Cadence Engine MCP undo/redo/history tool handlers
 *
 * Implements:
 *   - edit.undo    : Undo the last command group (with optional count)
 *   - edit.redo    : Redo the last undone command group (with optional count)
 *   - edit.history : List recent undo stack entries
 *
 * Task S2.2: Expose the existing undo/redo command queue API to MCP agents.
 *
 * All handlers follow the cd_mcp_tool_handler_t signature.
 * Undo/redo operate through the command queue, which is the approved
 * mutation path -- no direct scene modification from the MCP thread.
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_scene.h"
#include "cadence/cd_commands.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

/* ============================================================================
 * Internal: command type name lookup
 * ============================================================================ */

static const char* cd_command_type_name(cd_command_type_t type) {
    switch (type) {
        case CD_CMD_NODE_CREATE:      return "node.create";
        case CD_CMD_NODE_DELETE:      return "node.delete";
        case CD_CMD_NODE_SET_PARENT:  return "node.setParent";
        case CD_CMD_SET_TRANSFORM:    return "node.setTransform";
        case CD_CMD_COMPONENT_ADD:    return "component.add";
        case CD_CMD_COMPONENT_REMOVE: return "component.remove";
        case CD_CMD_COMPONENT_SET_PROP: return "prop.set";
        case CD_CMD_TAG_ADD:          return "tag.add";
        case CD_CMD_TAG_REMOVE:       return "tag.remove";
        case CD_CMD_SCRIPT_ATTACH:    return "script.attach";
        case CD_CMD_SCRIPT_DETACH:    return "script.detach";
        case CD_CMD_ASSET_IMPORT:     return "asset.import";
        case CD_CMD_SCENE_LOAD:       return "scene.load";
        case CD_CMD_SET_PERSISTENT:   return "node.setPersistent";
        case CD_CMD_CUSTOM:           return "custom";
        default:                      return "unknown";
    }
}

/* ============================================================================
 * edit.undo handler
 *
 * Input:  { "count": 1 }  (count is optional, default 1)
 * Output: { "undone": true, "steps": 1, "remaining": N }
 *     or: { "undone": false, "reason": "nothing to undo" }
 *
 * Calls cd_command_undo() which pops the undo stack and reverses
 * each command, then pushes to the redo stack.
 * ============================================================================ */

static cJSON* cd_mcp_handle_edit_undo(
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

    /* Extract optional "count" param (default 1) */
    int count = 1;
    if (params != NULL) {
        const cJSON* count_item = cJSON_GetObjectItemCaseSensitive(params, "count");
        if (count_item != NULL && cJSON_IsNumber(count_item)) {
            count = count_item->valueint;
            if (count < 1) count = 1;
        }
    }

    /* Check if anything to undo */
    if (cd_kernel_get_commands(kernel)->undo_top == 0) {
        cJSON* result = cJSON_CreateObject();
        if (result == NULL) {
            *error_code = CD_JSONRPC_INTERNAL_ERROR;
            *error_msg  = "Failed to allocate JSON response";
            return NULL;
        }
        cJSON_AddBoolToObject(result, "undone", 0);
        cJSON_AddStringToObject(result, "reason", "nothing to undo");
        cJSON_AddNumberToObject(result, "remaining", 0);
        return result;
    }

    /* Perform undo steps */
    int steps_done = 0;
    for (int i = 0; i < count; i++) {
        cd_result_t res = cd_command_undo(
            cd_kernel_get_commands(kernel), cd_kernel_get_scene(kernel), cd_kernel_get_events(kernel));
        if (res != CD_OK) {
            break; /* No more steps to undo */
        }
        steps_done++;
    }

    /* Build response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddBoolToObject(result, "undone", steps_done > 0 ? 1 : 0);
    cJSON_AddNumberToObject(result, "steps", (double)steps_done);
    cJSON_AddNumberToObject(result, "remaining",
                            (double)cd_kernel_get_commands(kernel)->undo_top);

    return result;
}

/* ============================================================================
 * edit.redo handler
 *
 * Input:  { "count": 1 }  (count is optional, default 1)
 * Output: { "redone": true, "steps": 1, "remaining": N }
 *     or: { "redone": false, "reason": "nothing to redo" }
 *
 * Calls cd_command_redo() which pops the redo stack and re-executes
 * each command, then pushes to the undo stack.
 * ============================================================================ */

static cJSON* cd_mcp_handle_edit_redo(
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

    /* Extract optional "count" param (default 1) */
    int count = 1;
    if (params != NULL) {
        const cJSON* count_item = cJSON_GetObjectItemCaseSensitive(params, "count");
        if (count_item != NULL && cJSON_IsNumber(count_item)) {
            count = count_item->valueint;
            if (count < 1) count = 1;
        }
    }

    /* Check if anything to redo */
    if (cd_kernel_get_commands(kernel)->redo_top == 0) {
        cJSON* result = cJSON_CreateObject();
        if (result == NULL) {
            *error_code = CD_JSONRPC_INTERNAL_ERROR;
            *error_msg  = "Failed to allocate JSON response";
            return NULL;
        }
        cJSON_AddBoolToObject(result, "redone", 0);
        cJSON_AddStringToObject(result, "reason", "nothing to redo");
        cJSON_AddNumberToObject(result, "remaining", 0);
        return result;
    }

    /* Perform redo steps */
    int steps_done = 0;
    for (int i = 0; i < count; i++) {
        cd_result_t res = cd_command_redo(
            cd_kernel_get_commands(kernel), cd_kernel_get_scene(kernel), cd_kernel_get_events(kernel));
        if (res != CD_OK) {
            break; /* No more steps to redo */
        }
        steps_done++;
    }

    /* Build response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddBoolToObject(result, "redone", steps_done > 0 ? 1 : 0);
    cJSON_AddNumberToObject(result, "steps", (double)steps_done);
    cJSON_AddNumberToObject(result, "remaining",
                            (double)cd_kernel_get_commands(kernel)->redo_top);

    return result;
}

/* ============================================================================
 * edit.history handler
 *
 * Input:  { "limit": 10 }  (limit is optional, default 10)
 * Output: {
 *     "undoCount": 5,
 *     "redoCount": 2,
 *     "entries": [
 *         { "index": 0, "commandCount": 1, "commands": [
 *             { "type": "node.create", "targetId": "3:1", "timestamp": 12345 }
 *         ]},
 *         ...
 *     ]
 * }
 *
 * Reads from cd_kernel_get_commands(kernel)->undo_stack without modifying it.
 * Entries are listed from most recent (top of stack) to oldest.
 * ============================================================================ */

static cJSON* cd_mcp_handle_edit_history(
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

    /* Extract optional "limit" param (default 10) */
    int limit = 10;
    if (params != NULL) {
        const cJSON* limit_item = cJSON_GetObjectItemCaseSensitive(params, "limit");
        if (limit_item != NULL && cJSON_IsNumber(limit_item)) {
            limit = limit_item->valueint;
            if (limit < 1) limit = 1;
            if (limit > 100) limit = 100;
        }
    }

    cd_command_queue_t* q = cd_kernel_get_commands(kernel);

    /* Build response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddNumberToObject(result, "undoCount", (double)q->undo_top);
    cJSON_AddNumberToObject(result, "redoCount", (double)q->redo_top);

    cJSON* entries = cJSON_CreateArray();
    if (entries == NULL) {
        cJSON_Delete(result);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON array";
        return NULL;
    }

    /* Walk undo stack from top (most recent) to bottom, up to limit */
    uint32_t start = q->undo_top;
    uint32_t count = (uint32_t)limit;
    if (count > start) count = start;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t stack_idx = start - 1 - i;
        cd_command_t* cmds = q->undo_stack[stack_idx];
        uint32_t cmd_count = q->undo_counts[stack_idx];

        cJSON* entry = cJSON_CreateObject();
        if (entry == NULL) continue;

        cJSON_AddNumberToObject(entry, "index", (double)(start - 1 - i));
        cJSON_AddNumberToObject(entry, "commandCount", (double)cmd_count);

        /* List individual commands in this group */
        cJSON* cmd_array = cJSON_CreateArray();
        if (cmd_array != NULL && cmds != NULL) {
            for (uint32_t j = 0; j < cmd_count; j++) {
                cJSON* cmd_obj = cJSON_CreateObject();
                if (cmd_obj == NULL) continue;

                cJSON_AddStringToObject(cmd_obj, "type",
                                        cd_command_type_name(cmds[j].type));

                /* Format target ID as "index:generation" */
                if (cd_id_is_valid(cmds[j].target)) {
                    char id_buf[24];
                    uint32_t idx = cd_id_index(cmds[j].target);
                    uint32_t gen = cd_id_generation(cmds[j].target);
                    snprintf(id_buf, sizeof(id_buf), "%u:%u", idx, gen);
                    cJSON_AddStringToObject(cmd_obj, "targetId", id_buf);
                }

                cJSON_AddNumberToObject(cmd_obj, "timestamp",
                                        (double)cmds[j].timestamp);

                cJSON_AddItemToArray(cmd_array, cmd_obj);
            }
        }
        cJSON_AddItemToObject(entry, "commands", cmd_array);
        cJSON_AddItemToArray(entries, entry);
    }

    cJSON_AddItemToObject(result, "entries", entries);
    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_edit_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "edit.undo", cd_mcp_handle_edit_undo,
        "Undo the last command group with optional repeat count.",
        "{\"type\":\"object\",\"properties\":{"
        "\"count\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"Number of undo steps (default 1)\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "edit.redo", cd_mcp_handle_edit_redo,
        "Redo the last undone command group with optional repeat count.",
        "{\"type\":\"object\",\"properties\":{"
        "\"count\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"Number of redo steps (default 1)\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "edit.history", cd_mcp_handle_edit_history,
        "List recent undo stack entries for inspection.",
        "{\"type\":\"object\",\"properties\":{"
        "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100,\"description\":\"Max entries to return (default 10)\"}"
        "}}");
    if (res != CD_OK) return res;

    return CD_OK;
}
