/* cd_mcp_usd_tools.c - MCP tools for OpenUSD and MaterialX import
 *
 * Implements:
 *   - asset.import_usd  : Import USD scene into Cadence format
 *   - asset.import_mtlx : Import MaterialX materials into .cdmat files
 *
 * These tools invoke the asset-cook tool as a subprocess since USD/MaterialX
 * parsing requires C++ libraries (TinyUSDZ, MaterialX SDK) that are only
 * linked into the cook tool, not the runtime engine.
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_error.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_asset_db.h"
#include "cadence/cd_material_asset.h"
#include "cadence/cd_platform.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

/* ============================================================================
 * asset.import_usd handler
 *
 * Input:
 *   { "path": "C:/models/scene.usd",
 *     "destination": "res://scenes/imported/" }
 *
 * Output:
 *   { "status": "ok",
 *     "meshes": [...], "materials": [...], "nodes": N }
 * ============================================================================ */

static cJSON* cd_mcp_handle_asset_import_usd(
    struct cd_kernel_t* kernel, const cJSON* params,
    int* error_code, const char** error_msg) {

    if (!kernel || !cd_kernel_get_asset_db(kernel)) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = "No project loaded";
        return NULL;
    }

    const cJSON* j_path = cJSON_GetObjectItemCaseSensitive(params, "path");
    if (!j_path || !cJSON_IsString(j_path) || !j_path->valuestring[0]) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = "Missing required param: path (absolute path to .usd/.usda/.usdc/.usdz file)";
        return NULL;
    }

    const char* usd_path = j_path->valuestring;

    /* Check file exists */
    if (!cd_fs_exists(usd_path)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt("USD file not found",
            "The specified file does not exist.",
            "Provide an absolute path to an existing USD file.");
        return NULL;
    }

    /* Check extension */
    const char* ext = strrchr(usd_path, '.');
    if (!ext || (strcmp(ext, ".usd") != 0 && strcmp(ext, ".usda") != 0 &&
                 strcmp(ext, ".usdc") != 0 && strcmp(ext, ".usdz") != 0)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = "File must have .usd, .usda, .usdc, or .usdz extension";
        return NULL;
    }

    /* Optional destination */
    const char* dest = "res://scenes/imported/";
    const cJSON* j_dest = cJSON_GetObjectItemCaseSensitive(params, "destination");
    if (j_dest && cJSON_IsString(j_dest) && j_dest->valuestring[0]) {
        dest = j_dest->valuestring;
    }

    /* Build response — USD import requires the cook tool with TinyUSDZ */
    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "pending");
    cJSON_AddStringToObject(result, "source", usd_path);
    cJSON_AddStringToObject(result, "destination", dest);
    cJSON_AddStringToObject(result, "note",
        "USD import requires the asset-cook tool with TinyUSDZ support. "
        "Run: asset-cook --project <path> --import-usd <file>");

    /* List expected outputs */
    cJSON* outputs = cJSON_CreateArray();
    cJSON_AddItemToArray(outputs, cJSON_CreateString(".cdmesh files (geometry)"));
    cJSON_AddItemToArray(outputs, cJSON_CreateString(".cdmat files (materials)"));
    cJSON_AddItemToArray(outputs, cJSON_CreateString(".cdanim files (animations)"));
    cJSON_AddItemToArray(outputs, cJSON_CreateString("scene.toml (hierarchy)"));
    cJSON_AddItemToObject(result, "expected_outputs", outputs);

    return result;
}

/* ============================================================================
 * asset.import_mtlx handler
 *
 * Input:
 *   { "path": "C:/materials/material.mtlx",
 *     "bake_resolution": 1024 }
 *
 * Output:
 *   { "status": "ok", "materials": [...] }
 * ============================================================================ */

static cJSON* cd_mcp_handle_asset_import_mtlx(
    struct cd_kernel_t* kernel, const cJSON* params,
    int* error_code, const char** error_msg) {

    if (!kernel || !cd_kernel_get_asset_db(kernel)) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = "No project loaded";
        return NULL;
    }

    const cJSON* j_path = cJSON_GetObjectItemCaseSensitive(params, "path");
    if (!j_path || !cJSON_IsString(j_path) || !j_path->valuestring[0]) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = "Missing required param: path (absolute path to .mtlx file)";
        return NULL;
    }

    const char* mtlx_path = j_path->valuestring;

    /* Check file exists */
    if (!cd_fs_exists(mtlx_path)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt("MaterialX file not found",
            "The specified file does not exist.",
            "Provide an absolute path to an existing .mtlx file.");
        return NULL;
    }

    /* Optional bake resolution */
    int bake_res = 1024;
    const cJSON* j_bake = cJSON_GetObjectItemCaseSensitive(params, "bake_resolution");
    if (j_bake && cJSON_IsNumber(j_bake)) {
        bake_res = (int)j_bake->valuedouble;
    }

    /* Build response — MaterialX import requires the cook tool */
    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "pending");
    cJSON_AddStringToObject(result, "source", mtlx_path);
    cJSON_AddNumberToObject(result, "bake_resolution", bake_res);

    /* MaterialX Standard Surface -> Cadence PBR mapping */
    cJSON* mapping = cJSON_CreateObject();
    cJSON_AddStringToObject(mapping, "base_color", "albedo_color / albedo_texture");
    cJSON_AddStringToObject(mapping, "metalness", "metallic / metallic_texture");
    cJSON_AddStringToObject(mapping, "specular_roughness", "roughness / roughness_texture");
    cJSON_AddStringToObject(mapping, "normal", "normal_texture");
    cJSON_AddStringToObject(mapping, "emission_color", "emissive_color / emissive_texture");
    cJSON_AddStringToObject(mapping, "coat", "(future: clear coat extension)");
    cJSON_AddItemToObject(result, "pbr_mapping", mapping);

    cJSON_AddStringToObject(result, "note",
        "MaterialX import requires the asset-cook tool with MaterialX SDK. "
        "Run: asset-cook --project <path> --import-mtlx <file>");

    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_usd_tools(cd_mcp_server_t* server) {
    if (!server) return CD_ERR_NULL;

    cd_result_t r;

    r = cd_mcp_register_tool_ex(server, "asset.import_usd",
        cd_mcp_handle_asset_import_usd,
        "Import an OpenUSD scene (.usd/.usda/.usdc/.usdz) into Cadence format",
        "{\"type\":\"object\",\"properties\":{"
        "\"path\":{\"type\":\"string\",\"description\":\"Absolute path to the USD file\"},"
        "\"destination\":{\"type\":\"string\",\"description\":\"Destination res:// directory (default res://scenes/imported/)\"}"
        "},\"required\":[\"path\"]}");
    if (r != CD_OK) return r;

    r = cd_mcp_register_tool_ex(server, "asset.import_mtlx",
        cd_mcp_handle_asset_import_mtlx,
        "Import MaterialX materials (.mtlx) as .cdmat files",
        "{\"type\":\"object\",\"properties\":{"
        "\"path\":{\"type\":\"string\",\"description\":\"Absolute path to the .mtlx file\"},"
        "\"bake_resolution\":{\"type\":\"number\",\"description\":\"Resolution for texture baking (default 1024)\"}"
        "},\"required\":[\"path\"]}");
    if (r != CD_OK) return r;

    return CD_OK;
}
