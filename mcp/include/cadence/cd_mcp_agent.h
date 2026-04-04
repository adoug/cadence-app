/* cd_mcp_agent.h - Cadence Engine MCP per-connection agent tracking
 *
 * Task 19.2: Multi-agent coordination with per-connection txn isolation.
 *
 * Each transport connection gets a unique agent ID. The transports set the
 * current agent before dispatching a request; tool handlers (especially
 * txn.*) use the current agent to scope their state.
 *
 * Agent ID ranges:
 *   0        = stdio transport (always present)
 *   1..16    = TCP client slots
 *   17..32   = WebSocket client slots
 *   33..48   = Unix domain socket client slots
 */
#ifndef CD_MCP_AGENT_H
#define CD_MCP_AGENT_H

#include <stdint.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

/** Agent ID for the stdio transport (local, always slot 0). */
#define CD_MCP_AGENT_STDIO      0

/** First agent ID for TCP client slots. */
#define CD_MCP_AGENT_TCP_BASE   1

/** First agent ID for WebSocket client slots. */
#define CD_MCP_AGENT_WS_BASE    17

/** First agent ID for Unix domain socket client slots. */
#define CD_MCP_AGENT_SOCKET_BASE 33

/** Maximum number of concurrent agents across all transports. */
#define CD_MCP_MAX_AGENTS       49

/* ============================================================================
 * Current agent context
 *
 * Single-threaded dispatch: transports set the current agent before calling
 * cd_mcp_process_request(). Tool handlers read it to scope per-agent state.
 * ============================================================================ */

/**
 * Set the current agent ID for the request being dispatched.
 *
 * Called by transports (TCP, WS) before dispatching each JSON-RPC request.
 * The stdio transport uses agent 0 (the default).
 *
 * @param agent_id  Agent ID (0 = stdio, 1..16 = TCP, 17..32 = WS, 33..48 = socket).
 */
void cd_mcp_set_current_agent(uint32_t agent_id);

/**
 * Get the current agent ID.
 *
 * Called by tool handlers (e.g., txn.*) to scope per-agent state.
 *
 * @return Current agent ID.
 */
uint32_t cd_mcp_get_current_agent(void);

/**
 * Format an agent ID as a human-readable string "agent_N".
 *
 * @param agent_id  Agent ID to format.
 * @param buf       Output buffer.
 * @param buf_size  Size of output buffer.
 */
void cd_mcp_agent_id_format(uint32_t agent_id, char* buf, uint32_t buf_size);

/* ============================================================================
 * State management (PB-3)
 * ============================================================================ */

/* Forward declaration (full definition in cd_mcp_tool_state.h) */
struct cd_mcp_agent_state_t;

/** Set the active agent state. NULL reverts to the internal fallback. */
void cd_mcp_agent_set_state(struct cd_mcp_agent_state_t* state);

#endif /* CD_MCP_AGENT_H */
