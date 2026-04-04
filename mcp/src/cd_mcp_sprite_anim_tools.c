/* cd_mcp_sprite_anim_tools.c - Cadence Engine MCP sprite animation tools (S1.5)
 *
 * Implements:
 *   - sprite_anim.setup   : Set frames grid on a node
 *   - sprite_anim.add_clip: Add animation clip
 *   - sprite_anim.play    : Play a clip
 *   - sprite_anim.query   : Get current animation state
 *
 * All handlers follow the cd_mcp_tool_handler_t signature.
 *
 * Sprite animation state is stored in a file-scoped array keyed by node ID,
 * matching the pattern used by the Lua bindings. This file manages its own
 * instances independently of the Lua system (they operate on separate state
 * for MCP-only workflows, e.g., headless testing).
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_sprite_anim.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Internal animation storage (per-node, MCP-side)
 * ============================================================================ */

#define CD_MCP_SA_MAX 128

typedef struct {
    cd_id_t          node_id;
    cd_sprite_anim_t anim;
    bool             active;
} cd_mcp_sa_entry_t;

static cd_mcp_sa_entry_t s_mcp_sa[CD_MCP_SA_MAX];
static uint32_t          s_mcp_sa_count = 0;

static cd_mcp_sa_entry_t* mcp_sa_find_or_create(cd_id_t node_id) {
    for (uint32_t i = 0; i < s_mcp_sa_count; i++) {
        if (s_mcp_sa[i].active && s_mcp_sa[i].node_id == node_id)
            return &s_mcp_sa[i];
    }
    for (uint32_t i = 0; i < CD_MCP_SA_MAX; i++) {
        if (!s_mcp_sa[i].active) {
            memset(&s_mcp_sa[i], 0, sizeof(s_mcp_sa[i]));
            s_mcp_sa[i].node_id = node_id;
            s_mcp_sa[i].active  = true;
            cd_sprite_anim_init(&s_mcp_sa[i].anim);
            if (i >= s_mcp_sa_count) s_mcp_sa_count = i + 1;
            return &s_mcp_sa[i];
        }
    }
    return NULL;
}

static cd_mcp_sa_entry_t* mcp_sa_find(cd_id_t node_id) {
    for (uint32_t i = 0; i < s_mcp_sa_count; i++) {
        if (s_mcp_sa[i].active && s_mcp_sa[i].node_id == node_id)
            return &s_mcp_sa[i];
    }
    return NULL;
}

/* ============================================================================
 * Helper: parse node_id from params
 * ============================================================================ */

static cd_id_t parse_node_id(const cJSON* params) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(params, "node_id");
    if (!item || !cJSON_IsNumber(item)) return CD_ID_INVALID;
    return (cd_id_t)(uint64_t)item->valuedouble;
}

/* ============================================================================
 * sprite_anim.setup
 * ============================================================================ */

static cJSON* cd_mcp_handle_sprite_anim_setup(
    struct cd_kernel_t* kernel, const cJSON* params,
    int* error_code, const char** error_msg)
{
    (void)kernel;

    cd_id_t node_id = parse_node_id(params);
    if (node_id == CD_ID_INVALID) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = "Missing or invalid node_id";
        return NULL;
    }

    const cJSON* cols_j  = cJSON_GetObjectItemCaseSensitive(params, "cols");
    const cJSON* rows_j  = cJSON_GetObjectItemCaseSensitive(params, "rows");
    const cJSON* total_j = cJSON_GetObjectItemCaseSensitive(params, "total_frames");
    const cJSON* fw_j    = cJSON_GetObjectItemCaseSensitive(params, "frame_w");
    const cJSON* fh_j    = cJSON_GetObjectItemCaseSensitive(params, "frame_h");

    if (!cols_j || !rows_j || !total_j || !fw_j || !fh_j ||
        !cJSON_IsNumber(cols_j) || !cJSON_IsNumber(rows_j) ||
        !cJSON_IsNumber(total_j) || !cJSON_IsNumber(fw_j) ||
        !cJSON_IsNumber(fh_j)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = "Missing required params: cols, rows, total_frames, frame_w, frame_h";
        return NULL;
    }

    cd_mcp_sa_entry_t* entry = mcp_sa_find_or_create(node_id);
    if (!entry) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = "Max sprite animations reached";
        return NULL;
    }

    cd_result_t res = cd_sprite_anim_set_frames_grid(
        &entry->anim,
        (uint32_t)cols_j->valueint,
        (uint32_t)rows_j->valueint,
        (uint32_t)total_j->valueint,
        (float)fw_j->valuedouble,
        (float)fh_j->valuedouble);

    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = "Failed to set frames grid";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "frame_count",
                             (double)entry->anim.frame_count);
    return result;
}

/* ============================================================================
 * sprite_anim.add_clip
 * ============================================================================ */

static cJSON* cd_mcp_handle_sprite_anim_add_clip(
    struct cd_kernel_t* kernel, const cJSON* params,
    int* error_code, const char** error_msg)
{
    (void)kernel;

    cd_id_t node_id = parse_node_id(params);
    if (node_id == CD_ID_INVALID) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = "Missing or invalid node_id";
        return NULL;
    }

    const cJSON* name_j  = cJSON_GetObjectItemCaseSensitive(params, "name");
    const cJSON* start_j = cJSON_GetObjectItemCaseSensitive(params, "frame_start");
    const cJSON* count_j = cJSON_GetObjectItemCaseSensitive(params, "frame_count");
    const cJSON* fps_j   = cJSON_GetObjectItemCaseSensitive(params, "fps");
    const cJSON* mode_j  = cJSON_GetObjectItemCaseSensitive(params, "mode");

    if (!name_j || !cJSON_IsString(name_j) ||
        !start_j || !cJSON_IsNumber(start_j) ||
        !count_j || !cJSON_IsNumber(count_j) ||
        !fps_j   || !cJSON_IsNumber(fps_j)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = "Missing required params: name, frame_start, frame_count, fps";
        return NULL;
    }

    cd_mcp_sa_entry_t* entry = mcp_sa_find(node_id);
    if (!entry) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = "No sprite animation setup for this node";
        return NULL;
    }

    cd_sprite_anim_mode_t mode = CD_SPRITE_ANIM_LOOP;
    if (mode_j && cJSON_IsString(mode_j)) {
        const char* m = mode_j->valuestring;
        if (strcmp(m, "once") == 0) mode = CD_SPRITE_ANIM_ONCE;
        else if (strcmp(m, "pingpong") == 0) mode = CD_SPRITE_ANIM_PINGPONG;
    }

    cd_result_t res = cd_sprite_anim_add_clip(
        &entry->anim,
        name_j->valuestring,
        (uint32_t)start_j->valueint,
        (uint32_t)count_j->valueint,
        (float)fps_j->valuedouble,
        mode);

    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = "Failed to add clip";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "clip_count",
                             (double)entry->anim.clip_count);
    return result;
}

/* ============================================================================
 * sprite_anim.play
 * ============================================================================ */

static cJSON* cd_mcp_handle_sprite_anim_play(
    struct cd_kernel_t* kernel, const cJSON* params,
    int* error_code, const char** error_msg)
{
    (void)kernel;

    cd_id_t node_id = parse_node_id(params);
    if (node_id == CD_ID_INVALID) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = "Missing or invalid node_id";
        return NULL;
    }

    const cJSON* clip_j = cJSON_GetObjectItemCaseSensitive(params, "clip");
    if (!clip_j || !cJSON_IsString(clip_j)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = "Missing required param: clip";
        return NULL;
    }

    cd_mcp_sa_entry_t* entry = mcp_sa_find(node_id);
    if (!entry) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = "No sprite animation setup for this node";
        return NULL;
    }

    cd_result_t res = cd_sprite_anim_play(&entry->anim, clip_j->valuestring);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = "Clip not found";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "clip", clip_j->valuestring);
    cJSON_AddBoolToObject(result, "playing", 1);
    return result;
}

/* ============================================================================
 * sprite_anim.query
 * ============================================================================ */

static cJSON* cd_mcp_handle_sprite_anim_query(
    struct cd_kernel_t* kernel, const cJSON* params,
    int* error_code, const char** error_msg)
{
    (void)kernel;

    cd_id_t node_id = parse_node_id(params);
    if (node_id == CD_ID_INVALID) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = "Missing or invalid node_id";
        return NULL;
    }

    cd_mcp_sa_entry_t* entry = mcp_sa_find(node_id);
    if (!entry) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = "No sprite animation for this node";
        return NULL;
    }

    cd_sprite_anim_t* anim = &entry->anim;

    cJSON* result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "frame_count", (double)anim->frame_count);
    cJSON_AddNumberToObject(result, "clip_count", (double)anim->clip_count);
    cJSON_AddNumberToObject(result, "current_clip", (double)anim->current_clip);
    cJSON_AddNumberToObject(result, "current_frame", (double)anim->current_frame);
    cJSON_AddBoolToObject(result, "playing", anim->playing);
    cJSON_AddBoolToObject(result, "finished", anim->finished);

    if (anim->current_clip >= 0 &&
        (uint32_t)anim->current_clip < anim->clip_count) {
        cJSON_AddStringToObject(result, "clip_name",
                                 anim->clips[anim->current_clip].name);
    }

    /* List all clips */
    cJSON* clips_arr = cJSON_CreateArray();
    for (uint32_t i = 0; i < anim->clip_count; i++) {
        cJSON* c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "name", anim->clips[i].name);
        cJSON_AddNumberToObject(c, "frame_start",
                                 (double)anim->clips[i].frame_start);
        cJSON_AddNumberToObject(c, "frame_count",
                                 (double)anim->clips[i].frame_count);
        cJSON_AddNumberToObject(c, "fps", (double)anim->clips[i].fps);
        const char* mode_str = "loop";
        if (anim->clips[i].mode == CD_SPRITE_ANIM_ONCE) mode_str = "once";
        else if (anim->clips[i].mode == CD_SPRITE_ANIM_PINGPONG) mode_str = "pingpong";
        cJSON_AddStringToObject(c, "mode", mode_str);
        cJSON_AddItemToArray(clips_arr, c);
    }
    cJSON_AddItemToObject(result, "clips", clips_arr);

    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_sprite_anim_tools(cd_mcp_server_t* server) {
    if (!server) return CD_ERR_NULL;

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "sprite_anim.setup",
        cd_mcp_handle_sprite_anim_setup,
        "Set up sprite animation frames grid on a node",
        "{\"type\":\"object\",\"properties\":{"
        "\"node_id\":{\"type\":\"number\",\"description\":\"Scene node ID\"},"
        "\"cols\":{\"type\":\"number\",\"description\":\"Number of columns in sprite sheet\"},"
        "\"rows\":{\"type\":\"number\",\"description\":\"Number of rows in sprite sheet\"},"
        "\"total_frames\":{\"type\":\"number\",\"description\":\"Total frame count\"},"
        "\"frame_w\":{\"type\":\"number\",\"description\":\"Frame width in pixels\"},"
        "\"frame_h\":{\"type\":\"number\",\"description\":\"Frame height in pixels\"}"
        "},\"required\":[\"node_id\",\"cols\",\"rows\",\"total_frames\",\"frame_w\",\"frame_h\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "sprite_anim.add_clip",
        cd_mcp_handle_sprite_anim_add_clip,
        "Add a named animation clip to a sprite animation",
        "{\"type\":\"object\",\"properties\":{"
        "\"node_id\":{\"type\":\"number\",\"description\":\"Scene node ID\"},"
        "\"name\":{\"type\":\"string\",\"description\":\"Clip name\"},"
        "\"frame_start\":{\"type\":\"number\",\"description\":\"First frame index\"},"
        "\"frame_count\":{\"type\":\"number\",\"description\":\"Number of frames in clip\"},"
        "\"fps\":{\"type\":\"number\",\"description\":\"Playback speed in frames per second\"},"
        "\"mode\":{\"type\":\"string\",\"enum\":[\"loop\",\"once\",\"pingpong\"],\"description\":\"Playback mode (default loop)\"}"
        "},\"required\":[\"node_id\",\"name\",\"frame_start\",\"frame_count\",\"fps\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "sprite_anim.play",
        cd_mcp_handle_sprite_anim_play,
        "Play a named sprite animation clip",
        "{\"type\":\"object\",\"properties\":{"
        "\"node_id\":{\"type\":\"number\",\"description\":\"Scene node ID\"},"
        "\"clip\":{\"type\":\"string\",\"description\":\"Clip name to play\"}"
        "},\"required\":[\"node_id\",\"clip\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "sprite_anim.query",
        cd_mcp_handle_sprite_anim_query,
        "Get current sprite animation state and clip list",
        "{\"type\":\"object\",\"properties\":{"
        "\"node_id\":{\"type\":\"number\",\"description\":\"Scene node ID\"}"
        "},\"required\":[\"node_id\"]}");
    if (res != CD_OK) return res;

    return CD_OK;
}
