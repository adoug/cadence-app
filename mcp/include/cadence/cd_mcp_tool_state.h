/* cd_mcp_tool_state.h - Aggregated MCP tool module state
 *
 * PB-3: Eliminate Global Mutable State
 *
 * Instead of per-module static globals, all mutable MCP tool state is
 * collected into cd_mcp_tool_state_t, which lives on cd_kernel_t.
 * Tool handlers access it via cd_kernel_get_mcp_tool_state(kernel).
 */
#ifndef CD_MCP_TOOL_STATE_H
#define CD_MCP_TOOL_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include "cadence/cd_core_types.h"
#include "cadence/cd_mcp_log.h"
#include "cadence/cd_mcp_agent.h"

/* Forward declarations -- use void* to avoid collisions with plugin-local typedefs */

/* ============================================================================
 * Transaction tool state (from cd_mcp_txn_tools.c)
 * ============================================================================ */

typedef struct {
    bool        is_active;
    cd_id_t     txn_id;          /* Command queue transaction ID */
    uint32_t    mcp_txn_id;      /* Current MCP transaction ID number */
    char        label[256];
    uint32_t    checkpoint_undo_top; /* Undo stack top at begin time */
} cd_mcp_txn_state_t;

/* ============================================================================
 * Play tool state (from cd_mcp_play_tools.c)
 * ============================================================================ */

typedef struct {
    uint32_t    session_counter;
    uint32_t    active_session;
} cd_mcp_play_state_t;

/* ============================================================================
 * Script tool state (from cd_mcp_script_tools.c + cd_mcp_observe_tools.c)
 * ============================================================================ */

typedef struct {
    void*   script_mgr;           /**< cd_script_mgr_t* */
    void*   script_reload;        /**< cd_script_reload_t* */
    void*   observe_script_mgr;   /**< cd_script_mgr_t* */
} cd_mcp_script_state_t;

/* ============================================================================
 * Log buffer state (from cd_mcp_log.c)
 * ============================================================================ */

typedef struct cd_mcp_log_state_t {
    cd_mcp_log_entry_t  entries[CD_MCP_LOG_CAPACITY];
    uint32_t            head;       /* Next write position */
    uint32_t            count;      /* Entries in buffer (up to CAPACITY) */
    uint64_t            seq;        /* Monotonic sequence counter */
} cd_mcp_log_state_t;

/* ============================================================================
 * Log tools state (from cd_mcp_log_tools.c)
 * ============================================================================ */

typedef struct {
    uint64_t    stream_cursors[CD_MCP_MAX_AGENTS];
} cd_mcp_log_tools_state_t;

/* ============================================================================
 * Agent state (from cd_mcp_agent.c)
 * ============================================================================ */

typedef struct cd_mcp_agent_state_t {
    uint32_t    current_agent_id;
} cd_mcp_agent_state_t;

/* ============================================================================
 * System tools state (from cd_mcp_system_tools.c)
 * ============================================================================ */

typedef struct {
    double      start_time;
    bool        start_time_set;
} cd_mcp_system_state_t;

/* ============================================================================
 * Project tools state (from cd_mcp_project_tools.c)
 * ============================================================================ */

typedef struct {
    char        project_path[1024];
} cd_mcp_project_state_t;

/* ============================================================================
 * Aggregated state — one instance lives on cd_kernel_t
 * ============================================================================ */

struct cd_mcp_tool_state_t {
    /* Transaction state */
    uint32_t              txn_counter;
    cd_mcp_txn_state_t    txn_states[CD_MCP_MAX_AGENTS];

    /* Play mode state */
    cd_mcp_play_state_t   play;

    /* Script pointers */
    cd_mcp_script_state_t script;

    /* Log buffer */
    cd_mcp_log_state_t    log;

    /* Log tools cursors */
    cd_mcp_log_tools_state_t log_tools;

    /* Agent tracking */
    cd_mcp_agent_state_t  agent;

    /* System tools */
    cd_mcp_system_state_t system;

    /* Project tools */
    cd_mcp_project_state_t project;
};

typedef struct cd_mcp_tool_state_t cd_mcp_tool_state_t;

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/** Initialize all tool state to defaults. */
void cd_mcp_tool_state_init(cd_mcp_tool_state_t* state);

#endif /* CD_MCP_TOOL_STATE_H */
