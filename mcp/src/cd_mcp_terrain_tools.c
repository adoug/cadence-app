/* cd_mcp_terrain_tools.c - Cadence Engine MCP terrain tool handlers
 *
 * Implements:
 *   - terrain.create        : Create terrain with dimensions
 *   - terrain.loadHeightmap : Load heightmap file (RAW or PNG)
 *   - terrain.paint         : Brush-based height modification
 *   - terrain.setSplatTexture : Set layer texture
 *   - terrain.getHeight     : Query height at position
 *
 * All handlers write to kernel terrain_descs[] descriptors.
 * The renderer processes descriptors each frame — no GPU calls here.
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_error.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_terrain_desc.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

/* ============================================================================
 * Helpers
 * ============================================================================ */

/** Find a free terrain descriptor slot, or return -1. */
static int find_free_slot(cd_kernel_t* kernel) {
    if (!kernel || !cd_kernel_get_terrain_descs(kernel, NULL)) return -1;
    cd_terrain_desc_t* arr = (cd_terrain_desc_t*)cd_kernel_get_terrain_descs(kernel, NULL);
    for (uint32_t i = 0; i < cd_kernel_get_terrain_desc_count(kernel); i++) {
        if (!arr[i].active && !arr[i].pending_destroy) return (int)i;
    }
    return -1;
}

/** Find the first active terrain slot, or return -1. */
static int find_active_slot(cd_kernel_t* kernel) {
    if (!kernel || !cd_kernel_get_terrain_descs(kernel, NULL)) return -1;
    cd_terrain_desc_t* arr = (cd_terrain_desc_t*)cd_kernel_get_terrain_descs(kernel, NULL);
    for (uint32_t i = 0; i < cd_kernel_get_terrain_desc_count(kernel); i++) {
        if (arr[i].active) return (int)i;
    }
    return -1;
}

static cd_terrain_desc_t* get_desc(cd_kernel_t* kernel, int slot) {
    if (slot < 0 || !kernel || !cd_kernel_get_terrain_descs(kernel, NULL)) return NULL;
    if ((uint32_t)slot >= cd_kernel_get_terrain_desc_count(kernel)) return NULL;
    return &((cd_terrain_desc_t*)cd_kernel_get_terrain_descs(kernel, NULL))[slot];
}

/* ============================================================================
 * terrain.create
 * ============================================================================ */

static cJSON* handle_terrain_create(
        cd_kernel_t* kernel, const cJSON* params,
        int* error_code, const char** error_msg) {

    /* Destroy existing active terrain (only one via MCP for simplicity) */
    int existing = find_active_slot(kernel);
    if (existing >= 0) {
        cd_terrain_desc_t* old = get_desc(kernel, existing);
        if (old) old->pending_destroy = true;
    }

    int slot = find_free_slot(kernel);
    if (slot < 0) {
        /* If we just marked one for destroy, reuse it */
        if (existing >= 0) {
            cd_terrain_desc_t* old = get_desc(kernel, existing);
            if (old) {
                old->pending_destroy = false;
                old->active = false;
                old->renderer_initialized = false;
            }
            slot = existing;
        } else {
            char detail[256];
            snprintf(detail, sizeof(detail),
                     "All %u terrain descriptor slots are occupied and none are pending destroy.",
                     cd_kernel_get_terrain_desc_count(kernel));
            *error_code = CD_JSONRPC_INTERNAL_ERROR;
            *error_msg  = cd_mcp_error_fmt(
                "No free terrain slots",
                detail,
                "Destroy an existing terrain before creating a new one.");
            return NULL;
        }
    }

    uint32_t width = 257, depth = 257, chunk_size = 33;
    float world_w = 100.0f, world_d = 100.0f, h_scale = 50.0f;

    if (params) {
        const cJSON* j;
        j = cJSON_GetObjectItemCaseSensitive(params, "width");
        if (cJSON_IsNumber(j)) width = (uint32_t)j->valuedouble;
        j = cJSON_GetObjectItemCaseSensitive(params, "depth");
        if (cJSON_IsNumber(j)) depth = (uint32_t)j->valuedouble;
        j = cJSON_GetObjectItemCaseSensitive(params, "worldWidth");
        if (cJSON_IsNumber(j)) world_w = (float)j->valuedouble;
        j = cJSON_GetObjectItemCaseSensitive(params, "worldDepth");
        if (cJSON_IsNumber(j)) world_d = (float)j->valuedouble;
        j = cJSON_GetObjectItemCaseSensitive(params, "heightScale");
        if (cJSON_IsNumber(j)) h_scale = (float)j->valuedouble;
        j = cJSON_GetObjectItemCaseSensitive(params, "chunkSize");
        if (cJSON_IsNumber(j)) chunk_size = (uint32_t)j->valuedouble;
    }

    cd_terrain_desc_t* desc = get_desc(kernel, slot);
    memset(desc, 0, sizeof(*desc));
    desc->active       = true;
    desc->slot_id      = (uint32_t)slot;
    desc->height_res_x = width;
    desc->height_res_z = depth;
    desc->world_width  = world_w;
    desc->world_depth  = world_d;
    desc->height_scale = h_scale;
    desc->chunk_size   = chunk_size;
    desc->heightmap_path[0] = '\0';  /* empty = generate flat */
    desc->heightmap_dirty   = true;

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddNumberToObject(result, "width", width);
    cJSON_AddNumberToObject(result, "depth", depth);
    cJSON_AddNumberToObject(result, "worldWidth", world_w);
    cJSON_AddNumberToObject(result, "worldDepth", world_d);
    cJSON_AddNumberToObject(result, "heightScale", h_scale);
    cJSON_AddNumberToObject(result, "slot", slot);
    return result;
}

/* ============================================================================
 * terrain.loadHeightmap
 * ============================================================================ */

static cJSON* handle_terrain_load_heightmap(
        cd_kernel_t* kernel, const cJSON* params,
        int* error_code, const char** error_msg) {

    int slot = find_active_slot(kernel);
    if (slot < 0) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt(
            "No terrain created",
            "terrain.loadHeightmap requires an active terrain to load the heightmap into.",
            "Call terrain.create first to initialize a terrain, then load a heightmap.");
        return NULL;
    }

    const cJSON* path_j = cJSON_GetObjectItemCaseSensitive(params, "path");
    if (!cJSON_IsString(path_j) || !path_j->valuestring[0]) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt(
            "Missing required parameter: path",
            "terrain.loadHeightmap requires a 'path' string to a RAW or PNG heightmap file.",
            "Example: {\"path\": \"assets/heightmap.raw\"} or {\"path\": \"assets/terrain.png\"}");
        return NULL;
    }

    cd_terrain_desc_t* desc = get_desc(kernel, slot);
    snprintf(desc->heightmap_path, sizeof(desc->heightmap_path),
             "%s", path_j->valuestring);
    desc->heightmap_dirty = true;

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "path", path_j->valuestring);
    return result;
}

/* ============================================================================
 * terrain.paint
 * ============================================================================ */

static cJSON* handle_terrain_paint(
        cd_kernel_t* kernel, const cJSON* params,
        int* error_code, const char** error_msg) {

    int slot = find_active_slot(kernel);
    if (slot < 0) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt(
            "No terrain created",
            "terrain.paint requires an active terrain to apply brush strokes to.",
            "Call terrain.create first to initialize a terrain.");
        return NULL;
    }

    cd_terrain_desc_t* desc = get_desc(kernel, slot);

    float x = 0.0f, z = 0.0f, radius = 5.0f, strength = 0.05f;
    cd_terrain_paint_mode_t mode = CD_TERRAIN_PAINT_RAISE;

    if (params) {
        const cJSON* j;
        j = cJSON_GetObjectItemCaseSensitive(params, "x");
        if (cJSON_IsNumber(j)) x = (float)j->valuedouble;
        j = cJSON_GetObjectItemCaseSensitive(params, "z");
        if (cJSON_IsNumber(j)) z = (float)j->valuedouble;
        j = cJSON_GetObjectItemCaseSensitive(params, "radius");
        if (cJSON_IsNumber(j)) radius = (float)j->valuedouble;
        j = cJSON_GetObjectItemCaseSensitive(params, "strength");
        if (cJSON_IsNumber(j)) strength = (float)j->valuedouble;

        j = cJSON_GetObjectItemCaseSensitive(params, "mode");
        if (cJSON_IsString(j)) {
            if (strcmp(j->valuestring, "lower") == 0)
                mode = CD_TERRAIN_PAINT_LOWER;
            else if (strcmp(j->valuestring, "smooth") == 0)
                mode = CD_TERRAIN_PAINT_SMOOTH;
            else if (strcmp(j->valuestring, "flatten") == 0)
                mode = CD_TERRAIN_PAINT_FLATTEN;
        }
    }

    if (desc->paint_count >= CD_TERRAIN_DESC_MAX_PAINTS) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "Paint queue has %u/%d commands pending. The renderer has not processed them yet.",
                 desc->paint_count, CD_TERRAIN_DESC_MAX_PAINTS);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt(
            "Paint queue full",
            detail,
            "Wait for the next frame to let the renderer process pending paints, then retry.");
        return NULL;
    }

    cd_terrain_paint_cmd_t* cmd = &desc->paint_queue[desc->paint_count++];
    cmd->world_x  = x;
    cmd->world_z  = z;
    cmd->radius   = radius;
    cmd->strength = strength;
    cmd->mode     = mode;

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    /* Height at center will be available after renderer processes the paint */
    if (desc->heights_ready && desc->heights) {
        float norm_x = (x / desc->heights_world_w) * (float)(desc->heights_res_x - 1);
        float norm_z = (z / desc->heights_world_d) * (float)(desc->heights_res_z - 1);
        if (norm_x >= 0 && norm_z >= 0 &&
            norm_x < (float)desc->heights_res_x &&
            norm_z < (float)desc->heights_res_z) {
            uint32_t ix = (uint32_t)norm_x;
            uint32_t iz = (uint32_t)norm_z;
            float h = desc->heights[iz * desc->heights_res_x + ix]
                      * desc->heights_height_scale;
            cJSON_AddNumberToObject(result, "heightAtCenter", h);
        }
    }
    return result;
}

/* ============================================================================
 * terrain.setSplatTexture
 * ============================================================================ */

static cJSON* handle_terrain_set_splat_texture(
        cd_kernel_t* kernel, const cJSON* params,
        int* error_code, const char** error_msg) {

    int slot = find_active_slot(kernel);
    if (slot < 0) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt(
            "No terrain created",
            "terrain.setSplatTexture requires an active terrain to assign textures to.",
            "Call terrain.create first to initialize a terrain.");
        return NULL;
    }

    const cJSON* layer_j = cJSON_GetObjectItemCaseSensitive(params, "layer");
    const cJSON* path_j  = cJSON_GetObjectItemCaseSensitive(params, "path");

    if (!cJSON_IsNumber(layer_j) || !cJSON_IsString(path_j)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt(
            "Missing required parameters: layer, path",
            "terrain.setSplatTexture requires 'layer' (int 0-3) and 'path' (string to texture file).",
            "Example: {\"layer\": 0, \"path\": \"assets/grass.png\", \"uvScale\": 10.0}");
        return NULL;
    }

    uint32_t layer = (uint32_t)layer_j->valuedouble;
    if (layer >= CD_TERRAIN_DESC_MAX_SPLAT) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "Received layer=%u, but valid range is 0-%d.",
                 layer, CD_TERRAIN_DESC_MAX_SPLAT - 1);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt(
            "Layer index out of range",
            detail,
            "Use layer 0-3. Layer 0 is the base texture; layers 1-3 are painted via splat map.");
        return NULL;
    }

    cd_terrain_desc_t* desc = get_desc(kernel, slot);
    const char* path = path_j->valuestring;

    snprintf(desc->splat_texture_paths[layer],
             sizeof(desc->splat_texture_paths[layer]), "%s", path);
    desc->splat_textures_dirty[layer] = true;

    /* Optional UV scale */
    const cJSON* uv_j = cJSON_GetObjectItemCaseSensitive(params, "uvScale");
    if (cJSON_IsNumber(uv_j)) {
        desc->splat_uv_scales[layer] = (float)uv_j->valuedouble;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddNumberToObject(result, "layer", layer);
    cJSON_AddStringToObject(result, "path", path);
    return result;
}

/* ============================================================================
 * terrain.getHeight
 * ============================================================================ */

static cJSON* handle_terrain_get_height(
        cd_kernel_t* kernel, const cJSON* params,
        int* error_code, const char** error_msg) {

    int slot = find_active_slot(kernel);
    if (slot < 0) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt(
            "No terrain created",
            "terrain.getHeight requires an active terrain to sample height from.",
            "Call terrain.create first to initialize a terrain.");
        return NULL;
    }

    float x = 0.0f, z = 0.0f;
    if (params) {
        const cJSON* j;
        j = cJSON_GetObjectItemCaseSensitive(params, "x");
        if (cJSON_IsNumber(j)) x = (float)j->valuedouble;
        j = cJSON_GetObjectItemCaseSensitive(params, "z");
        if (cJSON_IsNumber(j)) z = (float)j->valuedouble;
    }

    cd_terrain_desc_t* desc = get_desc(kernel, slot);
    float h = 0.0f;

    if (desc->heights_ready && desc->heights) {
        float norm_x = (x / desc->heights_world_w) * (float)(desc->heights_res_x - 1);
        float norm_z = (z / desc->heights_world_d) * (float)(desc->heights_res_z - 1);

        if (norm_x < 0.0f) norm_x = 0.0f;
        if (norm_z < 0.0f) norm_z = 0.0f;
        if (norm_x >= (float)(desc->heights_res_x - 1))
            norm_x = (float)(desc->heights_res_x - 1) - 0.001f;
        if (norm_z >= (float)(desc->heights_res_z - 1))
            norm_z = (float)(desc->heights_res_z - 1) - 0.001f;

        uint32_t ix = (uint32_t)norm_x;
        uint32_t iz = (uint32_t)norm_z;
        float fx = norm_x - (float)ix;
        float fz = norm_z - (float)iz;

        uint32_t rx = desc->heights_res_x;
        float h00 = desc->heights[iz * rx + ix];
        float h10 = desc->heights[iz * rx + ix + 1];
        float h01 = desc->heights[(iz + 1) * rx + ix];
        float h11 = desc->heights[(iz + 1) * rx + ix + 1];

        h = (h00 * (1.0f - fx) * (1.0f - fz) +
             h10 * fx * (1.0f - fz) +
             h01 * (1.0f - fx) * fz +
             h11 * fx * fz) * desc->heights_height_scale;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "x", x);
    cJSON_AddNumberToObject(result, "z", z);
    cJSON_AddNumberToObject(result, "height", h);
    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_terrain_tools(cd_mcp_server_t* server) {
    if (!server) return CD_ERR_NULL;

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "terrain.create", handle_terrain_create,
        "Create a new terrain with specified dimensions",
        "{\"type\":\"object\",\"properties\":{"
        "\"width\":{\"type\":\"number\",\"description\":\"Heightmap resolution X (default 257)\"},"
        "\"depth\":{\"type\":\"number\",\"description\":\"Heightmap resolution Z (default 257)\"},"
        "\"worldWidth\":{\"type\":\"number\",\"description\":\"World-space width (default 100)\"},"
        "\"worldDepth\":{\"type\":\"number\",\"description\":\"World-space depth (default 100)\"},"
        "\"heightScale\":{\"type\":\"number\",\"description\":\"Height scale factor (default 50)\"},"
        "\"chunkSize\":{\"type\":\"number\",\"description\":\"Chunk size in vertices (default 33)\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "terrain.loadHeightmap",
        handle_terrain_load_heightmap,
        "Load a RAW or PNG heightmap file onto the active terrain",
        "{\"type\":\"object\",\"properties\":{"
        "\"path\":{\"type\":\"string\",\"description\":\"Path to heightmap file (RAW or PNG)\"}"
        "},\"required\":[\"path\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "terrain.paint", handle_terrain_paint,
        "Apply a brush stroke to modify terrain height",
        "{\"type\":\"object\",\"properties\":{"
        "\"x\":{\"type\":\"number\",\"description\":\"World X position of brush center\"},"
        "\"z\":{\"type\":\"number\",\"description\":\"World Z position of brush center\"},"
        "\"radius\":{\"type\":\"number\",\"description\":\"Brush radius (default 5)\"},"
        "\"strength\":{\"type\":\"number\",\"description\":\"Brush strength (default 0.05)\"},"
        "\"mode\":{\"type\":\"string\",\"enum\":[\"raise\",\"lower\",\"smooth\",\"flatten\"],\"description\":\"Paint mode (default raise)\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "terrain.setSplatTexture",
        handle_terrain_set_splat_texture,
        "Assign a texture to a terrain splat layer",
        "{\"type\":\"object\",\"properties\":{"
        "\"layer\":{\"type\":\"number\",\"description\":\"Splat layer index (0-3)\"},"
        "\"path\":{\"type\":\"string\",\"description\":\"Path to texture file\"},"
        "\"uvScale\":{\"type\":\"number\",\"description\":\"UV tiling scale\"}"
        "},\"required\":[\"layer\",\"path\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "terrain.getHeight",
        handle_terrain_get_height,
        "Query the interpolated terrain height at a world position",
        "{\"type\":\"object\",\"properties\":{"
        "\"x\":{\"type\":\"number\",\"description\":\"World X position\"},"
        "\"z\":{\"type\":\"number\",\"description\":\"World Z position\"}"
        "}}");
    if (res != CD_OK) return res;

    return CD_OK;
}
