/* cd_mcp_tool_state.c - MCP tool state initialization
 *
 * PB-3: Eliminate Global Mutable State
 */

#include "cadence/cd_mcp_tool_state.h"
#include "cadence/cd_mcp_log.h"
#include "cadence/cd_mcp_agent.h"
#include <string.h>

void cd_mcp_tool_state_init(cd_mcp_tool_state_t* state) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->agent.current_agent_id = CD_MCP_AGENT_STDIO;

    /* Wire up the log and agent modules to use this state instance */
    cd_mcp_log_set_state(&state->log);
    cd_mcp_agent_set_state(&state->agent);
}
