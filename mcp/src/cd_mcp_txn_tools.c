/* cd_mcp_txn_tools.c - Cadence Engine MCP transaction tool handlers
 *
 * Implements:
 *   - txn.begin    : Begin a new transaction
 *   - txn.commit   : Commit (finalize) an active transaction
 *   - txn.rollback : Roll back an active transaction, undoing all commands
 *
 * Task 19.2: Transactions are isolated per-agent. Each connection (agent)
 * can have one active transaction. Multiple agents can have concurrent
 * transactions without conflict.
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
#include "cadence/cd_commands.h"
#include "cadence/cd_lock_table.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Per-agent transaction state
 *
 * Each agent (connection) gets its own transaction slot. The current agent
 * is determined by cd_mcp_get_current_agent(), set by the transport layer
 * before dispatching each request.
 *
 * PB-3: State moved from static globals into cd_kernel_get_mcp_tool_state(kernel).
 * ============================================================================ */

/** Fallback for tests that don't set up mcp_tool_state. */
static cd_mcp_tool_state_t s_txn_fallback;

/** Get the txn state for the current agent from kernel tool state. */
static cd_mcp_txn_state_t* cd_mcp_txn_current(struct cd_kernel_t* kernel) {
    cd_mcp_tool_state_t* ts = cd_kernel_get_mcp_tool_state(kernel);
    if (!ts) ts = &s_txn_fallback;
    uint32_t agent = cd_mcp_get_current_agent();
    if (agent >= CD_MCP_MAX_AGENTS) {
        return &ts->txn_states[0]; /* Fallback to stdio slot */
    }
    return &ts->txn_states[agent];
}

/** Get txn counter from tool state with fallback. */
static uint32_t* get_txn_counter(struct cd_kernel_t* kernel) {
    cd_mcp_tool_state_t* ts = cd_kernel_get_mcp_tool_state(kernel);
    if (!ts) ts = &s_txn_fallback;
    return &ts->txn_counter;
}

/* ============================================================================
 * Internal: Format MCP transaction ID as string "txn_N"
 * ============================================================================ */

#define CD_TXN_ID_BUF_SIZE 32

static void cd_mcp_txn_id_format(uint32_t id, char* buf, size_t buf_size) {
    snprintf(buf, buf_size, "txn_%u", id);
}

/* ============================================================================
 * Internal: Parse MCP transaction ID from string "txn_N"
 *
 * Returns the numeric part, or UINT32_MAX on failure.
 * ============================================================================ */

static uint32_t cd_mcp_txn_id_parse(const char* str) {
    if (str == NULL || strncmp(str, "txn_", 4) != 0) {
        return UINT32_MAX;
    }
    const char* p = str + 4;
    if (*p == '\0') {
        return UINT32_MAX;
    }
    uint32_t val = 0;
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (uint32_t)(*p - '0');
        p++;
    }
    /* If we didn't consume any digits, or there are trailing characters */
    if (p == str + 4 || *p != '\0') {
        return UINT32_MAX;
    }
    return val;
}

/* ============================================================================
 * txn.begin handler
 *
 * Input:  { "label": "create enemies" }  (label is optional)
 * Output: { "txnId": "txn_1", "label": "create enemies", "status": "active" }
 *
 * Begins a new transaction. Only one active transaction at a time.
 * ============================================================================ */

static cJSON* cd_mcp_handle_txn_begin(
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

    /* Get per-agent txn state */
    cd_mcp_txn_state_t* txn = cd_mcp_txn_current(kernel);

    /* Check for already-active transaction on this agent */
    if (txn->is_active) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Transaction already active";
        return NULL;
    }

    /* Extract optional "label" */
    const char* label = "";
    if (params != NULL) {
        const cJSON* label_item = cJSON_GetObjectItemCaseSensitive(params, "label");
        if (label_item != NULL && cJSON_IsString(label_item) &&
            label_item->valuestring != NULL) {
            label = label_item->valuestring;
        }
    }

    /* Begin the command queue transaction */
    cd_id_t cmd_txn_id = cd_transaction_begin(cd_kernel_get_commands(kernel), label);
    if (!cd_id_is_valid(cmd_txn_id)) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to begin command queue transaction";
        return NULL;
    }

    /* Record state for this agent */
    txn->is_active = true;
    txn->txn_id = cmd_txn_id;
    (*get_txn_counter(kernel))++;
    txn->mcp_txn_id = (*get_txn_counter(kernel));
    {
        size_t len = strlen(label);
        if (len >= sizeof(txn->label)) {
            len = sizeof(txn->label) - 1;
        }
        memcpy(txn->label, label, len);
        txn->label[len] = '\0';
    }
    txn->checkpoint_undo_top = cd_kernel_get_commands(kernel)->undo_top;

    /* Build response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    char txn_id_buf[CD_TXN_ID_BUF_SIZE];
    cd_mcp_txn_id_format(txn->mcp_txn_id, txn_id_buf, sizeof(txn_id_buf));

    cJSON_AddStringToObject(result, "txnId", txn_id_buf);
    if (txn->label[0] != '\0') {
        cJSON_AddStringToObject(result, "label", txn->label);
    }
    cJSON_AddStringToObject(result, "status", "active");

    /* Include agent ID in response */
    char agent_buf[16];
    cd_mcp_agent_id_format(cd_mcp_get_current_agent(), agent_buf, sizeof(agent_buf));
    cJSON_AddStringToObject(result, "agentId", agent_buf);

    return result;
}

/* ============================================================================
 * txn.commit handler
 *
 * Input:  { "txnId": "txn_1" }  (txnId is optional)
 * Output: { "txnId": "txn_1", "status": "committed", "commandCount": 3 }
 *
 * Commits the active transaction. Commands are kept (already applied).
 * ============================================================================ */

static cJSON* cd_mcp_handle_txn_commit(
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

    /* Get per-agent txn state */
    cd_mcp_txn_state_t* txn = cd_mcp_txn_current(kernel);

    /* Check for active transaction on this agent */
    if (!txn->is_active) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "No active transaction";
        return NULL;
    }

    /* If txnId provided, verify it matches */
    if (params != NULL) {
        const cJSON* txn_id_item = cJSON_GetObjectItemCaseSensitive(params, "txnId");
        if (txn_id_item != NULL && cJSON_IsString(txn_id_item) &&
            txn_id_item->valuestring != NULL) {
            uint32_t provided_id = cd_mcp_txn_id_parse(txn_id_item->valuestring);
            if (provided_id != txn->mcp_txn_id) {
                *error_code = CD_JSONRPC_INVALID_PARAMS;
                *error_msg  = "Transaction ID mismatch";
                return NULL;
            }
        }
    }

    /* Count commands in transaction before committing */
    uint32_t command_count = 0;
    cd_command_queue_t* q = cd_kernel_get_commands(kernel);
    for (uint32_t i = 0; i < q->txn_count; i++) {
        if (q->transactions[i].id == txn->txn_id &&
            q->transactions[i].active) {
            command_count = q->transactions[i].count;
            break;
        }
    }

    /* Commit the command queue transaction */
    cd_result_t res = cd_transaction_commit(cd_kernel_get_commands(kernel), txn->txn_id);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to commit command queue transaction";
        return NULL;
    }

    /* P1-6: Release all node locks held by this transaction */
    cd_lock_release_all(cd_kernel_get_lock_table(kernel), txn->mcp_txn_id);

    /* Build response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    char txn_id_buf[CD_TXN_ID_BUF_SIZE];
    cd_mcp_txn_id_format(txn->mcp_txn_id, txn_id_buf, sizeof(txn_id_buf));

    cJSON_AddStringToObject(result, "txnId", txn_id_buf);
    cJSON_AddStringToObject(result, "status", "committed");
    cJSON_AddNumberToObject(result, "commandCount", (double)command_count);

    /* Clear active transaction state for this agent */
    txn->is_active = false;
    txn->txn_id = CD_ID_INVALID;

    return result;
}

/* ============================================================================
 * txn.rollback handler
 *
 * Input:  { "txnId": "txn_1" }  (txnId is optional)
 * Output: { "txnId": "txn_1", "status": "rolled_back", "commandCount": 3 }
 *
 * Rolls back the active transaction, undoing all commands since txn.begin.
 * ============================================================================ */

static cJSON* cd_mcp_handle_txn_rollback(
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

    /* Get per-agent txn state */
    cd_mcp_txn_state_t* txn = cd_mcp_txn_current(kernel);

    /* Check for active transaction on this agent */
    if (!txn->is_active) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "No active transaction";
        return NULL;
    }

    /* If txnId provided, verify it matches */
    if (params != NULL) {
        const cJSON* txn_id_item = cJSON_GetObjectItemCaseSensitive(params, "txnId");
        if (txn_id_item != NULL && cJSON_IsString(txn_id_item) &&
            txn_id_item->valuestring != NULL) {
            uint32_t provided_id = cd_mcp_txn_id_parse(txn_id_item->valuestring);
            if (provided_id != txn->mcp_txn_id) {
                *error_code = CD_JSONRPC_INVALID_PARAMS;
                *error_msg  = "Transaction ID mismatch";
                return NULL;
            }
        }
    }

    /* Count commands in transaction before rolling back */
    uint32_t command_count = 0;
    cd_command_queue_t* q = cd_kernel_get_commands(kernel);
    for (uint32_t i = 0; i < q->txn_count; i++) {
        if (q->transactions[i].id == txn->txn_id &&
            q->transactions[i].active) {
            command_count = q->transactions[i].count;
            break;
        }
    }

    /* Rollback the command queue transaction */
    cd_result_t res = cd_transaction_rollback(
        cd_kernel_get_commands(kernel), txn->txn_id,
        cd_kernel_get_scene(kernel), NULL);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to rollback command queue transaction";
        return NULL;
    }

    /* P1-6: Release all node locks held by this transaction */
    cd_lock_release_all(cd_kernel_get_lock_table(kernel), txn->mcp_txn_id);

    /* Build response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    char txn_id_buf[CD_TXN_ID_BUF_SIZE];
    cd_mcp_txn_id_format(txn->mcp_txn_id, txn_id_buf, sizeof(txn_id_buf));

    cJSON_AddStringToObject(result, "txnId", txn_id_buf);
    cJSON_AddStringToObject(result, "status", "rolled_back");
    cJSON_AddNumberToObject(result, "commandCount", (double)command_count);

    /* Clear active transaction state for this agent */
    txn->is_active = false;
    txn->txn_id = CD_ID_INVALID;

    return result;
}

/* ============================================================================
 * txn.locks handler (P1-6)
 *
 * Input:  { "txnId": "txn_1" }  (txnId is optional, defaults to current)
 * Output: { "txnId": "txn_1", "locks": [ { "nodeId": "2:3" }, ... ] }
 *
 * Lists all node locks held by a transaction.
 * ============================================================================ */

static cJSON* cd_mcp_handle_txn_locks(
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

    /* Get per-agent txn state */
    cd_mcp_txn_state_t* txn = cd_mcp_txn_current(kernel);

    /* Determine which txn_id to query */
    uint32_t query_txn_id = 0;
    bool has_txn = false;

    if (params != NULL) {
        const cJSON* txn_id_item = cJSON_GetObjectItemCaseSensitive(params, "txnId");
        if (txn_id_item != NULL && cJSON_IsString(txn_id_item) &&
            txn_id_item->valuestring != NULL) {
            query_txn_id = cd_mcp_txn_id_parse(txn_id_item->valuestring);
            if (query_txn_id == UINT32_MAX) {
                *error_code = CD_JSONRPC_INVALID_PARAMS;
                *error_msg  = "Invalid txnId format";
                return NULL;
            }
            has_txn = true;
        }
    }

    if (!has_txn) {
        /* Default to current agent's active transaction */
        if (!txn->is_active) {
            *error_code = CD_JSONRPC_INVALID_PARAMS;
            *error_msg  = "No active transaction and no txnId provided";
            return NULL;
        }
        query_txn_id = txn->mcp_txn_id;
    }

    /* Query locks */
    cd_node_lock_t locks[256];
    uint32_t lock_count = 0;
    cd_lock_get_by_txn(cd_kernel_get_lock_table(kernel), query_txn_id,
                        locks, &lock_count, 256);

    /* Build response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    char txn_id_buf[CD_TXN_ID_BUF_SIZE];
    cd_mcp_txn_id_format(query_txn_id, txn_id_buf, sizeof(txn_id_buf));
    cJSON_AddStringToObject(result, "txnId", txn_id_buf);

    cJSON* locks_array = cJSON_CreateArray();
    if (locks_array != NULL) {
        for (uint32_t i = 0; i < lock_count; i++) {
            cJSON* entry = cJSON_CreateObject();
            if (entry != NULL) {
                char id_buf[24];
                uint32_t idx = cd_id_index(locks[i].node_id);
                uint32_t gen = cd_id_generation(locks[i].node_id);
                snprintf(id_buf, sizeof(id_buf), "%u:%u", idx, gen);
                cJSON_AddStringToObject(entry, "nodeId", id_buf);
                cJSON_AddItemToArray(locks_array, entry);
            }
        }
    }
    cJSON_AddItemToObject(result, "locks", locks_array);
    cJSON_AddNumberToObject(result, "count", (double)lock_count);

    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_txn_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "txn.begin",
        cd_mcp_handle_txn_begin,
        "Begin a new transaction for grouping commands with rollback support",
        "{\"type\":\"object\",\"properties\":{"
        "\"label\":{\"type\":\"string\",\"description\":\"Optional human-readable label for the transaction\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "txn.commit",
        cd_mcp_handle_txn_commit,
        "Commit the active transaction, finalizing all enqueued commands",
        "{\"type\":\"object\",\"properties\":{"
        "\"txnId\":{\"type\":\"string\",\"description\":\"Transaction ID to commit (defaults to current)\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "txn.rollback",
        cd_mcp_handle_txn_rollback,
        "Roll back the active transaction, undoing all commands since begin",
        "{\"type\":\"object\",\"properties\":{"
        "\"txnId\":{\"type\":\"string\",\"description\":\"Transaction ID to roll back (defaults to current)\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "txn.locks",
        cd_mcp_handle_txn_locks,
        "List all node locks held by a transaction",
        "{\"type\":\"object\",\"properties\":{"
        "\"txnId\":{\"type\":\"string\",\"description\":\"Transaction ID to query (defaults to current)\"}"
        "}}");
    if (res != CD_OK) return res;

    return CD_OK;
}

/* ============================================================================
 * Reset (for testing -- clear global state between tests)
 * ============================================================================ */

void cd_mcp_txn_reset_state(void) {
    /* Reset the fallback state (used by tests without mcp_tool_state) */
    memset(&s_txn_fallback, 0, sizeof(s_txn_fallback));
    cd_mcp_set_current_agent(CD_MCP_AGENT_STDIO);
}
