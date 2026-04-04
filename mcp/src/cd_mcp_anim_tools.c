/* cd_mcp_anim_tools.c - Cadence Engine MCP skeletal animation tools
 *
 * Implements:
 *   - anim.play       : Play a skeletal animation clip on a node
 *   - anim.blend_to   : Crossfade to a new clip
 *   - anim.stop       : Stop animation playback
 *   - anim.query      : Get current animation state
 *   - anim.add_state  : Add a state to the animation state machine
 *   - anim.add_transition : Add a state machine transition
 *
 * Uses a file-scoped array of per-node animation state (player + skeleton)
 * for MCP-only workflows (headless testing).
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_error.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_skeleton.h"
#include "cadence/cd_anim_clip.h"
#include "cadence/cd_anim_player.h"
#include "cadence/cd_anim_state_machine.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Internal per-node storage
 * ============================================================================ */

#define CD_MCP_ANIM_MAX 64

typedef struct {
    cd_id_t                  node_id;
    cd_skeleton_t            skeleton;
    cd_skel_player_t         player;
    cd_anim_state_machine_t  state_machine;
    bool                     has_sm;
    bool                     active;
} cd_mcp_anim_entry_t;

static cd_mcp_anim_entry_t s_entries[CD_MCP_ANIM_MAX];
static uint32_t            s_entry_count = 0;

static cd_mcp_anim_entry_t* mcp_anim_find(cd_id_t node_id) {
    for (uint32_t i = 0; i < s_entry_count; i++) {
        if (s_entries[i].active && s_entries[i].node_id == node_id)
            return &s_entries[i];
    }
    return NULL;
}

static cd_mcp_anim_entry_t* mcp_anim_find_or_create(cd_id_t node_id) {
    cd_mcp_anim_entry_t* e = mcp_anim_find(node_id);
    if (e) return e;

    for (uint32_t i = 0; i < CD_MCP_ANIM_MAX; i++) {
        if (!s_entries[i].active) {
            memset(&s_entries[i], 0, sizeof(s_entries[i]));
            s_entries[i].node_id = node_id;
            s_entries[i].active = true;
            cd_skeleton_init(&s_entries[i].skeleton);
            cd_skel_player_init(&s_entries[i].player, &s_entries[i].skeleton);
            if (i >= s_entry_count) s_entry_count = i + 1;
            return &s_entries[i];
        }
    }
    return NULL;
}

static cd_id_t parse_node_id(const cJSON* params) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(params, "node_id");
    if (!item || !cJSON_IsNumber(item)) return CD_ID_INVALID;
    return (cd_id_t)(uint64_t)item->valuedouble;
}

/* ============================================================================
 * anim.play
 * ============================================================================ */

static cJSON* handle_anim_play(struct cd_kernel_t* kernel, const cJSON* params,
                                int* error_code, const char** error_msg) {
    (void)kernel;
    cd_id_t nid = parse_node_id(params);
    if (!cd_id_is_valid(nid)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "Missing or invalid node_id",
            "anim.play requires a valid 'node_id' (uint64) identifying the scene node.",
            "Use node.find or scene.query to get valid node IDs.");
        return NULL;
    }

    const cJSON* clip_item = cJSON_GetObjectItemCaseSensitive(params, "clip");
    if (!clip_item || !cJSON_IsString(clip_item)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "Missing required parameter: clip",
            "anim.play requires a 'clip' string naming the animation clip to play.",
            "Use anim.query with the node_id to list available clips.");
        return NULL;
    }

    cd_mcp_anim_entry_t* e = mcp_anim_find(nid);
    if (!e) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "Node %llu has no animation setup. anim.add_state or a prior anim call is required.",
                 (unsigned long long)nid);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "No animation setup for node",
            detail,
            "Call anim.add_state first to set up animation on this node.");
        return NULL;
    }

    cd_result_t r = cd_skel_player_play(&e->player, clip_item->valuestring);
    if (r != CD_OK) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "Clip '%s' not found on node %llu. The node has %u registered clips.",
                 clip_item->valuestring, (unsigned long long)nid, e->player.clip_count);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "Animation clip not found",
            detail,
            "Use anim.query to list available clips for this node.");
        return NULL;
    }

    return cJSON_CreateString("ok");
}

/* ============================================================================
 * anim.blend_to
 * ============================================================================ */

static cJSON* handle_anim_blend_to(struct cd_kernel_t* kernel, const cJSON* params,
                                    int* error_code, const char** error_msg) {
    (void)kernel;
    cd_id_t nid = parse_node_id(params);
    if (!cd_id_is_valid(nid)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "Missing or invalid node_id",
            "anim.blend_to requires a valid 'node_id' (uint64) identifying the scene node.",
            "Use node.find or scene.query to get valid node IDs.");
        return NULL;
    }

    const cJSON* clip_item = cJSON_GetObjectItemCaseSensitive(params, "clip");
    if (!clip_item || !cJSON_IsString(clip_item)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "Missing required parameter: clip",
            "anim.blend_to requires a 'clip' string naming the target animation clip.",
            "Use anim.query with the node_id to list available clips.");
        return NULL;
    }

    float duration = 0.3f;
    const cJSON* dur_item = cJSON_GetObjectItemCaseSensitive(params, "duration");
    if (dur_item && cJSON_IsNumber(dur_item))
        duration = (float)dur_item->valuedouble;

    cd_mcp_anim_entry_t* e = mcp_anim_find(nid);
    if (!e) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "Node %llu has no animation setup. anim.add_state or a prior anim call is required.",
                 (unsigned long long)nid);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "No animation setup for node",
            detail,
            "Call anim.add_state first to set up animation on this node.");
        return NULL;
    }

    cd_result_t r = cd_skel_player_blend_to(&e->player, clip_item->valuestring,
                                              duration);
    if (r != CD_OK) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "Clip '%s' not found on node %llu. The node has %u registered clips.",
                 clip_item->valuestring, (unsigned long long)nid, e->player.clip_count);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "Animation clip not found",
            detail,
            "Use anim.query to list available clips for this node.");
        return NULL;
    }

    return cJSON_CreateString("ok");
}

/* ============================================================================
 * anim.stop
 * ============================================================================ */

static cJSON* handle_anim_stop(struct cd_kernel_t* kernel, const cJSON* params,
                                int* error_code, const char** error_msg) {
    (void)kernel;
    cd_id_t nid = parse_node_id(params);
    if (!cd_id_is_valid(nid)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "Missing or invalid node_id",
            "anim.stop requires a valid 'node_id' (uint64) identifying the scene node.",
            "Use node.find or scene.query to get valid node IDs.");
        return NULL;
    }

    cd_mcp_anim_entry_t* e = mcp_anim_find(nid);
    if (e) cd_skel_player_stop(&e->player);

    return cJSON_CreateString("ok");
}

/* ============================================================================
 * anim.query
 * ============================================================================ */

static cJSON* handle_anim_query(struct cd_kernel_t* kernel, const cJSON* params,
                                 int* error_code, const char** error_msg) {
    (void)kernel;
    cd_id_t nid = parse_node_id(params);
    if (!cd_id_is_valid(nid)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "Missing or invalid node_id",
            "anim.query requires a valid 'node_id' (uint64) identifying the scene node.",
            "Use node.find or scene.query to get valid node IDs.");
        return NULL;
    }

    cd_mcp_anim_entry_t* e = mcp_anim_find(nid);
    if (!e) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "Node %llu has no animation setup. No clips or state machine configured.",
                 (unsigned long long)nid);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "No animation setup for node",
            detail,
            "Call anim.add_state first to set up animation on this node.");
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "playing", e->player.playing);
    cJSON_AddNumberToObject(result, "speed", (double)e->player.speed);
    cJSON_AddNumberToObject(result, "time", (double)e->player.current_time);
    cJSON_AddNumberToObject(result, "bone_count",
                             (double)e->skeleton.bone_count);
    cJSON_AddNumberToObject(result, "clip_count",
                             (double)e->player.clip_count);

    if (e->player.current_clip >= 0 &&
        (uint32_t)e->player.current_clip < e->player.clip_count) {
        cJSON_AddStringToObject(result, "current_clip",
                                 e->player.clips[e->player.current_clip].name);
    } else {
        cJSON_AddNullToObject(result, "current_clip");
    }

    cJSON* clips = cJSON_CreateArray();
    for (uint32_t i = 0; i < e->player.clip_count; i++) {
        cJSON_AddItemToArray(clips,
                              cJSON_CreateString(e->player.clips[i].name));
    }
    cJSON_AddItemToObject(result, "clips", clips);

    return result;
}

/* ============================================================================
 * anim.add_state
 * ============================================================================ */

static cJSON* handle_anim_add_state(struct cd_kernel_t* kernel,
                                     const cJSON* params,
                                     int* error_code,
                                     const char** error_msg) {
    (void)kernel;
    cd_id_t nid = parse_node_id(params);
    if (!cd_id_is_valid(nid)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "Missing or invalid node_id",
            "anim.add_state requires a valid 'node_id' (uint64) identifying the scene node.",
            "Use node.find or scene.query to get valid node IDs.");
        return NULL;
    }

    const cJSON* name_item = cJSON_GetObjectItemCaseSensitive(params, "name");
    const cJSON* clip_item = cJSON_GetObjectItemCaseSensitive(params, "clip");
    if (!name_item || !clip_item ||
        !cJSON_IsString(name_item) || !cJSON_IsString(clip_item)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "Missing required parameters: name, clip",
            "anim.add_state requires both 'name' (state name) and 'clip' (animation clip name) as strings.",
            "Example: {\"node_id\": 1, \"name\": \"idle\", \"clip\": \"idle_anim\", \"loop\": true}");
        return NULL;
    }

    bool looping = true;
    const cJSON* loop_item = cJSON_GetObjectItemCaseSensitive(params, "loop");
    if (loop_item && cJSON_IsBool(loop_item))
        looping = cJSON_IsTrue(loop_item) != 0;

    cd_mcp_anim_entry_t* e = mcp_anim_find_or_create(nid);
    if (!e) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "All %d animation node slots are in use. Cannot add animation for node %llu.",
                 CD_MCP_ANIM_MAX, (unsigned long long)nid);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = cd_mcp_error_fmt(
            "Max animation nodes reached",
            detail,
            "Stop animations on unused nodes with anim.stop to free slots.");
        return NULL;
    }

    if (!e->has_sm) {
        cd_anim_sm_init(&e->state_machine, &e->player);
        e->has_sm = true;
    }

    cd_result_t r = cd_anim_sm_add_state(&e->state_machine,
                                           name_item->valuestring,
                                           clip_item->valuestring,
                                           looping);
    if (r != CD_OK) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "Failed to add state '%s' with clip '%s' to node %llu. The state machine may be full or the state name duplicated.",
                 name_item->valuestring, clip_item->valuestring, (unsigned long long)nid);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = cd_mcp_error_fmt(
            "State add failed",
            detail,
            "Check that the state name is unique. Use anim.query to inspect current states.");
        return NULL;
    }

    return cJSON_CreateString("ok");
}

/* ============================================================================
 * anim.add_transition
 * ============================================================================ */

static cJSON* handle_anim_add_transition(struct cd_kernel_t* kernel,
                                          const cJSON* params,
                                          int* error_code,
                                          const char** error_msg) {
    (void)kernel;
    cd_id_t nid = parse_node_id(params);
    if (!cd_id_is_valid(nid)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "Missing or invalid node_id",
            "anim.add_transition requires a valid 'node_id' (uint64) identifying the scene node.",
            "Use node.find or scene.query to get valid node IDs.");
        return NULL;
    }

    const cJSON* from_item = cJSON_GetObjectItemCaseSensitive(params, "from");
    const cJSON* to_item = cJSON_GetObjectItemCaseSensitive(params, "to");
    const cJSON* cond_item = cJSON_GetObjectItemCaseSensitive(params, "condition");
    if (!from_item || !to_item || !cond_item ||
        !cJSON_IsString(from_item) || !cJSON_IsString(to_item) ||
        !cJSON_IsString(cond_item)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "Missing required parameters",
            "anim.add_transition requires 'from' (string), 'to' (string), and 'condition' (string).",
            "Example: {\"node_id\": 1, \"from\": \"idle\", \"to\": \"walk\", \"condition\": \"is_moving\"}");
        return NULL;
    }

    float blend = 0.2f;
    const cJSON* blend_item = cJSON_GetObjectItemCaseSensitive(params, "blend");
    if (blend_item && cJSON_IsNumber(blend_item))
        blend = (float)blend_item->valuedouble;

    cd_mcp_anim_entry_t* e = mcp_anim_find(nid);
    if (!e || !e->has_sm) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "Node %llu has no state machine. States must be added before transitions.",
                 (unsigned long long)nid);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt(
            "No state machine for node",
            detail,
            "Call anim.add_state first to create states, then add transitions between them.");
        return NULL;
    }

    cd_result_t r = cd_anim_sm_add_transition(&e->state_machine,
                                                from_item->valuestring,
                                                to_item->valuestring,
                                                cond_item->valuestring,
                                                blend);
    if (r != CD_OK) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "Failed to add transition '%s' -> '%s' (condition='%s') on node %llu. A source or target state may not exist.",
                 from_item->valuestring, to_item->valuestring,
                 cond_item->valuestring, (unsigned long long)nid);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = cd_mcp_error_fmt(
            "Transition add failed",
            detail,
            "Ensure both 'from' and 'to' states exist. Use anim.query to list current states.");
        return NULL;
    }

    return cJSON_CreateString("ok");
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_anim_tools(cd_mcp_server_t* server) {
    if (!server) return CD_ERR_NULL;

    cd_mcp_register_tool_ex(server, "anim.play", handle_anim_play,
        "Play a skeletal animation clip on a node",
        "{\"type\":\"object\",\"properties\":{"
        "\"node_id\":{\"type\":\"number\",\"description\":\"Scene node ID\"},"
        "\"clip\":{\"type\":\"string\",\"description\":\"Animation clip name to play\"}"
        "},\"required\":[\"node_id\",\"clip\"]}");
    cd_mcp_register_tool_ex(server, "anim.blend_to", handle_anim_blend_to,
        "Crossfade from the current animation to a new clip",
        "{\"type\":\"object\",\"properties\":{"
        "\"node_id\":{\"type\":\"number\",\"description\":\"Scene node ID\"},"
        "\"clip\":{\"type\":\"string\",\"description\":\"Target animation clip name\"},"
        "\"duration\":{\"type\":\"number\",\"description\":\"Blend duration in seconds (default 0.3)\"}"
        "},\"required\":[\"node_id\",\"clip\"]}");
    cd_mcp_register_tool_ex(server, "anim.stop", handle_anim_stop,
        "Stop animation playback on a node",
        "{\"type\":\"object\",\"properties\":{"
        "\"node_id\":{\"type\":\"number\",\"description\":\"Scene node ID\"}"
        "},\"required\":[\"node_id\"]}");
    cd_mcp_register_tool_ex(server, "anim.query", handle_anim_query,
        "Get current animation state and clip list for a node",
        "{\"type\":\"object\",\"properties\":{"
        "\"node_id\":{\"type\":\"number\",\"description\":\"Scene node ID\"}"
        "},\"required\":[\"node_id\"]}");
    cd_mcp_register_tool_ex(server, "anim.add_state", handle_anim_add_state,
        "Add a state to the animation state machine on a node",
        "{\"type\":\"object\",\"properties\":{"
        "\"node_id\":{\"type\":\"number\",\"description\":\"Scene node ID\"},"
        "\"name\":{\"type\":\"string\",\"description\":\"State name\"},"
        "\"clip\":{\"type\":\"string\",\"description\":\"Animation clip name\"},"
        "\"loop\":{\"type\":\"boolean\",\"description\":\"Whether the clip loops (default true)\"}"
        "},\"required\":[\"node_id\",\"name\",\"clip\"]}");
    cd_mcp_register_tool_ex(server, "anim.add_transition", handle_anim_add_transition,
        "Add a transition between animation states",
        "{\"type\":\"object\",\"properties\":{"
        "\"node_id\":{\"type\":\"number\",\"description\":\"Scene node ID\"},"
        "\"from\":{\"type\":\"string\",\"description\":\"Source state name\"},"
        "\"to\":{\"type\":\"string\",\"description\":\"Target state name\"},"
        "\"condition\":{\"type\":\"string\",\"description\":\"Transition condition name\"},"
        "\"blend\":{\"type\":\"number\",\"description\":\"Blend duration in seconds (default 0.2)\"}"
        "},\"required\":[\"node_id\",\"from\",\"to\",\"condition\"]}");

    return CD_OK;
}
