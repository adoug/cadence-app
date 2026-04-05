#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
/* cd_mcp_net_tools.c - MCP networking tool handlers
 *
 * Implements:
 *   net.host               — Start hosting a session
 *   net.connect            — Connect to a hosted session
 *   net.disconnect         — Disconnect from current session
 *   net.status             — Query connection state, latency, peer list
 *   net.diagnostics        — Detailed network statistics + RTT history
 *   net.simulate_conditions — Inject artificial latency/loss/jitter for testing
 */

#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_net_ext_api.h"
#include "cadence/cd_net_types.h"
#include "cadence/cd_net_session.h"
#include "cadence/cd_net_replication.h"
#include "cadence/cd_net_diagnostics.h"
#include "cadence/cd_net_lobby.h"
#include "cadence/cd_net_nat.h"
#include "cadence/cd_net_relay.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

/* All networking operations go through the cd_net_ext_api_t vtable. */
static const cd_net_ext_api_t* net_api(struct cd_kernel_t* kernel) {
    return kernel ? cd_kernel_get_net_ext_api(kernel) : NULL;
}

/* Sub-object getters — for reading struct fields only. */
static cd_net_session_t* net_get_session(struct cd_kernel_t* kernel) {
    const cd_net_ext_api_t* api = net_api(kernel);
    return api && api->get_session ? (cd_net_session_t*)api->get_session(api->userdata) : NULL;
}
static cd_net_diagnostics_t* net_get_diagnostics(struct cd_kernel_t* kernel) {
    const cd_net_ext_api_t* api = net_api(kernel);
    return api && api->get_diagnostics ? (cd_net_diagnostics_t*)api->get_diagnostics(api->userdata) : NULL;
}
static cd_net_lobby_t* net_get_lobby(struct cd_kernel_t* kernel) {
    const cd_net_ext_api_t* api = net_api(kernel);
    return api && api->get_lobby ? (cd_net_lobby_t*)api->get_lobby(api->userdata) : NULL;
}
static cd_net_nat_t* net_get_nat(struct cd_kernel_t* kernel) {
    const cd_net_ext_api_t* api = net_api(kernel);
    return api && api->get_nat ? (cd_net_nat_t*)api->get_nat(api->userdata) : NULL;
}
static cd_net_replication_t* net_get_replication(struct cd_kernel_t* kernel) {
    const cd_net_ext_api_t* api = net_api(kernel);
    return api && api->get_replication ? (cd_net_replication_t*)api->get_replication(api->userdata) : NULL;
}
static cd_net_relay_server_t* net_get_relay_server(struct cd_kernel_t* kernel) {
    const cd_net_ext_api_t* api = net_api(kernel);
    return api && api->get_relay_server ? (cd_net_relay_server_t*)api->get_relay_server(api->userdata) : NULL;
}
static void net_set_active(struct cd_kernel_t* kernel, bool active) {
    const cd_net_ext_api_t* api = net_api(kernel);
    if (api && api->set_active) api->set_active(active, api->userdata);
}

/* Vtable wrappers for networking operations — return error/0 if plugin not loaded */
static cd_result_t net_session_host(struct cd_kernel_t* k, uint16_t port, uint32_t max, const char* name) {
    const cd_net_ext_api_t* a = net_api(k); return (a && a->session_host) ? a->session_host(a->userdata, port, max, name) : CD_ERR_INVALID;
}
static cd_result_t net_session_connect(struct cd_kernel_t* k, const char* host, uint16_t port, const char* name) {
    const cd_net_ext_api_t* a = net_api(k); return (a && a->session_connect) ? a->session_connect(a->userdata, host, port, name) : CD_ERR_INVALID;
}
static void net_session_disconnect(struct cd_kernel_t* k) {
    const cd_net_ext_api_t* a = net_api(k); if (a && a->session_disconnect) a->session_disconnect(a->userdata);
}
static uint32_t net_session_connected_count(struct cd_kernel_t* k) {
    const cd_net_ext_api_t* a = net_api(k); return (a && a->session_connected_count) ? a->session_connected_count(a->userdata) : 0;
}
static cd_result_t net_replication_register(struct cd_kernel_t* k, uint32_t node, uint8_t auth, uint32_t owner, uint8_t pri) {
    const cd_net_ext_api_t* a = net_api(k); return (a && a->replication_register) ? a->replication_register(a->userdata, node, auth, owner, pri) : CD_ERR_INVALID;
}
static uint32_t net_diagnostics_bandwidth_up(struct cd_kernel_t* k) {
    const cd_net_ext_api_t* a = net_api(k); return (a && a->diagnostics_bandwidth_up) ? a->diagnostics_bandwidth_up(a->userdata) : 0;
}
static uint32_t net_diagnostics_bandwidth_down(struct cd_kernel_t* k) {
    const cd_net_ext_api_t* a = net_api(k); return (a && a->diagnostics_bandwidth_down) ? a->diagnostics_bandwidth_down(a->userdata) : 0;
}
static void net_diagnostics_set_sim(struct cd_kernel_t* k, float lat, float jit, float loss) {
    const cd_net_ext_api_t* a = net_api(k); if (a && a->diagnostics_set_sim) a->diagnostics_set_sim(a->userdata, lat, jit, loss);
}
static void net_diagnostics_clear_sim(struct cd_kernel_t* k) {
    const cd_net_ext_api_t* a = net_api(k); if (a && a->diagnostics_clear_sim) a->diagnostics_clear_sim(a->userdata);
}
static cd_result_t net_lobby_create_room(struct cd_kernel_t* k, const char* name, uint32_t max, uint32_t* out) {
    const cd_net_ext_api_t* a = net_api(k); return (a && a->lobby_create_room) ? a->lobby_create_room(a->userdata, name, max, out) : CD_ERR_INVALID;
}
static cd_result_t net_lobby_set_property(struct cd_kernel_t* k, uint32_t room, const char* key, const char* val) {
    const cd_net_ext_api_t* a = net_api(k); return (a && a->lobby_set_property) ? a->lobby_set_property(a->userdata, room, key, val) : CD_ERR_INVALID;
}
static uint32_t net_lobby_list(struct cd_kernel_t* k, void* out, uint32_t max) {
    const cd_net_ext_api_t* a = net_api(k); return (a && a->lobby_list) ? a->lobby_list(a->userdata, out, max) : 0;
}
static cd_result_t net_lobby_join(struct cd_kernel_t* k, uint32_t room, const char* name) {
    const cd_net_ext_api_t* a = net_api(k); return (a && a->lobby_join) ? a->lobby_join(a->userdata, room, name) : CD_ERR_INVALID;
}
static cd_result_t net_lobby_leave(struct cd_kernel_t* k, uint32_t room) {
    const cd_net_ext_api_t* a = net_api(k); return (a && a->lobby_leave) ? a->lobby_leave(a->userdata, room) : CD_ERR_INVALID;
}
static cd_result_t net_lobby_start_game(struct cd_kernel_t* k, uint32_t room) {
    const cd_net_ext_api_t* a = net_api(k); return (a && a->lobby_start_game) ? a->lobby_start_game(a->userdata, room) : CD_ERR_INVALID;
}
static uint32_t net_lobby_get_players(struct cd_kernel_t* k, uint32_t room, void* out, uint32_t max) {
    const cd_net_ext_api_t* a = net_api(k); return (a && a->lobby_get_players) ? a->lobby_get_players(a->userdata, room, out, max) : 0;
}
static void net_nat_detect_poll(struct cd_kernel_t* k, double dt) {
    const cd_net_ext_api_t* a = net_api(k); if (a && a->nat_detect_poll) a->nat_detect_poll(a->userdata, dt);
}
static const char* net_nat_type_to_string(struct cd_kernel_t* k) {
    const cd_net_ext_api_t* a = net_api(k); return (a && a->nat_type_to_string) ? a->nat_type_to_string(a->userdata) : "unknown";
}

/* ============================================================================
 * net.host
 * ============================================================================ */

static cJSON* handle_net_host(struct cd_kernel_t* kernel,
                               const cJSON* params,
                               int* error_code,
                               const char** error_msg) {
    cd_net_session_t* session = net_get_session(kernel);
    if (session == NULL) {
        *error_code = -32603;
        *error_msg = "Networking plugin not loaded";
        return NULL;
    }

    /* Parse params */
    int port = 7777;
    int max_clients = 8;
    const char* name = "Cadence Server";

    const cJSON* port_item = cJSON_GetObjectItemCaseSensitive(params, "port");
    if (cJSON_IsNumber(port_item)) {
        port = port_item->valueint;
    }

    const cJSON* max_item = cJSON_GetObjectItemCaseSensitive(params, "maxClients");
    if (cJSON_IsNumber(max_item)) {
        max_clients = max_item->valueint;
    }

    const cJSON* name_item = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (cJSON_IsString(name_item) && name_item->valuestring != NULL) {
        name = name_item->valuestring;
    }

    cd_result_t res = net_session_host(kernel, (uint16_t)port, (uint32_t)max_clients, name);
    if (res != CD_OK) {
        *error_code = -32603;
        *error_msg = "Failed to start hosting";
        return NULL;
    }

    net_set_active(kernel, true);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "hosting");
    cJSON_AddNumberToObject(result, "port", port);
    cJSON_AddNumberToObject(result, "maxClients", max_clients);
    cJSON_AddStringToObject(result, "name", name);
    return result;
}

/* ============================================================================
 * net.connect
 * ============================================================================ */

static cJSON* handle_net_connect(struct cd_kernel_t* kernel,
                                  const cJSON* params,
                                  int* error_code,
                                  const char** error_msg) {
    cd_net_session_t* session = net_get_session(kernel);
    if (session == NULL) {
        *error_code = -32603;
        *error_msg = "Networking plugin not loaded";
        return NULL;
    }

    const cJSON* host_item = cJSON_GetObjectItemCaseSensitive(params, "host");
    if (!cJSON_IsString(host_item) || host_item->valuestring == NULL) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: host";
        return NULL;
    }

    int port = 7777;
    const cJSON* port_item = cJSON_GetObjectItemCaseSensitive(params, "port");
    if (cJSON_IsNumber(port_item)) {
        port = port_item->valueint;
    }

    const char* client_name = "Client";
    const cJSON* name_item = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (cJSON_IsString(name_item) && name_item->valuestring != NULL) {
        client_name = name_item->valuestring;
    }

    cd_result_t res = net_session_connect(kernel, host_item->valuestring, (uint16_t)port, client_name);
    if (res != CD_OK) {
        *error_code = -32603;
        *error_msg = "Failed to connect";
        return NULL;
    }

    net_set_active(kernel, true);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "connecting");
    cJSON_AddStringToObject(result, "host", host_item->valuestring);
    cJSON_AddNumberToObject(result, "port", port);
    return result;
}

/* ============================================================================
 * net.disconnect
 * ============================================================================ */

static cJSON* handle_net_disconnect(struct cd_kernel_t* kernel,
                                     const cJSON* params,
                                     int* error_code,
                                     const char** error_msg) {
    (void)params;

    cd_net_session_t* session = net_get_session(kernel);
    if (session == NULL) {
        *error_code = -32603;
        *error_msg = "Networking plugin not loaded";
        return NULL;
    }

    net_session_disconnect(kernel);
    net_set_active(kernel, false);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "disconnected");
    return result;
}

/* ============================================================================
 * net.status
 * ============================================================================ */

static const char* session_state_str(cd_session_state_t state) {
    switch (state) {
    case CD_SESSION_IDLE:       return "idle";
    case CD_SESSION_HOSTING:    return "hosting";
    case CD_SESSION_CONNECTING: return "connecting";
    case CD_SESSION_CONNECTED:  return "connected";
    default:                    return "unknown";
    }
}

static cJSON* handle_net_status(struct cd_kernel_t* kernel,
                                 const cJSON* params,
                                 int* error_code,
                                 const char** error_msg) {
    (void)params;
    (void)error_code;
    (void)error_msg;

    cd_net_session_t* session = net_get_session(kernel);
    if (session == NULL) {
        cJSON* result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "state", "unavailable");
        cJSON_AddBoolToObject(result, "pluginLoaded", 0);
        return result;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "pluginLoaded", 1);
    cJSON_AddStringToObject(result, "state", session_state_str(session->state));
    uint32_t peer_count = net_session_connected_count(kernel);
    cJSON_AddNumberToObject(result, "peerCount", (double)peer_count);
    cJSON_AddStringToObject(result, "sessionName", session->session_name);
    cJSON_AddNumberToObject(result, "protocolVersion",
                            (double)session->protocol_version);
    cJSON_AddNumberToObject(result, "localClientId",
                            (double)session->transport.local_client_id);
    cJSON_AddBoolToObject(result, "dedicated",
                          cd_kernel_get_config(kernel)->dedicated_server ||
                          session->server_is_dedicated);

    /* Peer details array */
    cJSON* peers = cJSON_AddArrayToObject(result, "peers");
    for (uint32_t i = 0; i < session->transport.peer_count; i++) {
        const cd_net_peer_t* peer = &session->transport.peers[i];
        if (peer->state != CD_PEER_CONNECTED) continue;

        cJSON* p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "clientId", (double)peer->client_id);
        cJSON_AddStringToObject(p, "name", session->peer_data[i].name);
        cJSON_AddNumberToObject(p, "rtt", peer->rtt);
        cJSON_AddStringToObject(p, "address", peer->address);
        cJSON_AddItemToArray(peers, p);
    }

    return result;
}

/* ============================================================================
 * net.replicate
 * ============================================================================ */

static cd_authority_t parse_authority(const char* str) {
    if (!str) return CD_AUTHORITY_SERVER;
    if (strcmp(str, "local")  == 0) return CD_AUTHORITY_LOCAL;
    if (strcmp(str, "remote") == 0) return CD_AUTHORITY_REMOTE;
    if (strcmp(str, "shared") == 0) return CD_AUTHORITY_SHARED;
    return CD_AUTHORITY_SERVER;
}

static const char* authority_str(cd_authority_t auth) {
    switch (auth) {
    case CD_AUTHORITY_LOCAL:  return "local";
    case CD_AUTHORITY_REMOTE: return "remote";
    case CD_AUTHORITY_SERVER: return "server";
    case CD_AUTHORITY_SHARED: return "shared";
    default:                 return "unknown";
    }
}

static cJSON* handle_net_replicate(struct cd_kernel_t* kernel,
                                    const cJSON* params,
                                    int* error_code,
                                    const char** error_msg) {
    cd_net_replication_t* repl = net_get_replication(kernel);
    if (repl == NULL) {
        *error_code = -32603;
        *error_msg = "Networking plugin not loaded";
        return NULL;
    }

    /* Required: node id */
    const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (!cJSON_IsString(id_item) || !id_item->valuestring) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: id";
        return NULL;
    }

    /* Parse "gen:idx" format */
    uint32_t gen = 0, idx = 0;
    if (sscanf(id_item->valuestring, "%u:%u", &gen, &idx) != 2) {
        *error_code = -32602;
        *error_msg = "Invalid id format (expected gen:idx)";
        return NULL;
    }
    cd_id_t node_id = cd_id_make(gen, idx);

    /* Optional: authority (default: server) */
    cd_authority_t authority = CD_AUTHORITY_SERVER;
    const cJSON* auth_item = cJSON_GetObjectItemCaseSensitive(params, "authority");
    if (cJSON_IsString(auth_item) && auth_item->valuestring) {
        authority = parse_authority(auth_item->valuestring);
    }

    /* Optional: owner (default: 0 = server) */
    uint32_t owner_id = 0;
    const cJSON* owner_item = cJSON_GetObjectItemCaseSensitive(params, "owner");
    if (cJSON_IsNumber(owner_item)) {
        owner_id = (uint32_t)owner_item->valueint;
    }

    /* Optional: priority (default: 128) */
    uint8_t priority = 128;
    const cJSON* prio_item = cJSON_GetObjectItemCaseSensitive(params, "priority");
    if (cJSON_IsNumber(prio_item)) {
        priority = (uint8_t)prio_item->valueint;
    }

    cd_result_t res = net_replication_register(kernel, node_id, authority,
                                                owner_id, priority);
    if (res != CD_OK) {
        *error_code = -32603;
        *error_msg = "Failed to register node for replication";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "id", id_item->valuestring);
    cJSON_AddStringToObject(result, "authority", authority_str(authority));
    cJSON_AddNumberToObject(result, "owner", (double)owner_id);
    cJSON_AddNumberToObject(result, "priority", (double)priority);
    cJSON_AddStringToObject(result, "status", "registered");
    return result;
}

/* ============================================================================
 * net.diagnostics
 * ============================================================================ */

static cJSON* handle_net_diagnostics(struct cd_kernel_t* kernel,
                                      const cJSON* params,
                                      int* error_code,
                                      const char** error_msg) {
    cd_net_session_t* session = net_get_session(kernel);
    cd_net_diagnostics_t* diag = net_get_diagnostics(kernel);

    if (session == NULL || diag == NULL) {
        *error_code = -32603;
        *error_msg = "Networking plugin not loaded";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();

    /* Connected peers count */
    uint32_t peer_count = net_session_connected_count(kernel);
    cJSON_AddNumberToObject(result, "connected_peers", (double)peer_count);

    /* Aggregate stats from transport */
    cd_net_transport_t* transport = &session->transport;
    cJSON_AddNumberToObject(result, "bytes_sent",
                            (double)transport->total_bytes_sent);
    cJSON_AddNumberToObject(result, "bytes_received",
                            (double)transport->total_bytes_received);

    /* Per-peer packet totals */
    uint64_t total_pkts_sent = 0;
    uint64_t total_pkts_recv = 0;
    uint64_t total_pkts_lost = 0;
    double   best_rtt = 999999.0;
    double   worst_rtt = 0.0;
    double   rtt_sum = 0.0;
    uint32_t rtt_count = 0;

    for (uint32_t i = 0; i < CD_NET_MAX_PEERS; i++) {
        const cd_net_peer_t* peer = &transport->peers[i];
        if (peer->state == CD_PEER_DISCONNECTED) continue;

        total_pkts_sent += peer->packets_sent;
        total_pkts_recv += peer->packets_received;
        total_pkts_lost += peer->packets_lost;

        if (peer->rtt > 0.0) {
            double rtt_ms = peer->rtt * 1000.0;
            if (rtt_ms < best_rtt) best_rtt = rtt_ms;
            if (rtt_ms > worst_rtt) worst_rtt = rtt_ms;
            rtt_sum += rtt_ms;
            rtt_count++;
        }
    }

    cJSON_AddNumberToObject(result, "packets_sent", (double)total_pkts_sent);
    cJSON_AddNumberToObject(result, "packets_received", (double)total_pkts_recv);

    /* Packet loss percent */
    double loss_pct = 0.0;
    if (total_pkts_sent > 0) {
        loss_pct = (double)total_pkts_lost / (double)total_pkts_sent * 100.0;
    }
    cJSON_AddNumberToObject(result, "packet_loss_percent", loss_pct);

    /* RTT stats */
    double avg_rtt = rtt_count > 0 ? rtt_sum / (double)rtt_count : 0.0;
    if (rtt_count == 0) {
        best_rtt = 0.0;
        worst_rtt = 0.0;
    }

    /* Use diagnostics RTT stats if available (more samples) */
    if (diag->rtt_sample_count > 0) {
        avg_rtt = diag->rtt_sum_ms / (double)diag->rtt_sample_count;
        if (diag->rtt_min_ms < 999999.0f)
            best_rtt = (double)diag->rtt_min_ms;
        if (diag->rtt_max_ms > 0.0f)
            worst_rtt = (double)diag->rtt_max_ms;
    }

    cJSON_AddNumberToObject(result, "rtt_ms", avg_rtt);
    cJSON_AddNumberToObject(result, "rtt_min_ms", best_rtt);
    cJSON_AddNumberToObject(result, "rtt_max_ms", worst_rtt);
    cJSON_AddNumberToObject(result, "rtt_avg_ms", avg_rtt);

    /* Bandwidth */
    cJSON_AddNumberToObject(result, "bandwidth_up_kbps",
                            (double)net_diagnostics_bandwidth_up(kernel));
    cJSON_AddNumberToObject(result, "bandwidth_down_kbps",
                            (double)net_diagnostics_bandwidth_down(kernel));

    /* Uptime */
    double uptime = 0.0;
    if (diag->connect_start_time > 0.0 && session->current_time > 0.0) {
        uptime = session->current_time - diag->connect_start_time;
    }
    cJSON_AddNumberToObject(result, "uptime_seconds", uptime);

    /* Simulation status */
    cJSON_AddBoolToObject(result, "simulation_active",
                          diag->sim_params.enabled ? 1 : 0);

    /* Optional: RTT history */
    bool include_history = false;
    if (params) {
        const cJSON* hist_item =
            cJSON_GetObjectItemCaseSensitive(params, "include_history");
        if (cJSON_IsBool(hist_item) && cJSON_IsTrue(hist_item)) {
            include_history = true;
        }
    }

    if (include_history && diag->rtt_history_count > 0) {
        cJSON* history = cJSON_AddArrayToObject(result, "rtt_history");
        uint32_t count = diag->rtt_history_count;
        uint32_t start = 0;
        if (count >= CD_NET_RTT_HISTORY_SIZE) {
            start = diag->rtt_history_index;  /* Oldest sample */
        }
        for (uint32_t i = 0; i < count; i++) {
            uint32_t idx = (start + i) % CD_NET_RTT_HISTORY_SIZE;
            cJSON_AddItemToArray(history,
                cJSON_CreateNumber((double)diag->rtt_history[idx]));
        }
    }

    return result;
}

/* ============================================================================
 * net.simulate_conditions
 * ============================================================================ */

static cJSON* handle_net_simulate_conditions(struct cd_kernel_t* kernel,
                                              const cJSON* params,
                                              int* error_code,
                                              const char** error_msg) {
    cd_net_diagnostics_t* diag = net_get_diagnostics(kernel);
    if (diag == NULL) {
        *error_code = -32603;
        *error_msg = "Networking plugin not loaded";
        return NULL;
    }

    /* If params is NULL or empty object, disable simulation */
    if (params == NULL || cJSON_GetArraySize(params) == 0) {
        net_diagnostics_clear_sim(kernel);

        cJSON* result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", "disabled");
        return result;
    }

    /* Check for explicit disable */
    const cJSON* enabled_item =
        cJSON_GetObjectItemCaseSensitive(params, "enabled");
    if (cJSON_IsBool(enabled_item) && !cJSON_IsTrue(enabled_item)) {
        net_diagnostics_clear_sim(kernel);

        cJSON* result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", "disabled");
        return result;
    }

    cd_net_sim_params_t sim;
    memset(&sim, 0, sizeof(sim));

    const cJSON* item;

    item = cJSON_GetObjectItemCaseSensitive(params, "latency_ms");
    if (cJSON_IsNumber(item)) {
        sim.latency_ms = item->valueint;
    }

    item = cJSON_GetObjectItemCaseSensitive(params, "jitter_ms");
    if (cJSON_IsNumber(item)) {
        sim.jitter_ms = item->valueint;
    }

    item = cJSON_GetObjectItemCaseSensitive(params, "packet_loss_percent");
    if (cJSON_IsNumber(item)) {
        sim.packet_loss_percent = (float)item->valuedouble;
    }

    item = cJSON_GetObjectItemCaseSensitive(params, "bandwidth_limit_kbps");
    if (cJSON_IsNumber(item)) {
        sim.bandwidth_limit_kbps = item->valueint;
    }

    item = cJSON_GetObjectItemCaseSensitive(params, "duplicate_percent");
    if (cJSON_IsNumber(item)) {
        sim.duplicate_percent = (float)item->valuedouble;
    }

    /* Validate ranges */
    if (sim.packet_loss_percent < 0.0f || sim.packet_loss_percent > 100.0f) {
        *error_code = -32602;
        *error_msg = "packet_loss_percent must be 0-100";
        return NULL;
    }
    if (sim.duplicate_percent < 0.0f || sim.duplicate_percent > 100.0f) {
        *error_code = -32602;
        *error_msg = "duplicate_percent must be 0-100";
        return NULL;
    }
    if (sim.latency_ms < 0) {
        *error_code = -32602;
        *error_msg = "latency_ms must be >= 0";
        return NULL;
    }
    if (sim.jitter_ms < 0) {
        *error_code = -32602;
        *error_msg = "jitter_ms must be >= 0";
        return NULL;
    }

    net_diagnostics_set_sim(kernel, sim.latency_ms, sim.jitter_ms, sim.packet_loss_percent);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "enabled");
    cJSON_AddNumberToObject(result, "latency_ms", sim.latency_ms);
    cJSON_AddNumberToObject(result, "jitter_ms", sim.jitter_ms);
    cJSON_AddNumberToObject(result, "packet_loss_percent",
                            (double)sim.packet_loss_percent);
    cJSON_AddNumberToObject(result, "bandwidth_limit_kbps",
                            sim.bandwidth_limit_kbps);
    cJSON_AddNumberToObject(result, "duplicate_percent",
                            (double)sim.duplicate_percent);
    return result;
}

/* ============================================================================
 * net.lobby_create
 * ============================================================================ */

static cJSON* handle_net_lobby_create(struct cd_kernel_t* kernel,
                                       const cJSON* params,
                                       int* error_code,
                                       const char** error_msg) {
    cd_net_lobby_t* lobby = net_get_lobby(kernel);
    if (!lobby) {
        *error_code = -32603;
        *error_msg = "Networking plugin not loaded";
        return NULL;
    }

    const cJSON* name_item = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (!cJSON_IsString(name_item) || !name_item->valuestring) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: name";
        return NULL;
    }

    int max_players = 8;
    const cJSON* max_item = cJSON_GetObjectItemCaseSensitive(params, "maxPlayers");
    if (cJSON_IsNumber(max_item)) {
        max_players = max_item->valueint;
    }

    uint32_t host_peer_id = 1;
    const cJSON* host_item = cJSON_GetObjectItemCaseSensitive(params, "hostPeerId");
    if (cJSON_IsNumber(host_item)) {
        host_peer_id = (uint32_t)host_item->valueint;
    }

    const char* host_name = "Host";
    const cJSON* hname_item = cJSON_GetObjectItemCaseSensitive(params, "hostName");
    if (cJSON_IsString(hname_item) && hname_item->valuestring) {
        host_name = hname_item->valuestring;
    }

    uint32_t room_id = 0;
    cd_result_t res = net_lobby_create_room(kernel,
                                                name_item->valuestring,
                                                (uint32_t)max_players,
                                                host_peer_id, host_name,
                                                &room_id);
    if (res != CD_OK) {
        *error_code = -32603;
        *error_msg = (res == CD_ERR_FULL) ? "No room slots available"
                                          : "Failed to create room";
        return NULL;
    }

    /* Set custom properties if provided */
    const cJSON* props = cJSON_GetObjectItemCaseSensitive(params, "properties");
    if (cJSON_IsObject(props)) {
        const cJSON* prop = NULL;
        cJSON_ArrayForEach(prop, props) {
            if (cJSON_IsString(prop) && prop->string) {
                net_lobby_set_property(kernel, room_id,
                                           prop->string, prop->valuestring);
            }
        }
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "roomId", (double)room_id);
    cJSON_AddStringToObject(result, "name", name_item->valuestring);
    cJSON_AddNumberToObject(result, "maxPlayers", (double)max_players);
    cJSON_AddStringToObject(result, "status", "created");
    return result;
}

/* ============================================================================
 * net.lobby_list
 * ============================================================================ */

static cJSON* handle_net_lobby_list(struct cd_kernel_t* kernel,
                                     const cJSON* params,
                                     int* error_code,
                                     const char** error_msg) {
    (void)error_code;
    (void)error_msg;

    cd_net_lobby_t* lobby = net_get_lobby(kernel);
    if (!lobby) {
        *error_code = -32603;
        *error_msg = "Networking plugin not loaded";
        return NULL;
    }

    bool include_all = false;
    if (params) {
        const cJSON* all_item = cJSON_GetObjectItemCaseSensitive(params, "includeAll");
        if (cJSON_IsBool(all_item) && cJSON_IsTrue(all_item)) {
            include_all = true;
        }
    }

    cd_net_lobby_room_info_t rooms[CD_NET_LOBBY_MAX_ROOMS];
    uint32_t count = net_lobby_list(kernel, rooms,
                                        CD_NET_LOBBY_MAX_ROOMS, include_all);

    cJSON* result = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(result, "rooms");
    cJSON_AddNumberToObject(result, "count", (double)count);

    for (uint32_t i = 0; i < count; i++) {
        cJSON* r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "roomId", (double)rooms[i].room_id);
        cJSON_AddStringToObject(r, "name", rooms[i].name);
        cJSON_AddNumberToObject(r, "playerCount", (double)rooms[i].player_count);
        cJSON_AddNumberToObject(r, "maxPlayers", (double)rooms[i].max_players);
        cJSON_AddStringToObject(r, "hostName", rooms[i].host_name);

        const char* state_str = "unknown";
        switch (rooms[i].state) {
            case CD_LOBBY_ROOM_WAITING: state_str = "waiting"; break;
            case CD_LOBBY_ROOM_IN_GAME: state_str = "in_game"; break;
            case CD_LOBBY_ROOM_CLOSED:  state_str = "closed"; break;
            default: break;
        }
        cJSON_AddStringToObject(r, "state", state_str);
        cJSON_AddItemToArray(arr, r);
    }

    return result;
}

/* ============================================================================
 * net.lobby_join
 * ============================================================================ */

static cJSON* handle_net_lobby_join(struct cd_kernel_t* kernel,
                                     const cJSON* params,
                                     int* error_code,
                                     const char** error_msg) {
    cd_net_lobby_t* lobby = net_get_lobby(kernel);
    if (!lobby) {
        *error_code = -32603;
        *error_msg = "Networking plugin not loaded";
        return NULL;
    }

    const cJSON* room_item = cJSON_GetObjectItemCaseSensitive(params, "roomId");
    if (!cJSON_IsNumber(room_item)) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: roomId";
        return NULL;
    }

    const cJSON* name_item = cJSON_GetObjectItemCaseSensitive(params, "playerName");
    if (!cJSON_IsString(name_item) || !name_item->valuestring) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: playerName";
        return NULL;
    }

    uint32_t peer_id = 0;
    const cJSON* peer_item = cJSON_GetObjectItemCaseSensitive(params, "peerId");
    if (cJSON_IsNumber(peer_item)) {
        peer_id = (uint32_t)peer_item->valueint;
    }

    cd_result_t res = net_lobby_join(kernel,
                                         (uint32_t)room_item->valueint,
                                         peer_id,
                                         name_item->valuestring);
    if (res != CD_OK) {
        *error_code = -32603;
        if (res == CD_ERR_NOTFOUND)     *error_msg = "Room not found";
        else if (res == CD_ERR_FULL)    *error_msg = "Room is full";
        else if (res == CD_ERR_INVALID) *error_msg = "Cannot join (wrong state or already in room)";
        else                             *error_msg = "Failed to join room";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "roomId", room_item->valuedouble);
    cJSON_AddStringToObject(result, "playerName", name_item->valuestring);
    cJSON_AddStringToObject(result, "status", "joined");
    return result;
}

/* ============================================================================
 * net.lobby_leave
 * ============================================================================ */

static cJSON* handle_net_lobby_leave(struct cd_kernel_t* kernel,
                                      const cJSON* params,
                                      int* error_code,
                                      const char** error_msg) {
    cd_net_lobby_t* lobby = net_get_lobby(kernel);
    if (!lobby) {
        *error_code = -32603;
        *error_msg = "Networking plugin not loaded";
        return NULL;
    }

    const cJSON* room_item = cJSON_GetObjectItemCaseSensitive(params, "roomId");
    if (!cJSON_IsNumber(room_item)) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: roomId";
        return NULL;
    }

    const cJSON* peer_item = cJSON_GetObjectItemCaseSensitive(params, "peerId");
    if (!cJSON_IsNumber(peer_item)) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: peerId";
        return NULL;
    }

    cd_result_t res = net_lobby_leave(kernel,
                                          (uint32_t)room_item->valueint,
                                          (uint32_t)peer_item->valueint);
    if (res != CD_OK) {
        *error_code = -32603;
        *error_msg = (res == CD_ERR_NOTFOUND) ? "Room or player not found"
                                               : "Failed to leave room";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "left");
    return result;
}

/* ============================================================================
 * net.lobby_start
 * ============================================================================ */

static cJSON* handle_net_lobby_start(struct cd_kernel_t* kernel,
                                      const cJSON* params,
                                      int* error_code,
                                      const char** error_msg) {
    cd_net_lobby_t* lobby = net_get_lobby(kernel);
    if (!lobby) {
        *error_code = -32603;
        *error_msg = "Networking plugin not loaded";
        return NULL;
    }

    const cJSON* room_item = cJSON_GetObjectItemCaseSensitive(params, "roomId");
    if (!cJSON_IsNumber(room_item)) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: roomId";
        return NULL;
    }

    const cJSON* peer_item = cJSON_GetObjectItemCaseSensitive(params, "peerId");
    if (!cJSON_IsNumber(peer_item)) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: peerId";
        return NULL;
    }

    cd_result_t res = net_lobby_start_game(kernel,
                                               (uint32_t)room_item->valueint,
                                               (uint32_t)peer_item->valueint);
    if (res != CD_OK) {
        *error_code = -32603;
        if (res == CD_ERR_NOTFOUND) *error_msg = "Room not found";
        else if (res == CD_ERR_INVALID) *error_msg = "Not host or wrong room state";
        else *error_msg = "Failed to start game";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "roomId", room_item->valuedouble);
    cJSON_AddStringToObject(result, "status", "in_game");
    return result;
}

/* ============================================================================
 * net.lobby_players
 * ============================================================================ */

static cJSON* handle_net_lobby_players(struct cd_kernel_t* kernel,
                                        const cJSON* params,
                                        int* error_code,
                                        const char** error_msg) {
    cd_net_lobby_t* lobby = net_get_lobby(kernel);
    if (!lobby) {
        *error_code = -32603;
        *error_msg = "Networking plugin not loaded";
        return NULL;
    }

    const cJSON* room_item = cJSON_GetObjectItemCaseSensitive(params, "roomId");
    if (!cJSON_IsNumber(room_item)) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: roomId";
        return NULL;
    }

    cd_net_lobby_player_t players[CD_NET_LOBBY_MAX_PLAYERS];
    uint32_t count = net_lobby_get_players(kernel,
                                               (uint32_t)room_item->valueint,
                                               players,
                                               CD_NET_LOBBY_MAX_PLAYERS);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "roomId", room_item->valuedouble);
    cJSON_AddNumberToObject(result, "count", (double)count);

    cJSON* arr = cJSON_AddArrayToObject(result, "players");
    for (uint32_t i = 0; i < count; i++) {
        cJSON* p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "peerId", (double)players[i].peer_id);
        cJSON_AddStringToObject(p, "name", players[i].name);
        cJSON_AddBoolToObject(p, "isHost", players[i].is_host ? 1 : 0);
        cJSON_AddItemToArray(arr, p);
    }

    return result;
}

/* ============================================================================
 * net.nat_detect
 * ============================================================================ */

static cJSON* handle_net_nat_detect(struct cd_kernel_t* kernel,
                                     const cJSON* params,
                                     int* error_code,
                                     const char** error_msg) {
    (void)params;

    cd_net_nat_t* nat = net_get_nat(kernel);
    if (!nat) {
        *error_code = -32603;
        *error_msg = "Networking plugin not loaded";
        return NULL;
    }

    /* Trigger detection if not already done */
    if (!nat->detection.detected) {
        net_nat_detect_poll(kernel, 0.0);
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "nat_type",
                            net_nat_type_to_string(kernel));
    cJSON_AddBoolToObject(result, "detected",
                          nat->detection.detected ? 1 : 0);
    cJSON_AddStringToObject(result, "public_address",
                            nat->detection.public_address);
    cJSON_AddNumberToObject(result, "public_port",
                            (double)nat->detection.public_port);

    /* Hole punch status */
    const char* punch_str = "idle";
    switch (nat->punch.state) {
    case CD_PUNCH_IN_PROGRESS: punch_str = "in_progress"; break;
    case CD_PUNCH_SUCCESS:     punch_str = "success"; break;
    case CD_PUNCH_FAILED:      punch_str = "failed"; break;
    default: break;
    }
    cJSON_AddStringToObject(result, "hole_punch_state", punch_str);
    cJSON_AddNumberToObject(result, "hole_punch_retries",
                            (double)nat->punch.retry_count);

    return result;
}

/* ============================================================================
 * net.relay_status
 * ============================================================================ */

static cJSON* handle_net_relay_status(struct cd_kernel_t* kernel,
                                       const cJSON* params,
                                       int* error_code,
                                       const char** error_msg) {
    (void)params;

    cd_net_nat_t* nat = net_get_nat(kernel);
    if (!nat) {
        *error_code = -32603;
        *error_msg = "Networking plugin not loaded";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();

    /* Client relay connection status */
    const char* state_str = "disconnected";
    switch (nat->relay.state) {
    case CD_RELAY_CONNECTING: state_str = "connecting"; break;
    case CD_RELAY_CONNECTED:  state_str = "connected"; break;
    default: break;
    }
    cJSON_AddStringToObject(result, "client_state", state_str);
    cJSON_AddStringToObject(result, "relay_address",
                            nat->relay.relay_address);
    cJSON_AddNumberToObject(result, "relay_port",
                            (double)nat->relay.relay_port);
    cJSON_AddNumberToObject(result, "bytes_relayed",
                            (double)nat->relay.bytes_relayed);
    cJSON_AddNumberToObject(result, "packets_relayed",
                            (double)nat->relay.packets_relayed);
    cJSON_AddNumberToObject(result, "relay_peer_count",
                            (double)nat->relay.peer_count);

    /* Relay server status (if running locally) */
    cd_net_relay_server_t* server =
        net_get_relay_server(kernel);
    if (server && server->running) {
        cJSON* srv = cJSON_AddObjectToObject(result, "server");
        cJSON_AddBoolToObject(srv, "running", 1);
        cJSON_AddNumberToObject(srv, "port",
                                (double)server->port);
        cJSON_AddNumberToObject(srv, "client_count",
                                (double)server->client_count);
        cJSON_AddNumberToObject(srv, "total_bytes_forwarded",
                                (double)server->total_bytes_forwarded);
        cJSON_AddNumberToObject(srv, "total_packets_forwarded",
                                (double)server->total_packets_forwarded);
        cJSON_AddNumberToObject(srv, "total_registrations",
                                (double)server->total_registrations);
    } else {
        cJSON* srv = cJSON_AddObjectToObject(result, "server");
        cJSON_AddBoolToObject(srv, "running", 0);
    }

    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_net_tools(cd_mcp_server_t* server) {
    if (server == NULL) return CD_ERR_NULL;

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "net.host", handle_net_host,
        "Start hosting a network session",
        "{\"type\":\"object\",\"properties\":{"
        "\"port\":{\"type\":\"number\",\"description\":\"Listen port (default 7777)\"},"
        "\"maxClients\":{\"type\":\"number\",\"description\":\"Max connected clients (default 8)\"},"
        "\"name\":{\"type\":\"string\",\"description\":\"Session name\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "net.connect", handle_net_connect,
        "Connect to a hosted network session",
        "{\"type\":\"object\",\"properties\":{"
        "\"host\":{\"type\":\"string\",\"description\":\"Host address to connect to\"},"
        "\"port\":{\"type\":\"number\",\"description\":\"Port (default 7777)\"},"
        "\"name\":{\"type\":\"string\",\"description\":\"Client display name\"}"
        "},\"required\":[\"host\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "net.disconnect", handle_net_disconnect,
        "Disconnect from the current network session",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "net.status", handle_net_status,
        "Query connection state, latency, and peer list",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "net.replicate", handle_net_replicate,
        "Register a scene node for network replication",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\",\"description\":\"Node ID in gen:idx format\"},"
        "\"authority\":{\"type\":\"string\",\"enum\":[\"server\",\"local\",\"remote\",\"shared\"],\"description\":\"Authority model\"},"
        "\"owner\":{\"type\":\"number\",\"description\":\"Owner client ID (default 0 = server)\"},"
        "\"priority\":{\"type\":\"number\",\"description\":\"Replication priority 0-255 (default 128)\"}"
        "},\"required\":[\"id\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "net.diagnostics", handle_net_diagnostics,
        "Get detailed network statistics and RTT history",
        "{\"type\":\"object\",\"properties\":{"
        "\"include_history\":{\"type\":\"boolean\",\"description\":\"Include RTT sample history\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "net.simulate_conditions",
        handle_net_simulate_conditions,
        "Inject artificial latency, loss, and jitter for testing",
        "{\"type\":\"object\",\"properties\":{"
        "\"enabled\":{\"type\":\"boolean\",\"description\":\"Enable or disable simulation\"},"
        "\"latency_ms\":{\"type\":\"number\",\"description\":\"Added latency in ms\"},"
        "\"jitter_ms\":{\"type\":\"number\",\"description\":\"Jitter variance in ms\"},"
        "\"packet_loss_percent\":{\"type\":\"number\",\"description\":\"Packet loss 0-100\"},"
        "\"bandwidth_limit_kbps\":{\"type\":\"number\",\"description\":\"Bandwidth cap in kbps\"},"
        "\"duplicate_percent\":{\"type\":\"number\",\"description\":\"Duplicate packet rate 0-100\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "net.lobby_create",
        handle_net_lobby_create,
        "Create a new lobby room",
        "{\"type\":\"object\",\"properties\":{"
        "\"name\":{\"type\":\"string\",\"description\":\"Room name\"},"
        "\"maxPlayers\":{\"type\":\"number\",\"description\":\"Max players (default 8)\"},"
        "\"hostPeerId\":{\"type\":\"number\",\"description\":\"Host peer ID\"},"
        "\"hostName\":{\"type\":\"string\",\"description\":\"Host display name\"},"
        "\"properties\":{\"type\":\"object\",\"description\":\"Custom room properties (string values)\"}"
        "},\"required\":[\"name\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "net.lobby_list",
        handle_net_lobby_list,
        "List available lobby rooms",
        "{\"type\":\"object\",\"properties\":{"
        "\"includeAll\":{\"type\":\"boolean\",\"description\":\"Include closed/in-game rooms\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "net.lobby_join",
        handle_net_lobby_join,
        "Join an existing lobby room",
        "{\"type\":\"object\",\"properties\":{"
        "\"roomId\":{\"type\":\"number\",\"description\":\"Room ID to join\"},"
        "\"playerName\":{\"type\":\"string\",\"description\":\"Player display name\"},"
        "\"peerId\":{\"type\":\"number\",\"description\":\"Peer ID\"}"
        "},\"required\":[\"roomId\",\"playerName\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "net.lobby_leave",
        handle_net_lobby_leave,
        "Leave a lobby room",
        "{\"type\":\"object\",\"properties\":{"
        "\"roomId\":{\"type\":\"number\",\"description\":\"Room ID to leave\"},"
        "\"peerId\":{\"type\":\"number\",\"description\":\"Peer ID of the leaving player\"}"
        "},\"required\":[\"roomId\",\"peerId\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "net.lobby_start",
        handle_net_lobby_start,
        "Start the game in a lobby room",
        "{\"type\":\"object\",\"properties\":{"
        "\"roomId\":{\"type\":\"number\",\"description\":\"Room ID to start\"},"
        "\"peerId\":{\"type\":\"number\",\"description\":\"Peer ID of the host requesting start\"}"
        "},\"required\":[\"roomId\",\"peerId\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "net.lobby_players",
        handle_net_lobby_players,
        "List players in a lobby room",
        "{\"type\":\"object\",\"properties\":{"
        "\"roomId\":{\"type\":\"number\",\"description\":\"Room ID to query\"}"
        "},\"required\":[\"roomId\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "net.nat_detect",
        handle_net_nat_detect,
        "Detect NAT type and public address",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "net.relay_status",
        handle_net_relay_status,
        "Get relay connection and server status",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    return CD_OK;
}
