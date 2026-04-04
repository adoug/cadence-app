/* cd_mcp_lock_tools.c - Cadence Engine MCP node lock tool handlers
 *
 * P1-6: Multi-Agent Transaction Locking
 *
 * Implements:
 *   - node.lock   : Explicitly lock a node for the current transaction
 *   - node.unlock : Explicitly unlock a node
 *
 * Locks are advisory and only apply within transactions. Commands outside
 * transactions bypass locking for backwards compatibility.
 *
 * All handlers follow the cd_mcp_tool_handler_t signature.
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_agent.h"
#include "cadence/cd_mcp_tool_state.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_scene.h"
#include "cadence/cd_lock_table.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

/* ============================================================================
 * Internal: Parse "index:generation" string to cd_id_t
 * ============================================================================ */

static cd_id_t cd_lock_parse_id(const char* str) {
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
 * Internal: Get the MCP txn state for the current agent
 * ============================================================================ */

static cd_mcp_txn_state_t* cd_lock_get_txn(struct cd_kernel_t* kernel) {
    cd_mcp_tool_state_t* ts = cd_kernel_get_mcp_tool_state(kernel);
    /* Fallback not needed for lock tools -- require mcp_tool_state */
    if (!ts) return NULL;
    uint32_t agent = cd_mcp_get_current_agent();
    if (agent >= CD_MCP_MAX_AGENTS) {
        return &ts->txn_states[0];
    }
    return &ts->txn_states[agent];
}

/* ============================================================================
 * node.lock handler
 *
 * Input:  { "id": "2:3", "txnId": "txn_1" }
 *         txnId is optional (defaults to current agent's active transaction)
 * Output: { "status": "ok", "nodeId": "2:3", "txnId": "txn_1" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_node_lock(
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

    /* Extract "id" (required) */
    const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (id_item == NULL || !cJSON_IsString(id_item) ||
        id_item->valuestring == NULL || id_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: id";
        return NULL;
    }

    cd_id_t node_id = cd_lock_parse_id(id_item->valuestring);
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

    /* Determine transaction ID */
    uint32_t txn_id = 0;
    cd_mcp_txn_state_t* txn = cd_lock_get_txn(kernel);
    if (txn != NULL && txn->is_active) {
        txn_id = txn->mcp_txn_id;
    }

    /* Check if txnId was explicitly provided */
    const cJSON* txn_id_item = cJSON_GetObjectItemCaseSensitive(params, "txnId");
    if (txn_id_item != NULL && cJSON_IsString(txn_id_item) &&
        txn_id_item->valuestring != NULL) {
        /* Parse "txn_N" format */
        const char* s = txn_id_item->valuestring;
        if (strncmp(s, "txn_", 4) == 0) {
            txn_id = 0;
            const char* p = s + 4;
            while (*p >= '0' && *p <= '9') {
                txn_id = txn_id * 10 + (uint32_t)(*p - '0');
                p++;
            }
        }
    }

    if (txn_id == 0) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "No active transaction -- node.lock requires a transaction";
        return NULL;
    }

    /* Acquire the lock */
    cd_result_t res = cd_lock_acquire(cd_kernel_get_lock_table(kernel), node_id, txn_id);
    if (res == CD_ERR_LOCKED) {
        uint32_t holder = 0;
        cd_lock_is_held(cd_kernel_get_lock_table(kernel), node_id, &holder);

        /* Build error with details about the holder */
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Node is locked by another transaction";
        return NULL;
    }
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to acquire lock";
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
    cJSON_AddStringToObject(result, "nodeId", id_item->valuestring);

    char txn_buf[32];
    snprintf(txn_buf, sizeof(txn_buf), "txn_%u", txn_id);
    cJSON_AddStringToObject(result, "txnId", txn_buf);

    return result;
}

/* ============================================================================
 * node.unlock handler
 *
 * Input:  { "id": "2:3", "txnId": "txn_1" }
 *         txnId is optional (defaults to current agent's active transaction)
 * Output: { "status": "ok", "nodeId": "2:3" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_node_unlock(
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

    /* Extract "id" (required) */
    const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (id_item == NULL || !cJSON_IsString(id_item) ||
        id_item->valuestring == NULL || id_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: id";
        return NULL;
    }

    cd_id_t node_id = cd_lock_parse_id(id_item->valuestring);
    if (!cd_id_is_valid(node_id)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Invalid id format (expected \"index:generation\")";
        return NULL;
    }

    /* Determine transaction ID */
    uint32_t txn_id = 0;
    cd_mcp_txn_state_t* txn = cd_lock_get_txn(kernel);
    if (txn != NULL && txn->is_active) {
        txn_id = txn->mcp_txn_id;
    }

    const cJSON* txn_id_item = cJSON_GetObjectItemCaseSensitive(params, "txnId");
    if (txn_id_item != NULL && cJSON_IsString(txn_id_item) &&
        txn_id_item->valuestring != NULL) {
        const char* s = txn_id_item->valuestring;
        if (strncmp(s, "txn_", 4) == 0) {
            txn_id = 0;
            const char* p = s + 4;
            while (*p >= '0' && *p <= '9') {
                txn_id = txn_id * 10 + (uint32_t)(*p - '0');
                p++;
            }
        }
    }

    if (txn_id == 0) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "No active transaction -- node.unlock requires a transaction";
        return NULL;
    }

    /* Release the lock */
    cd_result_t res = cd_lock_release(cd_kernel_get_lock_table(kernel), node_id, txn_id);
    if (res == CD_ERR_NOTFOUND) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Node is not locked by this transaction";
        return NULL;
    }
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to release lock";
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
    cJSON_AddStringToObject(result, "nodeId", id_item->valuestring);

    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_lock_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "node.lock", cd_mcp_handle_node_lock,
        "Lock a scene node for exclusive access within a transaction",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\",\"description\":\"Node ID in index:generation format\"},"
        "\"txnId\":{\"type\":\"string\",\"description\":\"Transaction ID (default: current agent transaction)\"}"
        "},\"required\":[\"id\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "node.unlock", cd_mcp_handle_node_unlock,
        "Unlock a previously locked scene node",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\",\"description\":\"Node ID in index:generation format\"},"
        "\"txnId\":{\"type\":\"string\",\"description\":\"Transaction ID (default: current agent transaction)\"}"
        "},\"required\":[\"id\"]}");
    if (res != CD_OK) return res;

    return CD_OK;
}
