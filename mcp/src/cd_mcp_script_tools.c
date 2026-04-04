/* cd_mcp_script_tools.c - Cadence Engine MCP script tool handlers
 *
 * Implements:
 *   - script.create  : Create a script file from content string
 *   - script.read    : Read a script file back
 *   - script.attach  : Attach a Lua script to a scene node
 *   - script.detach  : Detach a script from a node
 *
 * All handlers follow the cd_mcp_tool_handler_t signature.
 * Script-mutating operations work with the script manager accessible
 * through the kernel's plugin system or a global/stashed pointer.
 *
 * Task 5.6.
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_error.h"
#include "cadence/cd_mcp_tool_state.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_scene.h"
#include "cadence/cd_platform.h"
#include "cadence/cd_memory.h"
#include "cadence/cd_script_instance.h"
#include "cadence/cd_script_reload.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================================================================
 * Script manager and reload watcher pointers.
 *
 * PB-3: Moved from static globals into cd_kernel_get_mcp_tool_state(kernel)->script.
 * The set/get functions still use a module-level pointer as a fallback
 * for callers that don't have a kernel reference (plugin init).
 * ============================================================================ */

static cd_script_mgr_t*    s_fallback_script_mgr    = NULL;
static cd_script_reload_t* s_fallback_script_reload  = NULL;

void cd_mcp_script_tools_set_mgr(void* mgr) {
    s_fallback_script_mgr = (cd_script_mgr_t*)mgr;
}

void cd_mcp_script_tools_set_reload(void* reload) {
    s_fallback_script_reload = (cd_script_reload_t*)reload;
}

void* cd_mcp_script_tools_get_mgr(void) {
    return (void*)s_fallback_script_mgr;
}

/** Get the script manager, preferring kernel tool state if available. */
static cd_script_mgr_t* get_script_mgr(struct cd_kernel_t* kernel) {
    if (kernel && cd_kernel_get_mcp_tool_state(kernel) && cd_kernel_get_mcp_tool_state(kernel)->script.script_mgr) {
        return cd_kernel_get_mcp_tool_state(kernel)->script.script_mgr;
    }
    return s_fallback_script_mgr;
}

/** Get the script reload watcher, preferring kernel tool state if available. */
static cd_script_reload_t* get_script_reload(struct cd_kernel_t* kernel) {
    if (kernel && cd_kernel_get_mcp_tool_state(kernel) && cd_kernel_get_mcp_tool_state(kernel)->script.script_reload) {
        return cd_kernel_get_mcp_tool_state(kernel)->script.script_reload;
    }
    return s_fallback_script_reload;
}

/* ============================================================================
 * Internal: Format a cd_id_t as "index:generation" string
 * ============================================================================ */

#define CD_ID_STR_BUF_SIZE 24

static const char* cd_id_format_s(cd_id_t id, char* buf, size_t buf_size) {
    uint32_t index = cd_id_index(id);
    uint32_t gen   = cd_id_generation(id);
    snprintf(buf, buf_size, "%u:%u", index, gen);
    return buf;
}

/* ============================================================================
 * Internal: Parse "index:generation" string to cd_id_t
 * ============================================================================ */

static cd_id_t cd_id_parse_s(const char* str) {
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
 * Internal: Resolve a res:// URI to an absolute filesystem path.
 *
 * Uses the asset_db project_root if available, otherwise falls back to ".".
 * ============================================================================ */

static void cd_resolve_script_uri(const struct cd_kernel_t* kernel,
                                    const char* res_uri,
                                    char* out_path, size_t out_size) {
    const char* project_root = ".";
    if (kernel && cd_kernel_get_asset_db(kernel)) {
        project_root = cd_kernel_get_asset_db(kernel)->project_root;
    } else if (kernel && cd_kernel_get_config(kernel)->project_path) {
        project_root = cd_kernel_get_config(kernel)->project_path;
    }

    if (strncmp(res_uri, "res://", 6) == 0) {
        snprintf(out_path, out_size, "%s/%s", project_root, res_uri + 6);
    } else {
        snprintf(out_path, out_size, "%s/%s", project_root, res_uri);
    }
}

/* ============================================================================
 * Internal: Recursively create directories along a path
 * ============================================================================ */

static cd_result_t cd_script_mkdir_recursive(const char* path) {
    if (!path || path[0] == '\0') return CD_ERR_NULL;

    char buf[512];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) return CD_ERR_IO;
    memcpy(buf, path, len + 1);

    /* Normalize backslashes */
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\\') buf[i] = '/';
    }

    /* Remove trailing slash */
    if (len > 0 && buf[len - 1] == '/') {
        buf[len - 1] = '\0';
        len--;
    }

    /* Iterate through path components and create each one */
    for (size_t i = 1; i <= len; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char saved = buf[i];
            buf[i] = '\0';

            if (!cd_fs_exists(buf)) {
                cd_result_t res = cd_fs_mkdir(buf);
                if (res != CD_OK) return res;
            }

            buf[i] = saved;
        }
    }

    return CD_OK;
}

/* ============================================================================
 * Internal: Extract the directory portion from a file path
 *
 * Writes the directory part of `path` (up to and including the last separator)
 * into `dir_out`. If no separator is found, writes ".".
 * ============================================================================ */

static void cd_extract_dir(const char* path, char* dir_out, size_t dir_size) {
    const char* last_sep = NULL;
    const char* p = path;
    while (*p) {
        if (*p == '/' || *p == '\\') {
            last_sep = p;
        }
        p++;
    }
    if (last_sep) {
        size_t dlen = (size_t)(last_sep - path);
        if (dlen >= dir_size) dlen = dir_size - 1;
        memcpy(dir_out, path, dlen);
        dir_out[dlen] = '\0';
    } else {
        snprintf(dir_out, dir_size, ".");
    }
}

/* ============================================================================
 * script.create handler
 *
 * Input:
 *   { "scriptUri": "res://scripts/player.lua",
 *     "content": "function on_ready(self) ... end",
 *     "overwrite": false }
 *
 * Output:
 *   { "scriptUri": "res://scripts/player.lua", "size": 1234 }
 *
 * Error cases:
 *   -32602 INVALID_PARAMS: missing scriptUri or content
 *   -32603 INTERNAL_ERROR: file already exists and overwrite=false, I/O error
 * ============================================================================ */

static cJSON* cd_mcp_handle_script_create(
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

    /* Validate params */
    if (params == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing parameters";
        return NULL;
    }

    /* Extract "scriptUri" (required) */
    const cJSON* uri_item = cJSON_GetObjectItemCaseSensitive(params, "scriptUri");
    if (uri_item == NULL || !cJSON_IsString(uri_item) ||
        uri_item->valuestring == NULL || uri_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: scriptUri";
        return NULL;
    }
    const char* script_uri = uri_item->valuestring;

    /* Extract "content" (required) */
    const cJSON* content_item = cJSON_GetObjectItemCaseSensitive(params, "content");
    if (content_item == NULL || !cJSON_IsString(content_item) ||
        content_item->valuestring == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: content";
        return NULL;
    }
    const char* content = content_item->valuestring;

    /* Extract "overwrite" (optional, defaults to false) */
    bool overwrite = false;
    const cJSON* overwrite_item = cJSON_GetObjectItemCaseSensitive(params, "overwrite");
    if (overwrite_item != NULL && cJSON_IsBool(overwrite_item)) {
        overwrite = cJSON_IsTrue(overwrite_item);
    }

    /* Resolve URI to filesystem path */
    char file_path[512];
    cd_resolve_script_uri(kernel, script_uri, file_path, sizeof(file_path));

    /* Normalize backslashes */
    for (size_t i = 0; i < strlen(file_path); i++) {
        if (file_path[i] == '\\') file_path[i] = '/';
    }

    /* Check if file already exists and overwrite is false */
    if (!overwrite && cd_fs_exists(file_path)) {
        char detail[512];
        snprintf(detail, sizeof(detail),
                 "File already exists at: %s", file_path);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("Script file already exists", detail,
                          "Pass \"overwrite\": true to replace the existing file.");
        return NULL;
    }

    /* Create parent directories if needed */
    char dir_path[512];
    cd_extract_dir(file_path, dir_path, sizeof(dir_path));
    cd_result_t res = cd_script_mkdir_recursive(dir_path);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to create script directory";
        return NULL;
    }

    /* Write the script file */
    uint32_t content_len = (uint32_t)strlen(content);
    res = cd_fs_write_file(file_path, content, content_len);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to write script file";
        return NULL;
    }

    /* Build the response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "scriptUri", script_uri);
    cJSON_AddNumberToObject(result, "size", (double)content_len);

    return result;
}

/* ============================================================================
 * script.read handler
 *
 * Input:
 *   { "scriptUri": "res://scripts/player.lua" }
 *
 * Output:
 *   { "content": "...", "size": 1234, "lastModified": "2026-02-28T10:00:00Z" }
 *
 * Error cases:
 *   -32602 INVALID_PARAMS: missing scriptUri
 *   -32603 INTERNAL_ERROR: file not found, I/O error
 * ============================================================================ */

static cJSON* cd_mcp_handle_script_read(
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

    /* Validate params */
    if (params == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing parameters";
        return NULL;
    }

    /* Extract "scriptUri" (required) */
    const cJSON* uri_item = cJSON_GetObjectItemCaseSensitive(params, "scriptUri");
    if (uri_item == NULL || !cJSON_IsString(uri_item) ||
        uri_item->valuestring == NULL || uri_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: scriptUri";
        return NULL;
    }
    const char* script_uri = uri_item->valuestring;

    /* Resolve URI to filesystem path */
    char file_path[512];
    cd_resolve_script_uri(kernel, script_uri, file_path, sizeof(file_path));

    /* Normalize backslashes */
    for (size_t i = 0; i < strlen(file_path); i++) {
        if (file_path[i] == '\\') file_path[i] = '/';
    }

    /* Check file exists */
    if (!cd_fs_exists(file_path)) {
        char detail[512];
        snprintf(detail, sizeof(detail),
                 "Script file not found at resolved path: %s", file_path);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Script file not found", detail,
                          "Check the scriptUri path. Use script.create to create the file first.");
        return NULL;
    }

    /* Read file content */
    void* data = NULL;
    uint32_t size = 0;
    cd_result_t res = cd_fs_read_file(file_path, &data, &size);
    if (res != CD_OK || data == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to read script file";
        return NULL;
    }

    /* Get modification time */
    uint64_t mtime = cd_fs_modified_time(file_path);

    /* Format modification time as ISO 8601 */
    char time_buf[64];
    time_t time_val = (time_t)mtime;
    struct tm* tm_info = gmtime(&time_val);
    if (tm_info) {
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%SZ", tm_info);
    } else {
        snprintf(time_buf, sizeof(time_buf), "1970-01-01T00:00:00Z");
    }

    /* Build the response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        cd_mem_free_tagged(data);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    /* The data may not be null-terminated, so create a string carefully */
    char* content_str = (char*)malloc(size + 1);
    if (content_str == NULL) {
        cd_mem_free_tagged(data);
        cJSON_Delete(result);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate content buffer";
        return NULL;
    }
    memcpy(content_str, data, size);
    content_str[size] = '\0';
    cd_mem_free_tagged(data);

    cJSON_AddStringToObject(result, "content", content_str);
    cJSON_AddNumberToObject(result, "size", (double)size);
    cJSON_AddStringToObject(result, "lastModified", time_buf);

    free(content_str);

    return result;
}

/* ============================================================================
 * script.attach handler
 *
 * Input:
 *   { "id": "2:3", "scriptUri": "res://scripts/enemy_ai.lua" }
 *
 * Output:
 *   { "instanceId": "0:1", "nodeId": "2:3",
 *     "scriptUri": "res://scripts/enemy_ai.lua" }
 *
 * Error cases:
 *   -32602 INVALID_PARAMS: missing id or scriptUri, invalid node id
 *   -32603 INTERNAL_ERROR: script manager not available, attach failed
 * ============================================================================ */

static cJSON* cd_mcp_handle_script_attach(
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
        *error_msg  = "No active scene";
        return NULL;
    }

    /* Validate params */
    if (params == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing parameters";
        return NULL;
    }

    /* Check script manager is available */
    if (get_script_mgr(kernel) == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Script manager not available";
        return NULL;
    }

    /* Extract "id" (required) - node ID */
    const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (id_item == NULL || !cJSON_IsString(id_item) ||
        id_item->valuestring == NULL || id_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: id";
        return NULL;
    }

    cd_id_t node_id = cd_id_parse_s(id_item->valuestring);
    if (!cd_id_is_valid(node_id)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Invalid id format (expected \"index:generation\")";
        return NULL;
    }

    /* Verify node exists */
    cd_node_t* node = cd_node_get(cd_kernel_get_scene(kernel), node_id);
    if (node == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Node not found";
        return NULL;
    }

    /* Extract "scriptUri" (required) */
    const cJSON* uri_item = cJSON_GetObjectItemCaseSensitive(params, "scriptUri");
    if (uri_item == NULL || !cJSON_IsString(uri_item) ||
        uri_item->valuestring == NULL || uri_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: scriptUri";
        return NULL;
    }
    const char* script_uri = uri_item->valuestring;

    /* Resolve URI to filesystem path */
    char file_path[512];
    cd_resolve_script_uri(kernel, script_uri, file_path, sizeof(file_path));

    /* Normalize backslashes */
    for (size_t i = 0; i < strlen(file_path); i++) {
        if (file_path[i] == '\\') file_path[i] = '/';
    }

    /* Attach the script */
    cd_id_t instance_id = cd_script_mgr_attach(get_script_mgr(kernel), node_id, file_path);
    if (!cd_id_is_valid(instance_id)) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to attach script";
        return NULL;
    }

    /* Track in reload watcher if available */
    if (get_script_reload(kernel) != NULL) {
        cd_script_reload_track(get_script_reload(kernel), instance_id, true);
    }

    /* Build the response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    char id_buf[CD_ID_STR_BUF_SIZE];

    cd_id_format_s(instance_id, id_buf, sizeof(id_buf));
    cJSON_AddStringToObject(result, "instanceId", id_buf);

    cd_id_format_s(node_id, id_buf, sizeof(id_buf));
    cJSON_AddStringToObject(result, "nodeId", id_buf);

    cJSON_AddStringToObject(result, "scriptUri", script_uri);

    return result;
}

/* ============================================================================
 * script.detach handler
 *
 * Input (by instanceId):
 *   { "instanceId": "0:1" }
 * Input (by nodeId + scriptUri):
 *   { "id": "2:3", "scriptUri": "res://scripts/enemy_ai.lua" }
 *
 * Output:
 *   { "success": true }
 *
 * Error cases:
 *   -32602 INVALID_PARAMS: missing instanceId or (id + scriptUri)
 *   -32603 INTERNAL_ERROR: script manager not available, detach failed
 * ============================================================================ */

static cJSON* cd_mcp_handle_script_detach(
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

    /* Validate params */
    if (params == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing parameters";
        return NULL;
    }

    /* Check script manager is available */
    if (get_script_mgr(kernel) == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Script manager not available";
        return NULL;
    }

    cd_id_t instance_id = CD_ID_INVALID;

    /* Try "instanceId" first */
    const cJSON* inst_id_item = cJSON_GetObjectItemCaseSensitive(params, "instanceId");
    if (inst_id_item != NULL && cJSON_IsString(inst_id_item) &&
        inst_id_item->valuestring != NULL && inst_id_item->valuestring[0] != '\0') {
        instance_id = cd_id_parse_s(inst_id_item->valuestring);
        if (!cd_id_is_valid(instance_id)) {
            *error_code = CD_JSONRPC_INVALID_PARAMS;
            *error_msg  = "Invalid instanceId format (expected \"index:generation\")";
            return NULL;
        }
    } else {
        /* Try "id" (node ID) + "scriptUri" to find the instance */
        const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
        const cJSON* uri_item = cJSON_GetObjectItemCaseSensitive(params, "scriptUri");

        if (id_item == NULL || !cJSON_IsString(id_item) ||
            id_item->valuestring == NULL || id_item->valuestring[0] == '\0') {
            *error_code = CD_JSONRPC_INVALID_PARAMS;
            *error_msg  = "Missing instanceId or id parameter";
            return NULL;
        }

        if (uri_item == NULL || !cJSON_IsString(uri_item) ||
            uri_item->valuestring == NULL || uri_item->valuestring[0] == '\0') {
            *error_code = CD_JSONRPC_INVALID_PARAMS;
            *error_msg  = "Missing scriptUri parameter (required when using id)";
            return NULL;
        }

        cd_id_t node_id = cd_id_parse_s(id_item->valuestring);
        if (!cd_id_is_valid(node_id)) {
            *error_code = CD_JSONRPC_INVALID_PARAMS;
            *error_msg  = "Invalid id format (expected \"index:generation\")";
            return NULL;
        }

        /* Resolve script URI to path for matching */
        char file_path[512];
        cd_resolve_script_uri(kernel, uri_item->valuestring, file_path, sizeof(file_path));

        /* Normalize backslashes */
        for (size_t i = 0; i < strlen(file_path); i++) {
            if (file_path[i] == '\\') file_path[i] = '/';
        }

        /* Search through all instances to find matching node_id + script_path */
        bool found = false;
        for (uint32_t i = 0; i < get_script_mgr(kernel)->instances.capacity; i++) {
            uint32_t gen = get_script_mgr(kernel)->instances.generations[i];
            if (gen == 0) continue;

            cd_id_t id = cd_id_make(gen, i);
            cd_script_instance_t* inst = cd_script_mgr_get(get_script_mgr(kernel), id);
            if (inst && inst->active && inst->node_id == node_id) {
                /* Check if script path matches */
                if (strcmp(inst->script_path, file_path) == 0) {
                    instance_id = id;
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            *error_code = CD_JSONRPC_INVALID_PARAMS;
            *error_msg  = "No script instance found for given node and scriptUri";
            return NULL;
        }
    }

    /* Untrack from reload watcher before detaching */
    if (get_script_reload(kernel) != NULL) {
        cd_script_reload_untrack(get_script_reload(kernel), instance_id);
    }

    /* Detach the script instance */
    cd_result_t res = cd_script_mgr_detach(get_script_mgr(kernel), instance_id);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to detach script instance";
        return NULL;
    }

    /* Build the response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddBoolToObject(result, "success", 1);

    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_script_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "script.create", cd_mcp_handle_script_create,
        "Create a Lua script file from a content string",
        "{\"type\":\"object\",\"properties\":{"
        "\"scriptUri\":{\"type\":\"string\",\"description\":\"Resource URI for the script (e.g. res://scripts/player.lua)\"},"
        "\"content\":{\"type\":\"string\",\"description\":\"Lua source code to write\"},"
        "\"overwrite\":{\"type\":\"boolean\",\"description\":\"Replace existing file if true\"}"
        "},\"required\":[\"scriptUri\",\"content\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "script.read", cd_mcp_handle_script_read,
        "Read the contents of a Lua script file",
        "{\"type\":\"object\",\"properties\":{"
        "\"scriptUri\":{\"type\":\"string\",\"description\":\"Resource URI of the script to read\"}"
        "},\"required\":[\"scriptUri\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "script.attach", cd_mcp_handle_script_attach,
        "Attach a Lua script to a scene node",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\",\"description\":\"Node ID in index:generation format\"},"
        "\"scriptUri\":{\"type\":\"string\",\"description\":\"Resource URI of the script to attach\"}"
        "},\"required\":[\"id\",\"scriptUri\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "script.detach", cd_mcp_handle_script_detach,
        "Detach a script from a node by instanceId or by node id and scriptUri",
        "{\"type\":\"object\",\"properties\":{"
        "\"instanceId\":{\"type\":\"string\",\"description\":\"Script instance ID to detach\"},"
        "\"id\":{\"type\":\"string\",\"description\":\"Node ID (use with scriptUri)\"},"
        "\"scriptUri\":{\"type\":\"string\",\"description\":\"Script URI (use with id)\"}"
        "}}");
    if (res != CD_OK) return res;

    return CD_OK;
}
