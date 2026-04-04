/* cd_mcp_file_tools.c - Cadence Engine MCP file tool handlers
 *
 * Implements:
 *   - file.read   : Read a file by res:// URI, return content + size
 *   - file.write  : Write content to a file (creates dirs as needed)
 *   - file.list   : List files/directories in a path (optional glob pattern)
 *   - file.delete : Delete a file by res:// URI
 *
 * All paths are sandboxed to the project directory. Path traversal with
 * ".." that escapes the project root is rejected.
 *
 * All handlers follow the cd_mcp_tool_handler_t signature.
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_platform.h"
#include "cadence/cd_memory.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Helper: Resolve a res:// URI to an absolute path
 *
 * Strips the "res://" prefix (6 chars) and appends to project_root.
 * If the URI does not start with "res://", it is treated as a relative path.
 * ============================================================================ */

static void cd_file_resolve_uri(const char* project_root, const char* res_uri,
                                 char* out_path, size_t out_size) {
    if (strncmp(res_uri, "res://", 6) == 0) {
        snprintf(out_path, out_size, "%s/%s", project_root, res_uri + 6);
    } else {
        snprintf(out_path, out_size, "%s/%s", project_root, res_uri);
    }
}

/* ============================================================================
 * Helper: Normalize a path in-place (backslash -> slash)
 * ============================================================================ */

static void cd_normalize_slashes(char* path) {
    for (size_t i = 0; path[i] != '\0'; i++) {
        if (path[i] == '\\') path[i] = '/';
    }
}

/* ============================================================================
 * Helper: Resolve ".." and "." components in a path (canonicalize)
 *
 * Operates on a normalized path (forward slashes only).
 * Does NOT access the filesystem -- purely string-based.
 * Returns the length of the resolved path in the same buffer, or 0 on error.
 * ============================================================================ */

static size_t cd_resolve_path_components(char* path) {
    if (!path || path[0] == '\0') return 0;

    /* Split into components and resolve in-place using a stack approach */
    char* components[128];
    int   comp_count = 0;

    char* p = path;

    /* Preserve leading slash or drive letter */
    char prefix[8] = {0};
    size_t prefix_len = 0;

    if (p[0] == '/') {
        prefix[0] = '/';
        prefix_len = 1;
        p++;
    }
#ifdef _WIN32
    /* Handle drive letters like C:/ */
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'))
        && p[1] == ':' && p[2] == '/') {
        prefix[0] = p[0];
        prefix[1] = ':';
        prefix[2] = '/';
        prefix_len = 3;
        p += 3;
    }
#endif

    /* Tokenize */
    char* token = p;
    while (*p) {
        if (*p == '/') {
            *p = '\0';
            if (token[0] != '\0') {
                if (strcmp(token, ".") == 0) {
                    /* skip */
                } else if (strcmp(token, "..") == 0) {
                    if (comp_count > 0) {
                        comp_count--;
                    }
                    /* else: trying to go above root -- will be caught by sandbox check */
                } else {
                    if (comp_count < 128) {
                        components[comp_count++] = token;
                    }
                }
            }
            token = p + 1;
            p++;
        } else {
            p++;
        }
    }
    /* Last component */
    if (token[0] != '\0') {
        if (strcmp(token, ".") == 0) {
            /* skip */
        } else if (strcmp(token, "..") == 0) {
            if (comp_count > 0) {
                comp_count--;
            }
        } else {
            if (comp_count < 128) {
                components[comp_count++] = token;
            }
        }
    }

    /* Rebuild path into a temporary buffer, then copy back */
    char rebuilt[1024];
    size_t pos = 0;

    /* Write prefix */
    if (prefix_len > 0) {
        memcpy(rebuilt, prefix, prefix_len);
        pos = prefix_len;
    }

    for (int i = 0; i < comp_count; i++) {
        size_t clen = strlen(components[i]);
        if (i > 0) {
            if (pos < sizeof(rebuilt) - 1) rebuilt[pos++] = '/';
        }
        if (pos + clen < sizeof(rebuilt)) {
            memcpy(rebuilt + pos, components[i], clen);
            pos += clen;
        }
    }
    rebuilt[pos] = '\0';

    memcpy(path, rebuilt, pos + 1);
    return pos;
}

/* ============================================================================
 * Helper: Validate that a resolved path is within the project root
 *
 * Both paths should be normalized (forward slashes, no trailing slash).
 * Returns true if abs_path starts with project_root_normalized.
 * ============================================================================ */

static bool cd_path_is_sandboxed(const char* project_root, const char* abs_path) {
    size_t root_len = strlen(project_root);
    if (root_len == 0) return false;

    /* The resolved path must start with the project root */
    if (strncmp(abs_path, project_root, root_len) != 0) {
        return false;
    }

    /* After the root prefix, the next char must be '/' or '\0' (exact match) */
    char next = abs_path[root_len];
    if (next != '/' && next != '\0') {
        return false;
    }

    return true;
}

/* ============================================================================
 * Helper: Resolve URI to absolute path with sandbox validation
 *
 * Returns CD_OK if the path is valid and within the project sandbox.
 * On error, sets error_code/error_msg and returns an error result.
 * ============================================================================ */

static cd_result_t cd_file_resolve_and_validate(
    const char* project_root,
    const char* uri,
    char* out_path,
    size_t out_size,
    int* error_code,
    const char** error_msg)
{
    /* Build a normalized copy of the project root */
    char root_norm[512];
    snprintf(root_norm, sizeof(root_norm), "%s", project_root);
    cd_normalize_slashes(root_norm);

    /* Remove trailing slash from root */
    size_t rlen = strlen(root_norm);
    while (rlen > 0 && root_norm[rlen - 1] == '/') {
        root_norm[--rlen] = '\0';
    }

    /* Resolve the URI to an absolute path */
    cd_file_resolve_uri(root_norm, uri, out_path, out_size);
    cd_normalize_slashes(out_path);

    /* Canonicalize (resolve .. and .) */
    cd_resolve_path_components(out_path);

    /* Sandbox check */
    if (!cd_path_is_sandboxed(root_norm, out_path)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Path traversal rejected: path escapes project directory";
        return CD_ERR_INVALID;
    }

    return CD_OK;
}

/* ============================================================================
 * Helper: Recursively create directories along a path
 * ============================================================================ */

static cd_result_t cd_file_mkdir_recursive(const char* path) {
    if (!path || path[0] == '\0') return CD_ERR_NULL;

    char buf[1024];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) return CD_ERR_IO;
    memcpy(buf, path, len + 1);

    cd_normalize_slashes(buf);

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
 * Helper: Extract parent directory from a path
 *
 * Writes the parent directory portion of path into out_dir.
 * ============================================================================ */

static void cd_file_parent_dir(const char* path, char* out_dir, size_t out_size) {
    snprintf(out_dir, out_size, "%s", path);
    cd_normalize_slashes(out_dir);

    /* Find last slash */
    char* last_slash = NULL;
    for (char* p = out_dir; *p; p++) {
        if (*p == '/') last_slash = p;
    }

    if (last_slash != NULL && last_slash != out_dir) {
        *last_slash = '\0';
    }
}

/* ============================================================================
 * Helper: Simple glob pattern matching (supports * and ?)
 *
 * Returns true if name matches the pattern.
 * ============================================================================ */

static bool cd_glob_match(const char* pattern, const char* name) {
    const char* p = pattern;
    const char* n = name;
    const char* star_p = NULL;
    const char* star_n = NULL;

    while (*n) {
        if (*p == '*') {
            star_p = p++;
            star_n = n;
        } else if (*p == '?' || *p == *n) {
            p++;
            n++;
        } else if (star_p) {
            p = star_p + 1;
            n = ++star_n;
        } else {
            return false;
        }
    }

    while (*p == '*') p++;
    return *p == '\0';
}

/* ============================================================================
 * file.read handler
 *
 * Input:
 *   { "uri": "res://scripts/player.lua" }
 *
 * Output:
 *   { "content": "...", "size": 1234, "uri": "res://scripts/player.lua" }
 *
 * Error cases:
 *   -32602 INVALID_PARAMS: missing uri, path traversal
 *   -32603 INTERNAL_ERROR: kernel null, read failure
 * ============================================================================ */

static cJSON* cd_mcp_handle_file_read(
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

    const char* project_root = cd_kernel_get_config(kernel)->project_path;
    if (project_root == NULL || project_root[0] == '\0') {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "No project path configured";
        return NULL;
    }

    /* Parse uri parameter */
    if (params == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing required parameter: uri";
        return NULL;
    }

    const cJSON* uri_item = cJSON_GetObjectItemCaseSensitive(params, "uri");
    if (uri_item == NULL || !cJSON_IsString(uri_item) ||
        uri_item->valuestring == NULL || uri_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: uri";
        return NULL;
    }

    const char* uri = uri_item->valuestring;

    /* Resolve and validate path */
    char abs_path[1024];
    cd_result_t res = cd_file_resolve_and_validate(
        project_root, uri, abs_path, sizeof(abs_path), error_code, error_msg);
    if (res != CD_OK) return NULL;

    /* Check file exists */
    if (!cd_fs_exists(abs_path)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "File not found";
        return NULL;
    }

    /* Read file content */
    void* data = NULL;
    uint32_t size = 0;
    res = cd_fs_read_file(abs_path, &data, &size);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to read file";
        return NULL;
    }

    /* Build response -- content is treated as text */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        cd_mem_free_tagged(data);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    /* Null-terminate the data for JSON string creation */
    char* text = (char*)malloc(size + 1);
    if (text == NULL) {
        cd_mem_free_tagged(data);
        cJSON_Delete(result);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate buffer";
        return NULL;
    }
    memcpy(text, data, size);
    text[size] = '\0';
    cd_mem_free_tagged(data);

    cJSON_AddStringToObject(result, "content", text);
    cJSON_AddNumberToObject(result, "size", (double)size);
    cJSON_AddStringToObject(result, "uri", uri);

    free(text);

    return result;
}

/* ============================================================================
 * file.write handler
 *
 * Input:
 *   { "uri": "res://scripts/player.lua", "content": "..." }
 *
 * Output:
 *   { "status": "ok", "uri": "res://scripts/player.lua", "size": N }
 *
 * Error cases:
 *   -32602 INVALID_PARAMS: missing uri/content, path traversal
 *   -32603 INTERNAL_ERROR: kernel null, write failure
 * ============================================================================ */

static cJSON* cd_mcp_handle_file_write(
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

    const char* project_root = cd_kernel_get_config(kernel)->project_path;
    if (project_root == NULL || project_root[0] == '\0') {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "No project path configured";
        return NULL;
    }

    /* Parse parameters */
    if (params == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing required parameters: uri, content";
        return NULL;
    }

    const cJSON* uri_item = cJSON_GetObjectItemCaseSensitive(params, "uri");
    if (uri_item == NULL || !cJSON_IsString(uri_item) ||
        uri_item->valuestring == NULL || uri_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: uri";
        return NULL;
    }

    const cJSON* content_item = cJSON_GetObjectItemCaseSensitive(params, "content");
    if (content_item == NULL || !cJSON_IsString(content_item) ||
        content_item->valuestring == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing required parameter: content";
        return NULL;
    }

    const char* uri = uri_item->valuestring;
    const char* content = content_item->valuestring;
    uint32_t content_len = (uint32_t)strlen(content);

    /* Resolve and validate path */
    char abs_path[1024];
    cd_result_t res = cd_file_resolve_and_validate(
        project_root, uri, abs_path, sizeof(abs_path), error_code, error_msg);
    if (res != CD_OK) return NULL;

    /* Create parent directories if needed */
    char parent[1024];
    cd_file_parent_dir(abs_path, parent, sizeof(parent));
    if (!cd_fs_exists(parent)) {
        res = cd_file_mkdir_recursive(parent);
        if (res != CD_OK) {
            *error_code = CD_JSONRPC_INTERNAL_ERROR;
            *error_msg  = "Failed to create parent directories";
            return NULL;
        }
    }

    /* Write file */
    res = cd_fs_write_file(abs_path, content, content_len);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to write file";
        return NULL;
    }

    /* Build response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "uri", uri);
    cJSON_AddNumberToObject(result, "size", (double)content_len);

    return result;
}

/* ============================================================================
 * file.list handler
 *
 * Input:
 *   { "uri": "res://scripts/", "pattern": "*.lua" }   (pattern is optional)
 *
 * Output:
 *   { "entries": [
 *       { "name": "player.lua", "type": "file" },
 *       { "name": "utils/",     "type": "directory" },
 *       ...
 *     ],
 *     "uri": "res://scripts/" }
 *
 * Error cases:
 *   -32602 INVALID_PARAMS: missing uri, path traversal
 *   -32603 INTERNAL_ERROR: kernel null, list failure
 * ============================================================================ */

static cJSON* cd_mcp_handle_file_list(
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

    const char* project_root = cd_kernel_get_config(kernel)->project_path;
    if (project_root == NULL || project_root[0] == '\0') {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "No project path configured";
        return NULL;
    }

    /* Parse parameters */
    if (params == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing required parameter: uri";
        return NULL;
    }

    const cJSON* uri_item = cJSON_GetObjectItemCaseSensitive(params, "uri");
    if (uri_item == NULL || !cJSON_IsString(uri_item) ||
        uri_item->valuestring == NULL || uri_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: uri";
        return NULL;
    }

    const char* uri = uri_item->valuestring;

    /* Optional glob pattern */
    const char* pattern = NULL;
    const cJSON* pattern_item = cJSON_GetObjectItemCaseSensitive(params, "pattern");
    if (pattern_item != NULL && cJSON_IsString(pattern_item) &&
        pattern_item->valuestring != NULL && pattern_item->valuestring[0] != '\0') {
        pattern = pattern_item->valuestring;
    }

    /* Resolve and validate path */
    char abs_path[1024];
    cd_result_t res = cd_file_resolve_and_validate(
        project_root, uri, abs_path, sizeof(abs_path), error_code, error_msg);
    if (res != CD_OK) return NULL;

    /* Check directory exists */
    if (!cd_fs_exists(abs_path) || !cd_fs_is_dir(abs_path)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Directory not found";
        return NULL;
    }

    /* List directory entries */
    char** entries = NULL;
    uint32_t count = 0;
    res = cd_fs_list_dir(abs_path, &entries, &count);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to list directory";
        return NULL;
    }

    /* Build JSON array of entries */
    cJSON* entries_arr = cJSON_CreateArray();
    if (entries_arr == NULL) {
        /* Free entries */
        for (uint32_t i = 0; i < count; i++) cd_mem_free_tagged(entries[i]);
        cd_mem_free_tagged(entries);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    for (uint32_t i = 0; i < count; i++) {
        const char* name = entries[i];

        /* Apply glob filter if specified */
        if (pattern != NULL && !cd_glob_match(pattern, name)) {
            continue;
        }

        /* Determine if it's a directory */
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", abs_path, name);

        bool is_dir = cd_fs_is_dir(full_path);

        cJSON* entry_obj = cJSON_CreateObject();
        if (entry_obj == NULL) continue;

        cJSON_AddStringToObject(entry_obj, "name", name);
        cJSON_AddStringToObject(entry_obj, "type", is_dir ? "directory" : "file");

        cJSON_AddItemToArray(entries_arr, entry_obj);
    }

    /* Free directory entries */
    for (uint32_t i = 0; i < count; i++) cd_mem_free_tagged(entries[i]);
    cd_mem_free_tagged(entries);

    /* Build result object */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        cJSON_Delete(entries_arr);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddItemToObject(result, "entries", entries_arr);
    cJSON_AddStringToObject(result, "uri", uri);

    return result;
}

/* ============================================================================
 * file.delete handler
 *
 * Input:
 *   { "uri": "res://assets/old_model.glb" }
 *
 * Output:
 *   { "status": "ok", "uri": "res://assets/old_model.glb" }
 *
 * Error cases:
 *   -32602 INVALID_PARAMS: missing uri, path traversal, file not found
 *   -32603 INTERNAL_ERROR: kernel null, delete failure
 * ============================================================================ */

static cJSON* cd_mcp_handle_file_delete(
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

    const char* project_root = cd_kernel_get_config(kernel)->project_path;
    if (project_root == NULL || project_root[0] == '\0') {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "No project path configured";
        return NULL;
    }

    /* Parse uri parameter */
    if (params == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing required parameter: uri";
        return NULL;
    }

    const cJSON* uri_item = cJSON_GetObjectItemCaseSensitive(params, "uri");
    if (uri_item == NULL || !cJSON_IsString(uri_item) ||
        uri_item->valuestring == NULL || uri_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: uri";
        return NULL;
    }

    const char* uri = uri_item->valuestring;

    /* Resolve and validate path */
    char abs_path[1024];
    cd_result_t res = cd_file_resolve_and_validate(
        project_root, uri, abs_path, sizeof(abs_path), error_code, error_msg);
    if (res != CD_OK) return NULL;

    /* Check file exists */
    if (!cd_fs_exists(abs_path)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "File not found";
        return NULL;
    }

    /* Delete the file */
    res = cd_fs_delete(abs_path);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to delete file";
        return NULL;
    }

    /* Build response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "uri", uri);

    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_file_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "file.read",
        cd_mcp_handle_file_read,
        "Read a project file by res:// URI and return its content",
        "{\"type\":\"object\",\"properties\":{"
        "\"uri\":{\"type\":\"string\",\"description\":\"Resource URI (e.g. res://scripts/player.lua)\"}"
        "},\"required\":[\"uri\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "file.write",
        cd_mcp_handle_file_write,
        "Write content to a project file, creating directories as needed",
        "{\"type\":\"object\",\"properties\":{"
        "\"uri\":{\"type\":\"string\",\"description\":\"Resource URI (e.g. res://scripts/player.lua)\"},"
        "\"content\":{\"type\":\"string\",\"description\":\"File content to write\"}"
        "},\"required\":[\"uri\",\"content\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "file.list",
        cd_mcp_handle_file_list,
        "List files and directories at a res:// path with optional glob filter",
        "{\"type\":\"object\",\"properties\":{"
        "\"uri\":{\"type\":\"string\",\"description\":\"Directory URI (e.g. res://scripts/)\"},"
        "\"pattern\":{\"type\":\"string\",\"description\":\"Optional glob pattern (e.g. *.lua)\"}"
        "},\"required\":[\"uri\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "file.delete",
        cd_mcp_handle_file_delete,
        "Delete a project file by res:// URI",
        "{\"type\":\"object\",\"properties\":{"
        "\"uri\":{\"type\":\"string\",\"description\":\"Resource URI of the file to delete\"}"
        "},\"required\":[\"uri\"]}");
    if (res != CD_OK) return res;

    return CD_OK;
}
