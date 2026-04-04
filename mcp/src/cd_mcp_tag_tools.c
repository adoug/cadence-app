/* cd_mcp_tag_tools.c - Cadence Engine MCP tag tool handlers
 *
 * Implements:
 *   - tag.add    : Add one or more tags to a node
 *   - tag.remove : Remove one or more tags from a node
 *
 * All handlers follow the cd_mcp_tool_handler_t signature.
 * Tag operations work directly on cd_kernel_get_scene(kernel).
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_error.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_scene.h"
#include "cadence/cd_commands.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

/* ============================================================================
 * Internal: Format a cd_id_t as "index:generation" string
 * ============================================================================ */

#define CD_ID_STR_BUF_SIZE 24

static const char* cd_id_format(cd_id_t id, char* buf, size_t buf_size) {
    uint32_t index = cd_id_index(id);
    uint32_t gen   = cd_id_generation(id);
    snprintf(buf, buf_size, "%u:%u", index, gen);
    return buf;
}

/* ============================================================================
 * Internal: Parse "index:generation" string to cd_id_t
 * ============================================================================ */

static cd_id_t cd_id_parse(const char* str) {
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
 * Internal: Build a JSON array of the node's current tags, filtering out
 * internal "type:" prefixed tags.
 * ============================================================================ */

static cJSON* cd_node_tags_to_json(cd_node_t* node) {
    cJSON* tags = cJSON_CreateArray();
    if (tags == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < node->tag_count; i++) {
        if (strncmp(node->tag_names[i].buf, "type:", 5) != 0) {
            cJSON_AddItemToArray(tags, cJSON_CreateString(node->tag_names[i].buf));
        }
    }
    return tags;
}

/* ============================================================================
 * tag.add handler
 *
 * Input:  { "id": "2:3", "tags": ["enemy", "damageable"] }
 * Output: { "nodeId": "2:3", "added": ["enemy", "damageable"],
 *           "tags": ["enemy", "damageable", ...] }
 *
 * Adds one or more tags to a node. Tags that the node already has are
 * silently skipped (idempotent). Returns the list of tags actually added
 * and the full current tag list.
 * ============================================================================ */

static cJSON* cd_mcp_handle_tag_add(
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
        *error_msg  = cd_mcp_error_fmt("No active scene",
                          "Tag operations require a scene to be loaded.",
                          "Call scene.create3d or scene.open to load a scene.");
        return NULL;
    }

    /* Extract "id" (required) */
    const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (id_item == NULL || !cJSON_IsString(id_item) ||
        id_item->valuestring == NULL || id_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: id";
        return NULL;
    }

    cd_id_t node_id = cd_id_parse(id_item->valuestring);
    if (!cd_id_is_valid(node_id)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Invalid id format (expected \"index:generation\")";
        return NULL;
    }

    /* Verify node exists */
    cd_node_t* node = cd_node_get(cd_kernel_get_scene(kernel), node_id);
    if (node == NULL) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "Node with id '%s' does not exist in the scene.",
                 id_item->valuestring);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Node not found", detail,
                          "Use node.find to list available nodes.");
        return NULL;
    }

    /* Extract "tags" (required, must be array) */
    const cJSON* tags_item = cJSON_GetObjectItemCaseSensitive(params, "tags");
    if (tags_item == NULL || !cJSON_IsArray(tags_item)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or invalid required parameter: tags (must be array)";
        return NULL;
    }

    /* Add each tag */
    cJSON* added = cJSON_CreateArray();
    if (added == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON array";
        return NULL;
    }

    const cJSON* tag_item = NULL;
    cJSON_ArrayForEach(tag_item, tags_item) {
        if (!cJSON_IsString(tag_item) || tag_item->valuestring == NULL ||
            tag_item->valuestring[0] == '\0') {
            continue; /* Skip non-string or empty entries */
        }

        /* Add tag via command queue */
        cd_command_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = CD_CMD_TAG_ADD;
        cmd.target = node_id;
        cmd.payload.tag_op.tag = cd_name_from_cstr(tag_item->valuestring);

        cd_result_t res = cd_command_execute_sync(
            cd_kernel_get_commands(kernel), &cmd, cd_kernel_get_scene(kernel), cd_kernel_get_events(kernel));
        if (res == CD_OK) {
            cJSON_AddItemToArray(added, cJSON_CreateString(tag_item->valuestring));
        }
    }

    /* Re-fetch node pointer in case realloc moved it */
    node = cd_node_get(cd_kernel_get_scene(kernel), node_id);
    if (node == NULL) {
        cJSON_Delete(added);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Node unexpectedly invalid after tag add";
        return NULL;
    }

    /* Build the response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        cJSON_Delete(added);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    char id_buf[CD_ID_STR_BUF_SIZE];
    cd_id_format(node_id, id_buf, sizeof(id_buf));
    cJSON_AddStringToObject(result, "nodeId", id_buf);
    cJSON_AddItemToObject(result, "added", added);

    cJSON* current_tags = cd_node_tags_to_json(node);
    if (current_tags != NULL) {
        cJSON_AddItemToObject(result, "tags", current_tags);
    }

    return result;
}

/* ============================================================================
 * tag.remove handler
 *
 * Input:  { "id": "2:3", "tags": ["enemy"] }
 * Output: { "nodeId": "2:3", "removed": ["enemy"],
 *           "tags": ["damageable", ...] }
 *
 * Removes one or more tags from a node. Tags that the node does not have
 * are silently skipped. Returns the list of tags actually removed and the
 * full current tag list.
 * ============================================================================ */

static cJSON* cd_mcp_handle_tag_remove(
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
        *error_msg  = cd_mcp_error_fmt("No active scene",
                          "Tag operations require a scene to be loaded.",
                          "Call scene.create3d or scene.open to load a scene.");
        return NULL;
    }

    /* Extract "id" (required) */
    const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (id_item == NULL || !cJSON_IsString(id_item) ||
        id_item->valuestring == NULL || id_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: id";
        return NULL;
    }

    cd_id_t node_id = cd_id_parse(id_item->valuestring);
    if (!cd_id_is_valid(node_id)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Invalid id format (expected \"index:generation\")";
        return NULL;
    }

    /* Verify node exists */
    cd_node_t* node = cd_node_get(cd_kernel_get_scene(kernel), node_id);
    if (node == NULL) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "Node with id '%s' does not exist in the scene.",
                 id_item->valuestring);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Node not found", detail,
                          "Use node.find to list available nodes.");
        return NULL;
    }

    /* Extract "tags" (required, must be array) */
    const cJSON* tags_item = cJSON_GetObjectItemCaseSensitive(params, "tags");
    if (tags_item == NULL || !cJSON_IsArray(tags_item)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or invalid required parameter: tags (must be array)";
        return NULL;
    }

    /* Remove each tag */
    cJSON* removed = cJSON_CreateArray();
    if (removed == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON array";
        return NULL;
    }

    const cJSON* tag_item = NULL;
    cJSON_ArrayForEach(tag_item, tags_item) {
        if (!cJSON_IsString(tag_item) || tag_item->valuestring == NULL ||
            tag_item->valuestring[0] == '\0') {
            continue; /* Skip non-string or empty entries */
        }

        /* Remove tag via command queue */
        cd_command_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = CD_CMD_TAG_REMOVE;
        cmd.target = node_id;
        cmd.payload.tag_op.tag = cd_name_from_cstr(tag_item->valuestring);

        cd_result_t res = cd_command_execute_sync(
            cd_kernel_get_commands(kernel), &cmd, cd_kernel_get_scene(kernel), cd_kernel_get_events(kernel));
        if (res == CD_OK) {
            cJSON_AddItemToArray(removed, cJSON_CreateString(tag_item->valuestring));
        }
        /* CD_ERR_NOTFOUND means the tag was not present -- silently skip */
    }

    /* Re-fetch node pointer in case realloc moved it */
    node = cd_node_get(cd_kernel_get_scene(kernel), node_id);
    if (node == NULL) {
        cJSON_Delete(removed);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Node unexpectedly invalid after tag remove";
        return NULL;
    }

    /* Build the response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        cJSON_Delete(removed);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    char id_buf[CD_ID_STR_BUF_SIZE];
    cd_id_format(node_id, id_buf, sizeof(id_buf));
    cJSON_AddStringToObject(result, "nodeId", id_buf);
    cJSON_AddItemToObject(result, "removed", removed);

    cJSON* current_tags = cd_node_tags_to_json(node);
    if (current_tags != NULL) {
        cJSON_AddItemToObject(result, "tags", current_tags);
    }

    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_tag_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "tag.add",
        cd_mcp_handle_tag_add,
        "Add one or more tags to a node",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\",\"description\":\"Node ID (index:generation)\"},"
        "\"tags\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Tags to add\"}"
        "},\"required\":[\"id\",\"tags\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "tag.remove",
        cd_mcp_handle_tag_remove,
        "Remove one or more tags from a node",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\",\"description\":\"Node ID (index:generation)\"},"
        "\"tags\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Tags to remove\"}"
        "},\"required\":[\"id\",\"tags\"]}");
    if (res != CD_OK) return res;

    return CD_OK;
}
