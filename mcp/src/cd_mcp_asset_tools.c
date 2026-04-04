/* cd_mcp_asset_tools.c - Cadence Engine MCP asset tool handlers
 *
 * Implements:
 *   - asset.import : Copy an external file into the project and register it
 *                    in the asset database.
 *   - asset.list   : Query registered assets with optional kind and prefix
 *                    filtering.
 *
 * All handlers follow the cd_mcp_tool_handler_t signature.
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_error.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_asset_db.h"
#include "cadence/cd_platform.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Helper: Map a kind string to cd_asset_kind_t enum
 * ============================================================================ */

static cd_asset_kind_t cd_asset_kind_from_str(const char* str) {
    if (!str) return CD_ASSET_UNKNOWN;
    if (strcmp(str, "mesh") == 0)    return CD_ASSET_MESH;
    if (strcmp(str, "texture") == 0) return CD_ASSET_TEXTURE;
    if (strcmp(str, "audio") == 0)   return CD_ASSET_AUDIO;
    if (strcmp(str, "script") == 0)  return CD_ASSET_SCRIPT;
    if (strcmp(str, "scene") == 0)    return CD_ASSET_SCENE;
    if (strcmp(str, "material") == 0) return CD_ASSET_MATERIAL;
    if (strcmp(str, "shader") == 0)   return CD_ASSET_SHADER;
    return CD_ASSET_UNKNOWN;
}

/* ============================================================================
 * Helper: Resolve a res:// URI to an absolute path
 *
 * Strips the "res://" prefix (6 chars) and appends to project_root.
 * If the URI does not start with "res://", it is appended as-is.
 * ============================================================================ */

static void cd_resolve_res_uri(const char* project_root, const char* res_uri,
                                char* out_path, size_t out_size) {
    if (strncmp(res_uri, "res://", 6) == 0) {
        snprintf(out_path, out_size, "%s/%s", project_root, res_uri + 6);
    } else {
        snprintf(out_path, out_size, "%s/%s", project_root, res_uri);
    }
}

/* ============================================================================
 * Helper: Extract filename from a path
 *
 * Returns a pointer into the original string at the last path separator + 1,
 * or the entire string if no separator is found.
 * ============================================================================ */

static const char* cd_extract_filename(const char* path) {
    const char* last_sep = NULL;
    const char* p = path;
    while (*p) {
        if (*p == '/' || *p == '\\') {
            last_sep = p;
        }
        p++;
    }
    return last_sep ? last_sep + 1 : path;
}

/* ============================================================================
 * Helper: Recursively create directories along a path
 *
 * Given an absolute path like "/a/b/c/d/", creates /a, /a/b, /a/b/c, /a/b/c/d
 * if they do not already exist.
 * ============================================================================ */

static cd_result_t cd_fs_mkdir_recursive(const char* path) {
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
 * asset.import handler
 *
 * Input:
 *   { "uri": "/absolute/path/to/model.glb",
 *     "destDir": "res://assets/meshes/" }    (optional, default "res://assets/")
 *
 * Output:
 *   { "status": "ok",
 *     "assetUri": "res://assets/meshes/model.glb",
 *     "kind": "mesh" }
 *
 * Error cases:
 *   -32602 INVALID_PARAMS: missing uri, source file not found
 *   -32603 INTERNAL_ERROR: copy failed, no asset_db, registration failed
 * ============================================================================ */

static cJSON* cd_mcp_handle_asset_import(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    /* Validate kernel */
    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("Kernel not available",
            "This asset tool requires the engine to be initialized.", NULL);
        return NULL;
    }

    /* Validate asset_db */
    if (cd_kernel_get_asset_db(kernel) == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("Asset database not available",
            "No project is loaded or the asset database is uninitialized.",
            "Load a project with --project <path> or call project.open first.");
        return NULL;
    }

    /* --- Parse parameters ------------------------------------------------ */

    if (params == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing required parameter: uri";
        return NULL;
    }

    /* uri (required) - absolute path to source file */
    const cJSON* uri_item = cJSON_GetObjectItemCaseSensitive(params, "uri");
    if (uri_item == NULL || !cJSON_IsString(uri_item) ||
        uri_item->valuestring == NULL || uri_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: uri";
        return NULL;
    }
    const char* source_path = uri_item->valuestring;

    /* Validate source file exists */
    if (!cd_fs_exists(source_path)) {
        char detail[384];
        snprintf(detail, sizeof(detail),
            "File '%s' does not exist or is not accessible.", source_path);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Source file not found", detail,
            "Provide an absolute path to an existing file.");
        return NULL;
    }

    /* destDir (optional, default "res://assets/") */
    const char* dest_dir = "res://assets/";
    const cJSON* dest_item = cJSON_GetObjectItemCaseSensitive(params, "destDir");
    if (dest_item != NULL && cJSON_IsString(dest_item) &&
        dest_item->valuestring != NULL && dest_item->valuestring[0] != '\0') {
        dest_dir = dest_item->valuestring;
    }

    /* --- Extract filename from source path ------------------------------- */

    const char* filename = cd_extract_filename(source_path);
    if (filename[0] == '\0') {
        char detail[384];
        snprintf(detail, sizeof(detail),
            "Could not extract filename from uri '%s'.", source_path);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Invalid uri path", detail,
            "Provide a path with a filename, e.g. '/path/to/model.glb'.");
        return NULL;
    }

    /* --- Resolve destination directory to absolute path ------------------ */

    const char* project_root = cd_kernel_get_asset_db(kernel)->project_root;
    char dest_dir_abs[512];
    cd_resolve_res_uri(project_root, dest_dir, dest_dir_abs, sizeof(dest_dir_abs));

    /* Create destination directory if needed */
    cd_result_t res = cd_fs_mkdir_recursive(dest_dir_abs);
    if (res != CD_OK) {
        char detail[384];
        snprintf(detail, sizeof(detail),
            "Could not create directory '%s'.", dest_dir_abs);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("Failed to create destination directory",
            detail, "Check that the project directory is writable.");
        return NULL;
    }

    /* --- Build destination file path ------------------------------------- */

    char dest_file_abs[512];
    /* Remove trailing slash from dest_dir_abs for clean path construction */
    size_t dir_len = strlen(dest_dir_abs);
    if (dir_len > 0 && dest_dir_abs[dir_len - 1] == '/') {
        dest_dir_abs[dir_len - 1] = '\0';
    }
    snprintf(dest_file_abs, sizeof(dest_file_abs), "%s/%s",
             dest_dir_abs, filename);

    /* --- Copy file ------------------------------------------------------- */

    res = cd_fs_copy(source_path, dest_file_abs);
    if (res != CD_OK) {
        char detail[384];
        snprintf(detail, sizeof(detail),
            "Copy from '%s' to '%s' failed.", source_path, dest_file_abs);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("Failed to copy file", detail,
            "Check file permissions and available disk space.");
        return NULL;
    }

    /* --- Determine asset kind from extension ----------------------------- */

    cd_asset_kind_t kind = cd_asset_kind_from_ext(filename);

    /* --- Build the res:// URI for the imported asset --------------------- */

    /* Ensure dest_dir has trailing slash for URI construction */
    char asset_uri[256];
    size_t dest_dir_len = strlen(dest_dir);
    if (dest_dir_len > 0 && dest_dir[dest_dir_len - 1] == '/') {
        snprintf(asset_uri, sizeof(asset_uri), "%s%s", dest_dir, filename);
    } else {
        snprintf(asset_uri, sizeof(asset_uri), "%s/%s", dest_dir, filename);
    }

    /* Ensure the URI starts with res:// */
    if (strncmp(asset_uri, "res://", 6) != 0) {
        /* Prepend res:// if not present */
        char temp[256];
        snprintf(temp, sizeof(temp), "res://%s", asset_uri);
        memcpy(asset_uri, temp, sizeof(asset_uri));
    }

    /* --- Register in asset database -------------------------------------- */

    res = cd_asset_db_register(cd_kernel_get_asset_db(kernel), asset_uri, dest_file_abs, kind);
    if (res != CD_OK) {
        char detail[384];
        snprintf(detail, sizeof(detail),
            "Failed to register '%s' (kind=%s) at '%s'.",
            asset_uri, cd_asset_kind_str(kind), dest_file_abs);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("Asset registration failed", detail,
            "The file was copied but could not be added to the asset database.");
        return NULL;
    }

    /* --- Build JSON response --------------------------------------------- */

    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "assetUri", asset_uri);
    cJSON_AddStringToObject(result, "kind", cd_asset_kind_str(kind));

    return result;
}

/* ============================================================================
 * asset.list handler
 *
 * Input:
 *   { "kind": "mesh",                    (optional)
 *     "prefix": "res://assets/" }         (optional)
 *
 * Output:
 *   { "assets": [
 *       { "uri": "res://assets/meshes/player.glb", "kind": "mesh", "size": 45000 },
 *       ...
 *     ] }
 *
 * Error cases:
 *   -32603 INTERNAL_ERROR: no asset_db, kernel null
 * ============================================================================ */

static cJSON* cd_mcp_handle_asset_list(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    /* Validate kernel */
    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("Kernel not available",
            "This asset tool requires the engine to be initialized.", NULL);
        return NULL;
    }

    /* Validate asset_db */
    if (cd_kernel_get_asset_db(kernel) == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("Asset database not available",
            "No project is loaded or the asset database is uninitialized.",
            "Load a project with --project <path> or call project.open first.");
        return NULL;
    }

    /* --- Parse parameters ------------------------------------------------ */

    cd_asset_kind_t filter_kind = CD_ASSET_UNKNOWN; /* default: all */
    const char* prefix = NULL;

    if (params != NULL) {
        /* kind (optional) */
        const cJSON* kind_item = cJSON_GetObjectItemCaseSensitive(
            params, "kind");
        if (kind_item != NULL && cJSON_IsString(kind_item) &&
            kind_item->valuestring != NULL) {
            filter_kind = cd_asset_kind_from_str(kind_item->valuestring);
        }

        /* prefix (optional) */
        const cJSON* prefix_item = cJSON_GetObjectItemCaseSensitive(
            params, "prefix");
        if (prefix_item != NULL && cJSON_IsString(prefix_item) &&
            prefix_item->valuestring != NULL &&
            prefix_item->valuestring[0] != '\0') {
            prefix = prefix_item->valuestring;
        }
    }

    /* --- Query the asset database ---------------------------------------- */

    /*
     * cd_asset_db_list returns:
     * - When filter_kind == CD_ASSET_UNKNOWN: full array, count = total
     * - When filter_kind is specific: full array, count = number of matches
     *   but the caller must iterate all db->count entries and check kind.
     *
     * In both cases we iterate the full entries array (db->count entries)
     * and apply our own kind + prefix filtering.
     */
    const cd_asset_db_t* db = cd_kernel_get_asset_db(kernel);

    /* --- Build JSON array of matching assets ----------------------------- */

    cJSON* assets_arr = cJSON_CreateArray();
    if (assets_arr == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    for (uint32_t i = 0; i < db->count; i++) {
        const cd_asset_entry_t* entry = &db->entries[i];

        /* Filter by kind if specified */
        if (filter_kind != CD_ASSET_UNKNOWN && entry->kind != filter_kind) {
            continue;
        }

        /* Filter by prefix if specified */
        if (prefix != NULL) {
            if (strncmp(entry->uri, prefix, strlen(prefix)) != 0) {
                continue;
            }
        }

        /* Build asset entry object */
        cJSON* asset_obj = cJSON_CreateObject();
        if (asset_obj == NULL) continue;

        cJSON_AddStringToObject(asset_obj, "uri", entry->uri);
        cJSON_AddStringToObject(asset_obj, "kind",
                                cd_asset_kind_str(entry->kind));
        cJSON_AddNumberToObject(asset_obj, "size", (double)entry->file_size);

        cJSON_AddItemToArray(assets_arr, asset_obj);
    }

    /* --- Build result object --------------------------------------------- */

    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        cJSON_Delete(assets_arr);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddItemToObject(result, "assets", assets_arr);

    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_asset_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    cd_result_t res = cd_mcp_register_tool_ex(server, "asset.import",
        cd_mcp_handle_asset_import,
        "Import an external file into the project asset database",
        "{\"type\":\"object\",\"properties\":{"
        "\"uri\":{\"type\":\"string\",\"description\":\"Absolute path to the source file\"},"
        "\"destDir\":{\"type\":\"string\",\"description\":\"Destination directory as res:// URI (default res://assets/)\"}"
        "},\"required\":[\"uri\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "asset.list",
        cd_mcp_handle_asset_list,
        "List registered assets with optional kind and prefix filtering",
        "{\"type\":\"object\",\"properties\":{"
        "\"kind\":{\"type\":\"string\",\"enum\":[\"mesh\",\"texture\",\"audio\",\"script\",\"scene\",\"material\",\"shader\"],\"description\":\"Filter by asset kind\"},"
        "\"prefix\":{\"type\":\"string\",\"description\":\"Filter by URI prefix\"}"
        "}}");
    if (res != CD_OK) return res;

    return CD_OK;
}
