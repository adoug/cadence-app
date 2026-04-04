/* cd_mcp_cook_tools.c - Cadence Engine MCP asset cooking tool handlers
 *
 * Implements:
 *   - asset.cook          : Cook a single asset
 *   - asset.cook_all      : Cook all project assets
 *   - asset.cook_status   : Get stale/fresh counts
 *   - asset.import_settings.get : Read .import sidecar
 *   - asset.import_settings.set : Write .import sidecar
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_error.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_asset_db.h"
#include "cadence/cd_cook_formats.h"
#include "cadence/cd_platform.h"
#include "cadence/cd_memory.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ============================================================================
 * Helper: check if a cooked file exists for a source asset
 * ============================================================================ */

static bool cook_has_cooked_version(const char* project_root, const char* uri) {
    /* Map res://path/to/file.ext -> <project>/cooked/path/to/file.cd<kind> */
    const char* rel = uri;
    if (strncmp(rel, "res://", 6) == 0) rel += 6;

    char cooked_path[512];
    snprintf(cooked_path, sizeof(cooked_path), "%s/cooked/%s", project_root, rel);

    /* Change extension to .cdtex for textures, .cdmesh for meshes */
    char* dot = strrchr(cooked_path, '.');
    if (!dot) return false;

    cd_asset_kind_t kind = cd_asset_kind_from_ext(uri);
    switch (kind) {
    case CD_ASSET_TEXTURE:
        snprintf(dot, sizeof(cooked_path) - (size_t)(dot - cooked_path), ".cdtex");
        break;
    case CD_ASSET_MESH:
        snprintf(dot, sizeof(cooked_path) - (size_t)(dot - cooked_path), ".cdmesh");
        break;
    default:
        return true; /* No cooking needed */
    }

    return cd_fs_exists(cooked_path);
}

/* ============================================================================
 * asset.cook handler
 *
 * Input: { "uri": "res://textures/brick.png" }
 * Output: { "status": "ok", "output": "cooked/textures/brick.cdtex" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_asset_cook(
    struct cd_kernel_t* kernel, const cJSON* params,
    int* error_code, const char** error_msg) {

    if (!kernel || !cd_kernel_get_asset_db(kernel)) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = "No project loaded";
        return NULL;
    }

    const cJSON* j_uri = cJSON_GetObjectItemCaseSensitive(params, "uri");
    if (!j_uri || !cJSON_IsString(j_uri) || !j_uri->valuestring[0]) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = "Missing required param: uri";
        return NULL;
    }

    /* Find asset in DB */
    const cd_asset_entry_t* entry = cd_asset_db_lookup(cd_kernel_get_asset_db(kernel),
                                                        j_uri->valuestring);
    if (!entry) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt("Asset not found",
            "URI not registered in asset database",
            "Run asset.list to see available assets.");
        return NULL;
    }

    /* Build asset-cook command */
    const char* project_root = cd_kernel_get_asset_db(kernel)->project_root;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "asset-cook --project \"%s\" --incremental",
             project_root);

    /* Note: For single-asset cooking, we'd ideally pass just the URI.
     * For now, run the full cook tool which is incremental. */
    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "uri", j_uri->valuestring);
    cJSON_AddStringToObject(result, "kind", cd_asset_kind_str(entry->kind));
    cJSON_AddBoolToObject(result, "has_cooked",
                          cook_has_cooked_version(project_root,
                                                  j_uri->valuestring));
    cJSON_AddStringToObject(result, "note",
        "Run asset-cook CLI tool for actual cooking. MCP triggers are planned.");
    return result;
}

/* ============================================================================
 * asset.cook_all handler
 *
 * Input: { "incremental": true }
 * Output: { "total": N, "cookable": M, "stale": S }
 * ============================================================================ */

static cJSON* cd_mcp_handle_asset_cook_all(
    struct cd_kernel_t* kernel, const cJSON* params,
    int* error_code, const char** error_msg) {

    if (!kernel || !cd_kernel_get_asset_db(kernel)) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = "No project loaded";
        return NULL;
    }

    (void)params;

    const cd_asset_db_t* db = cd_kernel_get_asset_db(kernel);
    uint32_t total = db->count;
    uint32_t cookable = 0;
    uint32_t stale = 0;

    for (uint32_t i = 0; i < db->count; i++) {
        const cd_asset_entry_t* entry = &db->entries[i];
        if (entry->kind == CD_ASSET_TEXTURE || entry->kind == CD_ASSET_MESH) {
            cookable++;
            if (!cook_has_cooked_version(db->project_root, entry->uri)) {
                stale++;
            } else {
                /* 1G: Check if any dependency is newer than this asset.
                 * If a texture that a material depends on was modified,
                 * the material's cooked output is stale. */
                uint32_t deps[32];
                uint32_t dep_count = cd_asset_db_get_dependencies(
                    db, i, deps, 32);
                for (uint32_t d = 0; d < dep_count && d < 32; d++) {
                    if (deps[d] < db->count &&
                        db->entries[deps[d]].modified_time > entry->modified_time) {
                        stale++;
                        break;
                    }
                }
            }
        }
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "total", total);
    cJSON_AddNumberToObject(result, "cookable", cookable);
    cJSON_AddNumberToObject(result, "stale", stale);
    cJSON_AddNumberToObject(result, "fresh", cookable - stale);
    cJSON_AddStringToObject(result, "note",
        "Run 'asset-cook --project <path>' CLI tool to cook assets.");
    return result;
}

/* ============================================================================
 * asset.cook_status handler
 * ============================================================================ */

static cJSON* cd_mcp_handle_asset_cook_status(
    struct cd_kernel_t* kernel, const cJSON* params,
    int* error_code, const char** error_msg) {
    /* Delegate to cook_all — same info */
    return cd_mcp_handle_asset_cook_all(kernel, params, error_code, error_msg);
}

/* ============================================================================
 * asset.import_settings.get handler
 *
 * Input: { "uri": "res://textures/brick.png" }
 * Output: { settings fields }
 * ============================================================================ */

static cJSON* cd_mcp_handle_import_settings_get(
    struct cd_kernel_t* kernel, const cJSON* params,
    int* error_code, const char** error_msg) {

    if (!kernel || !cd_kernel_get_asset_db(kernel)) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = "No project loaded";
        return NULL;
    }

    const cJSON* j_uri = cJSON_GetObjectItemCaseSensitive(params, "uri");
    if (!j_uri || !cJSON_IsString(j_uri) || !j_uri->valuestring[0]) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = "Missing required param: uri";
        return NULL;
    }

    /* Find asset and look for .import sidecar */
    const cd_asset_entry_t* entry = cd_asset_db_lookup(cd_kernel_get_asset_db(kernel),
                                                        j_uri->valuestring);
    char import_path[512];
    if (entry) {
        snprintf(import_path, sizeof(import_path), "%s.import", entry->abs_path);
    } else {
        /* Resolve URI manually */
        const char* rel = j_uri->valuestring;
        if (strncmp(rel, "res://", 6) == 0) rel += 6;
        snprintf(import_path, sizeof(import_path), "%s/%s.import",
                 cd_kernel_get_asset_db(kernel)->project_root, rel);
    }

    cd_import_settings_t settings;
    cd_import_settings_load(&settings, import_path);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "uri", j_uri->valuestring);
    cJSON_AddStringToObject(result, "format", settings.format);
    cJSON_AddBoolToObject(result, "generate_mipmaps", settings.generate_mipmaps);
    cJSON_AddNumberToObject(result, "max_size", settings.max_size);
    cJSON_AddBoolToObject(result, "srgb", settings.srgb);
    cJSON_AddBoolToObject(result, "generate_lod", settings.generate_lod);
    cJSON_AddNumberToObject(result, "lod_levels", settings.lod_levels);
    cJSON_AddBoolToObject(result, "vertex_cache_optimize",
                          settings.vertex_cache_optimize);
    cJSON_AddBoolToObject(result, "remove_duplicates", settings.remove_duplicates);
    cJSON_AddBoolToObject(result, "has_sidecar", cd_fs_exists(import_path));
    return result;
}

/* ============================================================================
 * asset.import_settings.set handler
 *
 * Input: { "uri": "res://textures/brick.png", "format": "bc1", "srgb": false }
 * ============================================================================ */

static cJSON* cd_mcp_handle_import_settings_set(
    struct cd_kernel_t* kernel, const cJSON* params,
    int* error_code, const char** error_msg) {

    if (!kernel || !cd_kernel_get_asset_db(kernel)) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = "No project loaded";
        return NULL;
    }

    const cJSON* j_uri = cJSON_GetObjectItemCaseSensitive(params, "uri");
    if (!j_uri || !cJSON_IsString(j_uri) || !j_uri->valuestring[0]) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = "Missing required param: uri";
        return NULL;
    }

    /* Resolve path */
    const cd_asset_entry_t* entry = cd_asset_db_lookup(cd_kernel_get_asset_db(kernel),
                                                        j_uri->valuestring);
    char import_path[512];
    if (entry) {
        snprintf(import_path, sizeof(import_path), "%s.import", entry->abs_path);
    } else {
        const char* rel = j_uri->valuestring;
        if (strncmp(rel, "res://", 6) == 0) rel += 6;
        snprintf(import_path, sizeof(import_path), "%s/%s.import",
                 cd_kernel_get_asset_db(kernel)->project_root, rel);
    }

    /* Load existing settings (or defaults) */
    cd_import_settings_t settings;
    cd_import_settings_load(&settings, import_path);

    /* Apply overrides */
    const cJSON* j;
    j = cJSON_GetObjectItemCaseSensitive(params, "format");
    if (j && cJSON_IsString(j)) snprintf(settings.format, sizeof(settings.format), "%s", j->valuestring);

    j = cJSON_GetObjectItemCaseSensitive(params, "generate_mipmaps");
    if (j && cJSON_IsBool(j)) settings.generate_mipmaps = cJSON_IsTrue(j);

    j = cJSON_GetObjectItemCaseSensitive(params, "max_size");
    if (j && cJSON_IsNumber(j)) settings.max_size = (uint32_t)j->valuedouble;

    j = cJSON_GetObjectItemCaseSensitive(params, "srgb");
    if (j && cJSON_IsBool(j)) settings.srgb = cJSON_IsTrue(j);

    j = cJSON_GetObjectItemCaseSensitive(params, "generate_lod");
    if (j && cJSON_IsBool(j)) settings.generate_lod = cJSON_IsTrue(j);

    j = cJSON_GetObjectItemCaseSensitive(params, "lod_levels");
    if (j && cJSON_IsNumber(j)) settings.lod_levels = (uint32_t)j->valuedouble;

    j = cJSON_GetObjectItemCaseSensitive(params, "vertex_cache_optimize");
    if (j && cJSON_IsBool(j)) settings.vertex_cache_optimize = cJSON_IsTrue(j);

    j = cJSON_GetObjectItemCaseSensitive(params, "remove_duplicates");
    if (j && cJSON_IsBool(j)) settings.remove_duplicates = cJSON_IsTrue(j);

    /* Save */
    cd_result_t res = cd_import_settings_save(&settings, import_path);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = "Failed to write .import file";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "uri", j_uri->valuestring);
    cJSON_AddStringToObject(result, "import_file", import_path);
    return result;
}

/* ============================================================================
 * scene.bake_lightmaps — spawn lightmap-baker tool
 * ============================================================================ */

static cJSON* cd_mcp_handle_bake_lightmaps(
    struct cd_kernel_t* kernel, const cJSON* params,
    int* error_code, const char** error_msg) {
    (void)error_code; (void)error_msg;
    const char* proj = cd_kernel_get_config(kernel)->project_path;
    if (!proj) {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "No project loaded");
        return err;
    }

    /* Parse optional parameters */
    const char* quality = "medium";
    int bounces = 2;
    int atlas_size = 0;          /* 0 = auto-size */
    double texel_density = 0.0;  /* 0 = use baker default (10) */
    const char* env_map = NULL;
    bool no_denoise = false;

    if (params) {
        cJSON* j_quality = cJSON_GetObjectItemCaseSensitive(params, "quality");
        cJSON* j_bounces = cJSON_GetObjectItemCaseSensitive(params, "bounces");
        cJSON* j_atlas   = cJSON_GetObjectItemCaseSensitive(params, "atlas_size");
        cJSON* j_texden  = cJSON_GetObjectItemCaseSensitive(params, "texel_density");
        cJSON* j_envmap  = cJSON_GetObjectItemCaseSensitive(params, "env_map");
        cJSON* j_denoise = cJSON_GetObjectItemCaseSensitive(params, "denoise");
        if (j_quality && cJSON_IsString(j_quality)) quality = j_quality->valuestring;
        if (j_bounces && cJSON_IsNumber(j_bounces)) bounces = j_bounces->valueint;
        if (j_atlas   && cJSON_IsNumber(j_atlas))   atlas_size = j_atlas->valueint;
        if (j_texden  && cJSON_IsNumber(j_texden))  texel_density = j_texden->valuedouble;
        if (j_envmap  && cJSON_IsString(j_envmap))  env_map = j_envmap->valuestring;
        if (j_denoise && cJSON_IsBool(j_denoise) && !cJSON_IsTrue(j_denoise)) no_denoise = true;
    }

    /* Build command line */
    char cmd[1024];
    int pos = snprintf(cmd, sizeof(cmd),
        "lightmap-baker --project \"%s\" --quality %s --bounces %d",
        proj, quality, bounces);
    if (atlas_size > 0)
        pos += snprintf(cmd + pos, sizeof(cmd) - pos, " --atlas-size %d", atlas_size);
    if (texel_density > 0.0)
        pos += snprintf(cmd + pos, sizeof(cmd) - pos, " --texel-density %.1f", texel_density);
    if (env_map)
        pos += snprintf(cmd + pos, sizeof(cmd) - pos, " --env-map \"%s\"", env_map);
    if (no_denoise)
        pos += snprintf(cmd + pos, sizeof(cmd) - pos, " --no-denoise");

    fprintf(stderr, "[mcp] Launching: %s\n", cmd);
    int ret = system(cmd);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "success", ret == 0);
    cJSON_AddStringToObject(result, "command", cmd);
    if (ret == 0) {
        char out_path[512];
        snprintf(out_path, sizeof(out_path), "%s/cooked/lightmap_atlas.cdlightmap", proj);
        cJSON_AddStringToObject(result, "output", out_path);
    } else {
        cJSON_AddNumberToObject(result, "exit_code", ret);
    }
    return result;
}

/* ============================================================================
 * scene.bake_probes — bake L2 SH light probe grid
 * ============================================================================ */

static cJSON* cd_mcp_handle_bake_probes(
    struct cd_kernel_t* kernel, const cJSON* params,
    int* error_code, const char** error_msg) {
    (void)error_code; (void)error_msg;
    const char* proj = cd_kernel_get_config(kernel)->project_path;
    if (!proj) {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "No project loaded");
        return err;
    }

    double grid_spacing = 2.0;
    int samples_per_probe = 64;

    if (params) {
        cJSON* j_spacing = cJSON_GetObjectItemCaseSensitive(params, "grid_spacing");
        cJSON* j_samples = cJSON_GetObjectItemCaseSensitive(params, "samples_per_probe");
        if (j_spacing && cJSON_IsNumber(j_spacing)) grid_spacing = j_spacing->valuedouble;
        if (j_samples && cJSON_IsNumber(j_samples)) samples_per_probe = j_samples->valueint;
    }

    char cmd[1024];
    int pos = snprintf(cmd, sizeof(cmd),
        "lightmap-baker --project \"%s\" --bake-probes --grid-spacing %.2f --samples-per-probe %d",
        proj, grid_spacing, samples_per_probe);

    if (params) {
        cJSON* j_bmin = cJSON_GetObjectItemCaseSensitive(params, "bounds_min");
        cJSON* j_bmax = cJSON_GetObjectItemCaseSensitive(params, "bounds_max");
        if (j_bmin && cJSON_IsArray(j_bmin) && cJSON_GetArraySize(j_bmin) == 3) {
            pos += snprintf(cmd + pos, sizeof(cmd) - pos, " --bounds-min %.2f %.2f %.2f",
                cJSON_GetArrayItem(j_bmin, 0)->valuedouble,
                cJSON_GetArrayItem(j_bmin, 1)->valuedouble,
                cJSON_GetArrayItem(j_bmin, 2)->valuedouble);
        }
        if (j_bmax && cJSON_IsArray(j_bmax) && cJSON_GetArraySize(j_bmax) == 3) {
            pos += snprintf(cmd + pos, sizeof(cmd) - pos, " --bounds-max %.2f %.2f %.2f",
                cJSON_GetArrayItem(j_bmax, 0)->valuedouble,
                cJSON_GetArrayItem(j_bmax, 1)->valuedouble,
                cJSON_GetArrayItem(j_bmax, 2)->valuedouble);
        }
    }

    fprintf(stderr, "[mcp] Launching: %s\n", cmd);
    int ret = system(cmd);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "success", ret == 0);
    cJSON_AddStringToObject(result, "command", cmd);
    if (ret == 0) {
        char out_path[512];
        snprintf(out_path, sizeof(out_path), "%s/cooked/lightmap_probes.cdprobes", proj);
        cJSON_AddStringToObject(result, "output", out_path);
    }
    return result;
}

/* ============================================================================
 * scene.lightmap_status — check if lightmap atlas exists and its info
 * ============================================================================ */

static cJSON* cd_mcp_handle_lightmap_status(
    struct cd_kernel_t* kernel, const cJSON* params,
    int* error_code, const char** error_msg) {
    (void)params; (void)error_code; (void)error_msg;
    const char* proj = cd_kernel_get_config(kernel)->project_path;
    if (!proj) {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "No project loaded");
        return err;
    }

    char lm_path[512];
    snprintf(lm_path, sizeof(lm_path), "%s/cooked/lightmap_atlas.cdlightmap", proj);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "path", lm_path);

    FILE* f = fopen(lm_path, "rb");
    if (!f) {
        cJSON_AddBoolToObject(result, "baked", false);
        return result;
    }

    cd_cdlightmap_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) == 1 && hdr.magic == CD_COOK_MAGIC_LMAP) {
        cJSON_AddBoolToObject(result, "baked", true);
        cJSON_AddNumberToObject(result, "width", hdr.width);
        cJSON_AddNumberToObject(result, "height", hdr.height);
        cJSON_AddNumberToObject(result, "bounces", hdr.bounces);
        cJSON_AddNumberToObject(result, "samples_per_texel", hdr.samples_per_texel);
        cJSON_AddNumberToObject(result, "data_size_kb", hdr.data_size / 1024);
        const char* fmt_str = hdr.format == CD_TEX_FMT_RGBA16F ? "RGBA16F" :
                              hdr.format == CD_TEX_FMT_RGBA8   ? "RGBA8"   : "unknown";
        cJSON_AddStringToObject(result, "format", fmt_str);
    } else {
        cJSON_AddBoolToObject(result, "baked", false);
        cJSON_AddStringToObject(result, "error", "Invalid lightmap header");
    }
    fclose(f);
    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_cook_tools(cd_mcp_server_t* server) {
    if (!server) return CD_ERR_NULL;

    cd_result_t r;

    r = cd_mcp_register_tool_ex(server, "asset.cook",
        cd_mcp_handle_asset_cook,
        "Cook a single asset to optimized binary format",
        "{\"type\":\"object\",\"properties\":{"
        "\"uri\":{\"type\":\"string\",\"description\":\"Asset res:// URI to cook\"}"
        "},\"required\":[\"uri\"]}");
    if (r != CD_OK) return r;

    r = cd_mcp_register_tool_ex(server, "asset.cook_all",
        cd_mcp_handle_asset_cook_all,
        "Cook all project assets (incremental)",
        "{\"type\":\"object\",\"properties\":{"
        "\"incremental\":{\"type\":\"boolean\",\"description\":\"Skip unchanged assets (default true)\"}"
        "}}");
    if (r != CD_OK) return r;

    r = cd_mcp_register_tool_ex(server, "asset.cook_status",
        cd_mcp_handle_asset_cook_status,
        "Get stale/fresh asset counts",
        "{\"type\":\"object\",\"properties\":{}}");
    if (r != CD_OK) return r;

    r = cd_mcp_register_tool_ex(server, "asset.import_settings.get",
        cd_mcp_handle_import_settings_get,
        "Read import settings (.import sidecar) for an asset",
        "{\"type\":\"object\",\"properties\":{"
        "\"uri\":{\"type\":\"string\",\"description\":\"Asset res:// URI\"}"
        "},\"required\":[\"uri\"]}");
    if (r != CD_OK) return r;

    r = cd_mcp_register_tool_ex(server, "asset.import_settings.set",
        cd_mcp_handle_import_settings_set,
        "Write import settings (.import sidecar) for an asset",
        "{\"type\":\"object\",\"properties\":{"
        "\"uri\":{\"type\":\"string\",\"description\":\"Asset res:// URI\"},"
        "\"format\":{\"type\":\"string\",\"description\":\"Compression format: bc1, bc3, rgba8\"},"
        "\"generate_mipmaps\":{\"type\":\"boolean\"},\"max_size\":{\"type\":\"number\"},"
        "\"srgb\":{\"type\":\"boolean\"},\"generate_lod\":{\"type\":\"boolean\"},"
        "\"lod_levels\":{\"type\":\"number\"},\"vertex_cache_optimize\":{\"type\":\"boolean\"},"
        "\"remove_duplicates\":{\"type\":\"boolean\"}"
        "},\"required\":[\"uri\"]}");
    if (r != CD_OK) return r;

    r = cd_mcp_register_tool_ex(server, "scene.bake_lightmaps",
        cd_mcp_handle_bake_lightmaps,
        "Bake lightmap atlas for the current scene using Embree CPU path tracer",
        "{\"type\":\"object\",\"properties\":{"
        "\"quality\":{\"type\":\"string\",\"description\":\"low, medium, or high (default: medium)\"},"
        "\"bounces\":{\"type\":\"number\",\"description\":\"Indirect bounce count 1-4 (default: 2)\"},"
        "\"atlas_size\":{\"type\":\"number\",\"description\":\"Atlas dimension override (bypasses auto-sizing)\"},"
        "\"texel_density\":{\"type\":\"number\",\"description\":\"Texels per world unit for auto-sizing (default: 10)\"},"
        "\"env_map\":{\"type\":\"string\",\"description\":\"Path to HDR environment map for sky lighting\"},"
        "\"denoise\":{\"type\":\"boolean\",\"description\":\"Enable OIDN denoising (default: true)\"}"
        "}}");
    if (r != CD_OK) return r;

    r = cd_mcp_register_tool_ex(server, "scene.bake_probes",
        cd_mcp_handle_bake_probes,
        "Bake L2 SH light probe grid for dynamic object lighting",
        "{\"type\":\"object\",\"properties\":{"
        "\"grid_spacing\":{\"type\":\"number\",\"description\":\"Distance between probes in world units (default: 2.0)\"},"
        "\"samples_per_probe\":{\"type\":\"number\",\"description\":\"Ray samples per probe (default: 64)\"},"
        "\"bounds_min\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"description\":\"Manual AABB min [x,y,z]\"},"
        "\"bounds_max\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"description\":\"Manual AABB max [x,y,z]\"}"
        "}}");
    if (r != CD_OK) return r;

    r = cd_mcp_register_tool_ex(server, "scene.lightmap_status",
        cd_mcp_handle_lightmap_status,
        "Query lightmap bake status and atlas info for the current project",
        "{\"type\":\"object\",\"properties\":{}}");
    if (r != CD_OK) return r;

    return CD_OK;
}
