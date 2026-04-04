/* cd_mcp_agent.c - Cadence Engine MCP agent tracking implementation
 *
 * Task 19.2: Module-level current agent ID for single-threaded dispatch.
 * PB-3: Uses a module-level pointer to kernel-owned state when available,
 *        with a static fallback for standalone / early-init usage.
 *
 * Threading (Task S3.5): s_agent->current_agent_id is a plain variable,
 * NOT protected by a mutex. This is safe because all MCP dispatch is
 * single-threaded (main thread polling). If multi-threaded dispatch is
 * ever added, this must become thread-local storage (TLS).
 */

#include "cadence/cd_mcp_tool_state.h"
#include "cadence/cd_mcp_agent.h"
#include <stdio.h>

/* ============================================================================
 * Module state — fallback for when no kernel tool state is available
 * ============================================================================ */

static cd_mcp_agent_state_t s_fallback = { CD_MCP_AGENT_STDIO };
static cd_mcp_agent_state_t* s_agent = &s_fallback;

void cd_mcp_agent_set_state(cd_mcp_agent_state_t* state) {
    s_agent = state ? state : &s_fallback;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void cd_mcp_set_current_agent(uint32_t agent_id) {
    s_agent->current_agent_id = agent_id;
}

uint32_t cd_mcp_get_current_agent(void) {
    return s_agent->current_agent_id;
}

void cd_mcp_agent_id_format(uint32_t agent_id, char* buf, uint32_t buf_size) {
    if (buf == NULL || buf_size == 0) {
        return;
    }
    snprintf(buf, buf_size, "agent_%u", agent_id);
}
