#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
/* cd_mcp_prefab_tools.c - Cadence Engine MCP prefab tool handlers
 *
 * Task 10.5: MCP tools for prefab.create and prefab.instantiate
 *
 * Implements:
 *   - prefab.create      : Save a node subtree as a .prefab.toml file
 *   - prefab.instantiate : Instantiate a prefab from file under a parent node
 *
 * All handlers follow the cd_mcp_tool_handler_t signature.
 * These tools wrap the kernel prefab APIs from Task 10.4.
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_scene.h"
#include "cadence/cd_prefab.h"
#include "cadence/cd_prefab_api.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Helper: parse a node ID string "index:generation" into cd_id_t
 * ============================================================================ */

static cd_id_t parse_node_id(const char* id_str) {
    if (id_str == NULL || id_str[0] == '\0') return CD_ID_INVALID;
    uint32_t index = 0, gen = 0;
    if (sscanf(id_str, "%u:%u", &index, &gen) == 2) {
        return cd_id_make(gen, index);
    }
    return CD_ID_INVALID;
}

/* ============================================================================
 * Helper: format a cd_id_t as "index:generation" into a buffer
 * ============================================================================ */

static void format_node_id(cd_id_t id, char* buf, size_t buf_size) {
    snprintf(buf, buf_size, "%u:%u", cd_id_index(id), cd_id_generation(id));
}

/* ============================================================================
 * prefab.create handler
 *
 * Input:  { "nodeId": "idx:gen", "filepath": "prefabs/enemy.prefab.toml" }
 * Output: { "status": "ok", "filepath": "<filepath>", "nodeId": "<nodeId>" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_prefab_create(
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

    /* Extract nodeId parameter */
    const cJSON* node_id_item = NULL;
    if (params != NULL) {
        node_id_item = cJSON_GetObjectItemCaseSensitive(params, "nodeId");
    }

    if (node_id_item == NULL || !cJSON_IsString(node_id_item) ||
        node_id_item->valuestring == NULL || node_id_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty 'nodeId' parameter";
        return NULL;
    }

    cd_id_t node_id = parse_node_id(node_id_item->valuestring);
    if (!cd_id_is_valid(node_id)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Invalid 'nodeId' format (expected 'index:generation')";
        return NULL;
    }

    /* Verify node exists */
    cd_node_t* node = cd_node_get(cd_kernel_get_scene(kernel), node_id);
    if (node == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Node not found";
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

    /* Save prefab via vtable API (PA-6) */
    const cd_prefab_api_t* prefab = cd_kernel_get_prefab_api(kernel);
    if (prefab == NULL || prefab->save == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Prefab API not available (scene_io plugin not loaded)";
        return NULL;
    }
    cd_result_t res = prefab->save(cd_kernel_get_scene(kernel), cd_kernel_get_types(kernel),
                                    node_id, filepath, prefab->userdata);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        if (res == CD_ERR_IO) {
            *error_msg = "Failed to write prefab file";
        } else if (res == CD_ERR_NOTFOUND) {
            *error_msg = "Node not found";
        } else {
            *error_msg = "Failed to create prefab";
        }
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
    cJSON_AddStringToObject(result, "nodeId", node_id_item->valuestring);

    return result;
}

/* ============================================================================
 * prefab.instantiate handler
 *
 * Input:  { "filepath": "prefabs/enemy.prefab.toml", "parentId": "idx:gen" }
 *         parentId is optional -- defaults to scene root
 * Output: { "status": "ok", "rootNodeId": "idx:gen", "filepath": "<filepath>" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_prefab_instantiate(
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

    /* Extract optional parentId parameter (defaults to scene root) */
    cd_id_t parent_id = cd_kernel_get_scene(kernel)->root;

    const cJSON* parent_item = NULL;
    if (params != NULL) {
        parent_item = cJSON_GetObjectItemCaseSensitive(params, "parentId");
    }

    if (parent_item != NULL && cJSON_IsString(parent_item) &&
        parent_item->valuestring != NULL && parent_item->valuestring[0] != '\0') {
        parent_id = parse_node_id(parent_item->valuestring);
        if (!cd_id_is_valid(parent_id)) {
            *error_code = CD_JSONRPC_INVALID_PARAMS;
            *error_msg  = "Invalid 'parentId' format (expected 'index:generation')";
            return NULL;
        }

        /* Verify parent exists */
        cd_node_t* parent_node = cd_node_get(cd_kernel_get_scene(kernel), parent_id);
        if (parent_node == NULL) {
            *error_code = CD_JSONRPC_INVALID_PARAMS;
            *error_msg  = "Parent node not found";
            return NULL;
        }
    }

    /* Instantiate prefab via vtable API (PA-6) */
    const cd_prefab_api_t* prefab = cd_kernel_get_prefab_api(kernel);
    if (prefab == NULL || prefab->instantiate == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Prefab API not available (scene_io plugin not loaded)";
        return NULL;
    }
    cd_id_t root_id = CD_ID_INVALID;
    cd_result_t res = prefab->instantiate(cd_kernel_get_scene(kernel), cd_kernel_get_types(kernel),
                                           filepath, parent_id, &root_id,
                                           prefab->userdata);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        if (res == CD_ERR_IO) {
            *error_msg = "Prefab file not found or cannot be read";
        } else if (res == CD_ERR_PARSE) {
            *error_msg = "Failed to parse prefab file";
        } else if (res == CD_ERR_NOTFOUND) {
            *error_msg = "Parent node not found";
        } else {
            *error_msg = "Failed to instantiate prefab";
        }
        return NULL;
    }

    /* Build success response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    char id_buf[32];
    format_node_id(root_id, id_buf, sizeof(id_buf));

    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "rootNodeId", id_buf);
    cJSON_AddStringToObject(result, "filepath", filepath);

    return result;
}

/* ============================================================================
 * prefab.apply handler
 *
 * Input:  { "nodeId": "idx:gen" }
 * Output: { "status": "ok", "nodeId": "<nodeId>" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_prefab_apply(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    if (kernel == NULL || cd_kernel_get_scene(kernel) == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = kernel == NULL ? "Kernel not available" : "No active scene";
        return NULL;
    }

    const cJSON* node_id_item = params ? cJSON_GetObjectItemCaseSensitive(params, "nodeId") : NULL;
    if (node_id_item == NULL || !cJSON_IsString(node_id_item) ||
        node_id_item->valuestring == NULL || node_id_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty 'nodeId' parameter";
        return NULL;
    }

    cd_id_t node_id = parse_node_id(node_id_item->valuestring);
    if (!cd_id_is_valid(node_id)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Invalid 'nodeId' format (expected 'index:generation')";
        return NULL;
    }

    const cd_prefab_api_t* prefab = cd_kernel_get_prefab_api(kernel);
    if (prefab == NULL || prefab->apply == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Prefab API not available (scene_io plugin not loaded)";
        return NULL;
    }
    cd_result_t res = prefab->apply(cd_kernel_get_scene(kernel), cd_kernel_get_types(kernel), node_id,
                                    prefab->userdata);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        if (res == CD_ERR_NOTFOUND)  *error_msg = "Node is not a prefab instance";
        else if (res == CD_ERR_IO)   *error_msg = "Cannot read prefab template file";
        else                         *error_msg = "Failed to apply prefab template";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "nodeId", node_id_item->valuestring);
    return result;
}

/* ============================================================================
 * prefab.revert handler
 *
 * Input:  { "nodeId": "idx:gen", "componentType": "MeshRenderer" (optional),
 *           "fieldName": "color" }
 * Output: { "status": "ok" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_prefab_revert(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    if (kernel == NULL || cd_kernel_get_scene(kernel) == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = kernel == NULL ? "Kernel not available" : "No active scene";
        return NULL;
    }

    const cJSON* node_id_item = params ? cJSON_GetObjectItemCaseSensitive(params, "nodeId") : NULL;
    if (node_id_item == NULL || !cJSON_IsString(node_id_item) ||
        node_id_item->valuestring == NULL || node_id_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty 'nodeId' parameter";
        return NULL;
    }

    const cJSON* field_item = params ? cJSON_GetObjectItemCaseSensitive(params, "fieldName") : NULL;
    if (field_item == NULL || !cJSON_IsString(field_item) ||
        field_item->valuestring == NULL || field_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty 'fieldName' parameter";
        return NULL;
    }

    cd_id_t node_id = parse_node_id(node_id_item->valuestring);
    if (!cd_id_is_valid(node_id)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Invalid 'nodeId' format (expected 'index:generation')";
        return NULL;
    }

    /* componentType is optional (NULL means transform/name property) */
    const char* comp_type = NULL;
    const cJSON* comp_item = params ? cJSON_GetObjectItemCaseSensitive(params, "componentType") : NULL;
    if (comp_item != NULL && cJSON_IsString(comp_item) &&
        comp_item->valuestring != NULL && comp_item->valuestring[0] != '\0') {
        comp_type = comp_item->valuestring;
    }

    const cd_prefab_api_t* prefab = cd_kernel_get_prefab_api(kernel);
    if (prefab == NULL || prefab->revert_property == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Prefab API not available (scene_io plugin not loaded)";
        return NULL;
    }
    cd_result_t res = prefab->revert_property(cd_kernel_get_scene(kernel), cd_kernel_get_types(kernel),
                                               node_id, comp_type,
                                               field_item->valuestring,
                                               prefab->userdata);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        if (res == CD_ERR_NOTFOUND) *error_msg = "Node, component, or field not found";
        else if (res == CD_ERR_IO)  *error_msg = "Cannot read prefab template file";
        else                        *error_msg = "Failed to revert property";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "nodeId", node_id_item->valuestring);
    cJSON_AddStringToObject(result, "fieldName", field_item->valuestring);
    if (comp_type != NULL) {
        cJSON_AddStringToObject(result, "componentType", comp_type);
    }
    return result;
}

/* ============================================================================
 * prefab.listOverrides handler
 *
 * Input:  { "nodeId": "idx:gen" }
 * Output: { "status": "ok", "overrides": [ { "componentType": null, "fieldName": "position" }, ... ] }
 * ============================================================================ */

static cJSON* cd_mcp_handle_prefab_list_overrides(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    if (kernel == NULL || cd_kernel_get_scene(kernel) == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = kernel == NULL ? "Kernel not available" : "No active scene";
        return NULL;
    }

    const cJSON* node_id_item = params ? cJSON_GetObjectItemCaseSensitive(params, "nodeId") : NULL;
    if (node_id_item == NULL || !cJSON_IsString(node_id_item) ||
        node_id_item->valuestring == NULL || node_id_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty 'nodeId' parameter";
        return NULL;
    }

    cd_id_t node_id = parse_node_id(node_id_item->valuestring);
    if (!cd_id_is_valid(node_id)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Invalid 'nodeId' format (expected 'index:generation')";
        return NULL;
    }

    const cd_prefab_api_t* prefab = cd_kernel_get_prefab_api(kernel);
    if (prefab == NULL || prefab->get_overrides == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Prefab API not available (scene_io plugin not loaded)";
        return NULL;
    }

    /* First pass: count overrides */
    uint32_t total_count = 0;
    cd_result_t res = prefab->get_overrides(cd_kernel_get_scene(kernel), cd_kernel_get_types(kernel),
                                             node_id, NULL, &total_count, 0,
                                             prefab->userdata);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        if (res == CD_ERR_NOTFOUND) *error_msg = "Node is not a prefab instance";
        else                        *error_msg = "Failed to list overrides";
        return NULL;
    }

    /* Second pass: collect entries */
    cd_prefab_override_entry_t entries[128];
    uint32_t entry_count = 0;
    uint32_t max_entries = total_count < 128 ? total_count : 128;

    res = prefab->get_overrides(cd_kernel_get_scene(kernel), cd_kernel_get_types(kernel),
                                 node_id, entries, &entry_count, max_entries,
                                 prefab->userdata);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to list overrides";
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
    cJSON_AddStringToObject(result, "nodeId", node_id_item->valuestring);

    cJSON* overrides_arr = cJSON_AddArrayToObject(result, "overrides");
    uint32_t write_count = entry_count < max_entries ? entry_count : max_entries;
    for (uint32_t i = 0; i < write_count; i++) {
        cJSON* entry = cJSON_CreateObject();
        if (entries[i].component_type != NULL) {
            cJSON_AddStringToObject(entry, "componentType", entries[i].component_type);
        } else {
            cJSON_AddNullToObject(entry, "componentType");
        }
        cJSON_AddStringToObject(entry, "fieldName", entries[i].field_name);
        cJSON_AddItemToArray(overrides_arr, entry);
    }

    cJSON_AddNumberToObject(result, "totalOverrides", (double)total_count);
    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_prefab_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "prefab.create",
        cd_mcp_handle_prefab_create,
        "Save a node subtree as a prefab file",
        "{\"type\":\"object\",\"properties\":{"
        "\"nodeId\":{\"type\":\"string\",\"description\":\"Node ID in index:generation format\"},"
        "\"filepath\":{\"type\":\"string\",\"description\":\"Output prefab file path\"}"
        "},\"required\":[\"nodeId\",\"filepath\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "prefab.instantiate",
        cd_mcp_handle_prefab_instantiate,
        "Instantiate a prefab from file under a parent node",
        "{\"type\":\"object\",\"properties\":{"
        "\"filepath\":{\"type\":\"string\",\"description\":\"Path to the prefab file\"},"
        "\"parentId\":{\"type\":\"string\",\"description\":\"Parent node ID (defaults to scene root)\"}"
        "},\"required\":[\"filepath\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "prefab.apply",
        cd_mcp_handle_prefab_apply,
        "Re-apply the prefab template to a prefab instance node",
        "{\"type\":\"object\",\"properties\":{"
        "\"nodeId\":{\"type\":\"string\",\"description\":\"Node ID in index:generation format\"}"
        "},\"required\":[\"nodeId\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "prefab.revert",
        cd_mcp_handle_prefab_revert,
        "Revert a single overridden property on a prefab instance",
        "{\"type\":\"object\",\"properties\":{"
        "\"nodeId\":{\"type\":\"string\",\"description\":\"Node ID in index:generation format\"},"
        "\"fieldName\":{\"type\":\"string\",\"description\":\"Name of the field to revert\"},"
        "\"componentType\":{\"type\":\"string\",\"description\":\"Component type (optional, null for transform/name)\"}"
        "},\"required\":[\"nodeId\",\"fieldName\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "prefab.listOverrides",
        cd_mcp_handle_prefab_list_overrides,
        "List all property overrides on a prefab instance node",
        "{\"type\":\"object\",\"properties\":{"
        "\"nodeId\":{\"type\":\"string\",\"description\":\"Node ID in index:generation format\"}"
        "},\"required\":[\"nodeId\"]}");
    if (res != CD_OK) return res;

    return CD_OK;
}
