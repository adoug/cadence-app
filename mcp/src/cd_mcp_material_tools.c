/* cd_mcp_material_tools.c - Cadence Engine MCP material tool handlers
 *
 * Implements:
 *   - material.create : Create a new .cdmat file from template
 *   - material.get    : Read material properties by URI
 *   - material.set    : Modify material properties
 *   - material.list   : List all materials in project
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_error.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_asset_db.h"
#include "cadence/cd_material_asset.h"
#include "cadence/cd_platform.h"
#include "cadence/cd_memory.h"
#include "cd_gltf_import.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

/* ============================================================================
 * Helper: resolve res:// URI to absolute path
 * ============================================================================ */

static void cd_mat_resolve_uri(const char* project_root, const char* uri,
                                char* out_path, size_t out_size) {
    if (strncmp(uri, "res://", 6) == 0) {
        snprintf(out_path, out_size, "%s/%s", project_root, uri + 6);
    } else {
        snprintf(out_path, out_size, "%s/%s", project_root, uri);
    }
}

/* ============================================================================
 * Helper: add material fields to JSON object
 * ============================================================================ */

static void cd_mat_to_json(const cd_material_asset_t* mat, cJSON* obj) {
    cJSON_AddStringToObject(obj, "name", mat->name);

    /* Albedo */
    cJSON* albedo = cJSON_CreateObject();
    cJSON* ac = cJSON_CreateArray();
    for (int i = 0; i < 4; i++) cJSON_AddItemToArray(ac, cJSON_CreateNumber(mat->albedo_color[i]));
    cJSON_AddItemToObject(albedo, "color", ac);
    if (mat->albedo_texture[0]) cJSON_AddStringToObject(albedo, "texture", mat->albedo_texture);
    cJSON_AddItemToObject(obj, "albedo", albedo);

    /* Metallic */
    cJSON* metallic = cJSON_CreateObject();
    cJSON_AddNumberToObject(metallic, "value", mat->metallic);
    if (mat->metallic_texture[0]) cJSON_AddStringToObject(metallic, "texture", mat->metallic_texture);
    cJSON_AddItemToObject(obj, "metallic", metallic);

    /* Roughness */
    cJSON* roughness = cJSON_CreateObject();
    cJSON_AddNumberToObject(roughness, "value", mat->roughness);
    if (mat->roughness_texture[0]) cJSON_AddStringToObject(roughness, "texture", mat->roughness_texture);
    cJSON_AddItemToObject(obj, "roughness", roughness);

    /* Normal */
    if (mat->normal_texture[0]) {
        cJSON* normal = cJSON_CreateObject();
        cJSON_AddStringToObject(normal, "texture", mat->normal_texture);
        cJSON_AddItemToObject(obj, "normal", normal);
    }

    /* AO */
    cJSON* ao_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(ao_obj, "value", mat->ao);
    if (mat->ao_texture[0]) cJSON_AddStringToObject(ao_obj, "texture", mat->ao_texture);
    cJSON_AddItemToObject(obj, "ao", ao_obj);

    /* Emissive */
    cJSON* emissive = cJSON_CreateObject();
    cJSON* ec = cJSON_CreateArray();
    for (int i = 0; i < 4; i++) cJSON_AddItemToArray(ec, cJSON_CreateNumber(mat->emissive_color[i]));
    cJSON_AddItemToObject(emissive, "color", ec);
    cJSON_AddNumberToObject(emissive, "strength", mat->emissive_strength);
    if (mat->emissive_texture[0]) cJSON_AddStringToObject(emissive, "texture", mat->emissive_texture);
    cJSON_AddItemToObject(obj, "emissive", emissive);

    /* Height */
    if (mat->height_texture[0] || mat->height_scale != 0.05f) {
        cJSON* height = cJSON_CreateObject();
        cJSON_AddNumberToObject(height, "scale", mat->height_scale);
        if (mat->height_texture[0]) cJSON_AddStringToObject(height, "texture", mat->height_texture);
        cJSON_AddItemToObject(obj, "height", height);
    }

    /* Options */
    cJSON_AddBoolToObject(obj, "double_sided", mat->double_sided);
    cJSON_AddNumberToObject(obj, "alpha_cutoff", mat->alpha_cutoff);

    if (mat->shader_uri[0]) {
        cJSON_AddStringToObject(obj, "shader_uri", mat->shader_uri);
    }
}

/* ============================================================================
 * material.create handler
 *
 * Input:
 *   { "uri": "res://materials/brick.cdmat",
 *     "name": "brick_wall",
 *     "albedo_color": [0.8, 0.3, 0.2, 1.0],
 *     "roughness": 0.7 }
 *
 * Output: { "status": "ok", "uri": "res://materials/brick.cdmat" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_material_create(
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
        *error_msg = "Missing required param: uri (e.g. \"res://materials/brick.cdmat\")";
        return NULL;
    }

    cd_material_asset_t mat;
    cd_material_asset_defaults(&mat);

    /* Optional name */
    const cJSON* j_name = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (j_name && cJSON_IsString(j_name)) {
        snprintf(mat.name, sizeof(mat.name), "%s", j_name->valuestring);
    } else {
        /* Derive name from URI */
        const char* p = j_uri->valuestring;
        const char* last_slash = p;
        while (*p) { if (*p == '/') last_slash = p + 1; p++; }
        snprintf(mat.name, sizeof(mat.name), "%s", last_slash);
        char* dot = strrchr(mat.name, '.');
        if (dot) *dot = '\0';
    }

    /* Optional overrides */
    const cJSON* j_ac = cJSON_GetObjectItemCaseSensitive(params, "albedo_color");
    if (j_ac && cJSON_IsArray(j_ac)) {
        for (int i = 0; i < 4 && i < cJSON_GetArraySize(j_ac); i++) {
            cJSON* item = cJSON_GetArrayItem(j_ac, i);
            if (item) mat.albedo_color[i] = (float)item->valuedouble;
        }
    }

    const cJSON* j_r = cJSON_GetObjectItemCaseSensitive(params, "roughness");
    if (j_r && cJSON_IsNumber(j_r)) mat.roughness = (float)j_r->valuedouble;

    const cJSON* j_m = cJSON_GetObjectItemCaseSensitive(params, "metallic");
    if (j_m && cJSON_IsNumber(j_m)) mat.metallic = (float)j_m->valuedouble;

    const cJSON* j_ds = cJSON_GetObjectItemCaseSensitive(params, "double_sided");
    if (j_ds && cJSON_IsBool(j_ds)) mat.double_sided = cJSON_IsTrue(j_ds);

    const cJSON* j_at = cJSON_GetObjectItemCaseSensitive(params, "albedo_texture");
    if (j_at && cJSON_IsString(j_at)) {
        snprintf(mat.albedo_texture, sizeof(mat.albedo_texture), "%s", j_at->valuestring);
    }

    const cJSON* j_nt = cJSON_GetObjectItemCaseSensitive(params, "normal_texture");
    if (j_nt && cJSON_IsString(j_nt)) {
        snprintf(mat.normal_texture, sizeof(mat.normal_texture), "%s", j_nt->valuestring);
    }

    /* Resolve URI to absolute path */
    char abs_path[512];
    cd_mat_resolve_uri(cd_kernel_get_asset_db(kernel)->project_root,
                       j_uri->valuestring, abs_path, sizeof(abs_path));

    /* Ensure parent directory exists */
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s", abs_path);
    char* last_sep = strrchr(dir_path, '/');
    if (!last_sep) last_sep = strrchr(dir_path, '\\');
    if (last_sep) {
        *last_sep = '\0';
        /* Simple mkdir — parent should exist for project assets */
        if (!cd_fs_exists(dir_path)) {
            cd_fs_mkdir(dir_path);
        }
    }

    /* Save the .cdmat file */
    cd_result_t res = cd_material_asset_save(&mat, abs_path);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = "Failed to write .cdmat file";
        return NULL;
    }

    /* Register in asset database */
    cd_asset_db_register(cd_kernel_get_asset_db(kernel), j_uri->valuestring, abs_path,
                         CD_ASSET_MATERIAL);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "uri", j_uri->valuestring);
    cJSON_AddStringToObject(result, "name", mat.name);
    return result;
}

/* ============================================================================
 * P2-7: Register texture dependencies for a material asset.
 *
 * After loading a .cdmat, each texture URI referenced by the material is
 * looked up in the asset_db and registered as a dependency edge
 * (material -> texture).  This enables hot-reload propagation and
 * incremental cooking.
 * ============================================================================ */

static void cd_mat_register_deps(cd_asset_db_t* db,
                                  uint32_t mat_index,
                                  const cd_material_asset_t* mat) {
    const char* tex_uris[] = {
        mat->albedo_texture,
        mat->metallic_texture,
        mat->roughness_texture,
        mat->normal_texture,
        mat->ao_texture,
        mat->emissive_texture,
        mat->height_texture,
    };
    for (uint32_t i = 0; i < sizeof(tex_uris) / sizeof(tex_uris[0]); i++) {
        if (!tex_uris[i] || tex_uris[i][0] == '\0') continue;
        const cd_asset_entry_t* tex_entry = cd_asset_db_lookup(db, tex_uris[i]);
        if (tex_entry) {
            uint32_t tex_index = (uint32_t)(tex_entry - db->entries);
            cd_asset_db_add_dependency(db, mat_index, tex_index);
        }
    }
}

/* ============================================================================
 * material.get handler
 *
 * Input: { "uri": "res://materials/brick.cdmat" }
 * Output: { material fields as JSON }
 * ============================================================================ */

static cJSON* cd_mcp_handle_material_get(
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

    /* Resolve URI */
    char abs_path[512];
    const cd_asset_entry_t* entry = cd_asset_db_lookup(cd_kernel_get_asset_db(kernel),
                                                        j_uri->valuestring);
    if (entry) {
        snprintf(abs_path, sizeof(abs_path), "%s", entry->abs_path);
    } else {
        cd_mat_resolve_uri(cd_kernel_get_asset_db(kernel)->project_root,
                           j_uri->valuestring, abs_path, sizeof(abs_path));
    }

    /* Load the material */
    cd_material_asset_t mat;
    cd_result_t res = cd_material_asset_load(&mat, abs_path);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt("Material not found",
            "Could not load material file",
            "Check the URI path and ensure the .cdmat file exists.");
        return NULL;
    }

    /* P2-7: Register texture dependencies for hot-reload propagation */
    if (entry) {
        cd_asset_db_t* db = cd_kernel_get_asset_db(kernel);
        uint32_t mat_index = (uint32_t)(entry - db->entries);
        cd_mat_register_deps(db, mat_index, &mat);
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "uri", j_uri->valuestring);
    cd_mat_to_json(&mat, result);
    return result;
}

/* ============================================================================
 * material.set handler
 *
 * Input: { "uri": "res://materials/brick.cdmat", "roughness": 0.9 }
 * Output: { "status": "ok", "uri": "..." }
 * ============================================================================ */

static cJSON* cd_mcp_handle_material_set(
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

    /* Resolve and load existing */
    char abs_path[512];
    const cd_asset_entry_t* entry = cd_asset_db_lookup(cd_kernel_get_asset_db(kernel),
                                                        j_uri->valuestring);
    if (entry) {
        snprintf(abs_path, sizeof(abs_path), "%s", entry->abs_path);
    } else {
        cd_mat_resolve_uri(cd_kernel_get_asset_db(kernel)->project_root,
                           j_uri->valuestring, abs_path, sizeof(abs_path));
    }

    cd_material_asset_t mat;
    cd_result_t res = cd_material_asset_load(&mat, abs_path);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = "Material file not found";
        return NULL;
    }

    /* Apply overrides from params */
    const cJSON* j;

    j = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (j && cJSON_IsString(j)) snprintf(mat.name, sizeof(mat.name), "%s", j->valuestring);

    j = cJSON_GetObjectItemCaseSensitive(params, "albedo_color");
    if (j && cJSON_IsArray(j)) {
        for (int i = 0; i < 4 && i < cJSON_GetArraySize(j); i++) {
            cJSON* item = cJSON_GetArrayItem(j, i);
            if (item) mat.albedo_color[i] = (float)item->valuedouble;
        }
    }

    j = cJSON_GetObjectItemCaseSensitive(params, "albedo_texture");
    if (j && cJSON_IsString(j)) snprintf(mat.albedo_texture, sizeof(mat.albedo_texture), "%s", j->valuestring);

    j = cJSON_GetObjectItemCaseSensitive(params, "metallic");
    if (j && cJSON_IsNumber(j)) mat.metallic = (float)j->valuedouble;

    j = cJSON_GetObjectItemCaseSensitive(params, "metallic_texture");
    if (j && cJSON_IsString(j)) snprintf(mat.metallic_texture, sizeof(mat.metallic_texture), "%s", j->valuestring);

    j = cJSON_GetObjectItemCaseSensitive(params, "roughness");
    if (j && cJSON_IsNumber(j)) mat.roughness = (float)j->valuedouble;

    j = cJSON_GetObjectItemCaseSensitive(params, "roughness_texture");
    if (j && cJSON_IsString(j)) snprintf(mat.roughness_texture, sizeof(mat.roughness_texture), "%s", j->valuestring);

    j = cJSON_GetObjectItemCaseSensitive(params, "normal_texture");
    if (j && cJSON_IsString(j)) snprintf(mat.normal_texture, sizeof(mat.normal_texture), "%s", j->valuestring);

    j = cJSON_GetObjectItemCaseSensitive(params, "ao");
    if (j && cJSON_IsNumber(j)) mat.ao = (float)j->valuedouble;

    j = cJSON_GetObjectItemCaseSensitive(params, "ao_texture");
    if (j && cJSON_IsString(j)) snprintf(mat.ao_texture, sizeof(mat.ao_texture), "%s", j->valuestring);

    j = cJSON_GetObjectItemCaseSensitive(params, "emissive_color");
    if (j && cJSON_IsArray(j)) {
        for (int i = 0; i < 4 && i < cJSON_GetArraySize(j); i++) {
            cJSON* item = cJSON_GetArrayItem(j, i);
            if (item) mat.emissive_color[i] = (float)item->valuedouble;
        }
    }

    j = cJSON_GetObjectItemCaseSensitive(params, "emissive_strength");
    if (j && cJSON_IsNumber(j)) mat.emissive_strength = (float)j->valuedouble;

    j = cJSON_GetObjectItemCaseSensitive(params, "emissive_texture");
    if (j && cJSON_IsString(j)) snprintf(mat.emissive_texture, sizeof(mat.emissive_texture), "%s", j->valuestring);

    j = cJSON_GetObjectItemCaseSensitive(params, "double_sided");
    if (j && cJSON_IsBool(j)) mat.double_sided = cJSON_IsTrue(j);

    j = cJSON_GetObjectItemCaseSensitive(params, "alpha_cutoff");
    if (j && cJSON_IsNumber(j)) mat.alpha_cutoff = (float)j->valuedouble;

    j = cJSON_GetObjectItemCaseSensitive(params, "height_scale");
    if (j && cJSON_IsNumber(j)) mat.height_scale = (float)j->valuedouble;

    j = cJSON_GetObjectItemCaseSensitive(params, "height_texture");
    if (j && cJSON_IsString(j)) snprintf(mat.height_texture, sizeof(mat.height_texture), "%s", j->valuestring);

    j = cJSON_GetObjectItemCaseSensitive(params, "shader_uri");
    if (j && cJSON_IsString(j)) snprintf(mat.shader_uri, sizeof(mat.shader_uri), "%s", j->valuestring);

    /* Save back */
    res = cd_material_asset_save(&mat, abs_path);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = "Failed to save material file";
        return NULL;
    }

    /* P2-7: Re-register texture dependencies (textures may have changed) */
    if (entry) {
        cd_asset_db_t* db = cd_kernel_get_asset_db(kernel);
        uint32_t mat_index = (uint32_t)(entry - db->entries);
        cd_mat_register_deps(db, mat_index, &mat);
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "uri", j_uri->valuestring);
    return result;
}

/* ============================================================================
 * material.list handler
 *
 * Input: { "prefix": "res://materials/" }  (optional)
 * Output: { "materials": [ { "uri": "...", "name": "..." }, ... ] }
 * ============================================================================ */

static cJSON* cd_mcp_handle_material_list(
    struct cd_kernel_t* kernel, const cJSON* params,
    int* error_code, const char** error_msg) {

    if (!kernel || !cd_kernel_get_asset_db(kernel)) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = "No project loaded";
        return NULL;
    }

    const char* prefix = NULL;
    if (params) {
        const cJSON* j_pfx = cJSON_GetObjectItemCaseSensitive(params, "prefix");
        if (j_pfx && cJSON_IsString(j_pfx) && j_pfx->valuestring[0]) {
            prefix = j_pfx->valuestring;
        }
    }

    const cd_asset_db_t* db = cd_kernel_get_asset_db(kernel);
    cJSON* arr = cJSON_CreateArray();

    for (uint32_t i = 0; i < db->count; i++) {
        const cd_asset_entry_t* entry = &db->entries[i];
        if (entry->kind != CD_ASSET_MATERIAL) continue;

        if (prefix && strncmp(entry->uri, prefix, strlen(prefix)) != 0) continue;

        /* Try to load the material to get its name */
        cd_material_asset_t mat;
        cd_result_t res = cd_material_asset_load(&mat, entry->abs_path);

        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "uri", entry->uri);
        cJSON_AddStringToObject(obj, "name",
                                res == CD_OK ? mat.name : "(unknown)");
        cJSON_AddItemToArray(arr, obj);
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "materials", arr);
    return result;
}

/* ============================================================================
 * material.extract_gltf handler
 *
 * Input:
 *   { "path": "/absolute/path/to/model.glb",
 *     "output_dir": "res://materials/" }
 *
 * Output:
 *   { "status": "ok", "materials": [ { "name": "...", "uri": "..." } ] }
 * ============================================================================ */

static cJSON* cd_mcp_handle_material_extract_gltf(
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
        *error_msg = "Missing required param: path (absolute path to .glb/.gltf)";
        return NULL;
    }

    const char* gltf_path = j_path->valuestring;
    if (!cd_fs_exists(gltf_path)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg = cd_mcp_error_fmt("glTF file not found",
            "The specified file does not exist.",
            "Provide an absolute path to a .glb or .gltf file.");
        return NULL;
    }

    /* Output directory */
    const char* out_dir_uri = "res://materials/";
    const cJSON* j_out = cJSON_GetObjectItemCaseSensitive(params, "output_dir");
    if (j_out && cJSON_IsString(j_out) && j_out->valuestring[0]) {
        out_dir_uri = j_out->valuestring;
    }

    /* Resolve output dir to absolute path */
    char out_dir_abs[512];
    cd_mat_resolve_uri(cd_kernel_get_asset_db(kernel)->project_root, out_dir_uri,
                       out_dir_abs, sizeof(out_dir_abs));

    /* Create output directory */
    if (!cd_fs_exists(out_dir_abs)) {
        cd_fs_mkdir(out_dir_abs);
    }

    /* Import glTF */
    cd_gltf_scene_t scene;
    memset(&scene, 0, sizeof(scene));
    cd_result_t res = cd_gltf_import_file(&scene, gltf_path);
    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg = cd_mcp_error_fmt("glTF import failed",
            "Could not parse the glTF file.",
            "Ensure the file is valid glTF 2.0.");
        return NULL;
    }

    /* Convert each glTF material to .cdmat */
    cJSON* mat_arr = cJSON_CreateArray();
    uint32_t exported = 0;

    for (uint32_t i = 0; i < scene.material_count; i++) {
        const cd_gltf_material_t* gmat = &scene.materials[i];

        cd_material_asset_t mat;
        cd_material_asset_defaults(&mat);

        /* Name */
        if (gmat->name[0]) {
            snprintf(mat.name, sizeof(mat.name), "%s", gmat->name);
        } else {
            snprintf(mat.name, sizeof(mat.name), "material_%u", i);
        }

        /* PBR values from cd_material_t */
        mat.albedo_color[0] = gmat->material.albedo_color[0];
        mat.albedo_color[1] = gmat->material.albedo_color[1];
        mat.albedo_color[2] = gmat->material.albedo_color[2];
        mat.albedo_color[3] = gmat->material.albedo_color[3];
        mat.metallic = gmat->material.metallic;
        mat.roughness = gmat->material.roughness;
        mat.emissive_color[0] = gmat->material.emissive_color[0];
        mat.emissive_color[1] = gmat->material.emissive_color[1];
        mat.emissive_color[2] = gmat->material.emissive_color[2];
        mat.emissive_color[3] = gmat->material.emissive_color[3];
        mat.emissive_strength = gmat->material.emissive_strength;
        mat.double_sided = gmat->material.double_sided;
        mat.alpha_cutoff = gmat->material.alpha_cutoff;

        /* Texture paths */
        if (gmat->albedo_texture_path[0]) {
            snprintf(mat.albedo_texture, sizeof(mat.albedo_texture),
                     "%s", gmat->albedo_texture_path);
        }
        if (gmat->normal_texture_path[0]) {
            snprintf(mat.normal_texture, sizeof(mat.normal_texture),
                     "%s", gmat->normal_texture_path);
        }

        /* Build filename: sanitize name for filesystem */
        char filename[128];
        snprintf(filename, sizeof(filename), "%s.cdmat", mat.name);
        for (size_t c = 0; filename[c]; c++) {
            if (filename[c] == ' ' || filename[c] == '/' ||
                filename[c] == '\\' || filename[c] == ':') {
                filename[c] = '_';
            }
        }

        /* Build output path */
        char out_path[512];
        size_t dir_len = strlen(out_dir_abs);
        if (dir_len > 0 && out_dir_abs[dir_len-1] == '/') {
            snprintf(out_path, sizeof(out_path), "%s%s", out_dir_abs, filename);
        } else {
            snprintf(out_path, sizeof(out_path), "%s/%s", out_dir_abs, filename);
        }

        /* Save */
        res = cd_material_asset_save(&mat, out_path);
        if (res == CD_OK) {
            /* Build URI */
            char mat_uri[256];
            size_t uri_dir_len = strlen(out_dir_uri);
            if (uri_dir_len > 0 && out_dir_uri[uri_dir_len-1] == '/') {
                snprintf(mat_uri, sizeof(mat_uri), "%s%s", out_dir_uri, filename);
            } else {
                snprintf(mat_uri, sizeof(mat_uri), "%s/%s", out_dir_uri, filename);
            }

            /* Register in asset DB */
            cd_asset_db_register(cd_kernel_get_asset_db(kernel), mat_uri, out_path,
                                 CD_ASSET_MATERIAL);

            cJSON* obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "name", mat.name);
            cJSON_AddStringToObject(obj, "uri", mat_uri);
            cJSON_AddItemToArray(mat_arr, obj);
            exported++;
        }
    }

    cd_gltf_scene_free(&scene);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddNumberToObject(result, "exported_count", exported);
    cJSON_AddItemToObject(result, "materials", mat_arr);
    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_material_tools(cd_mcp_server_t* server) {
    if (!server) return CD_ERR_NULL;

    cd_result_t r;

    r = cd_mcp_register_tool_ex(server, "material.create",
        cd_mcp_handle_material_create,
        "Create a new .cdmat material file",
        "{\"type\":\"object\",\"properties\":{"
        "\"uri\":{\"type\":\"string\",\"description\":\"Destination res:// URI for the .cdmat file\"},"
        "\"name\":{\"type\":\"string\",\"description\":\"Material display name\"},"
        "\"albedo_color\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"description\":\"RGBA base color [r,g,b,a]\"},"
        "\"roughness\":{\"type\":\"number\",\"description\":\"PBR roughness 0-1\"},"
        "\"metallic\":{\"type\":\"number\",\"description\":\"PBR metallic 0-1\"},"
        "\"double_sided\":{\"type\":\"boolean\",\"description\":\"Render both sides\"},"
        "\"albedo_texture\":{\"type\":\"string\",\"description\":\"Albedo texture URI\"},"
        "\"normal_texture\":{\"type\":\"string\",\"description\":\"Normal map URI\"}"
        "},\"required\":[\"uri\"]}");
    if (r != CD_OK) return r;

    r = cd_mcp_register_tool_ex(server, "material.get",
        cd_mcp_handle_material_get,
        "Read material properties from a .cdmat file",
        "{\"type\":\"object\",\"properties\":{"
        "\"uri\":{\"type\":\"string\",\"description\":\"Material res:// URI\"}"
        "},\"required\":[\"uri\"]}");
    if (r != CD_OK) return r;

    r = cd_mcp_register_tool_ex(server, "material.set",
        cd_mcp_handle_material_set,
        "Modify material properties in a .cdmat file",
        "{\"type\":\"object\",\"properties\":{"
        "\"uri\":{\"type\":\"string\",\"description\":\"Material res:// URI\"},"
        "\"name\":{\"type\":\"string\"},\"roughness\":{\"type\":\"number\"},"
        "\"metallic\":{\"type\":\"number\"},\"double_sided\":{\"type\":\"boolean\"},"
        "\"albedo_color\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},"
        "\"albedo_texture\":{\"type\":\"string\"},\"normal_texture\":{\"type\":\"string\"},"
        "\"metallic_texture\":{\"type\":\"string\"},\"roughness_texture\":{\"type\":\"string\"},"
        "\"ao\":{\"type\":\"number\"},\"ao_texture\":{\"type\":\"string\"},"
        "\"emissive_color\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},"
        "\"emissive_strength\":{\"type\":\"number\"},\"emissive_texture\":{\"type\":\"string\"},"
        "\"alpha_cutoff\":{\"type\":\"number\"},\"height_scale\":{\"type\":\"number\"},"
        "\"height_texture\":{\"type\":\"string\"},\"shader_uri\":{\"type\":\"string\"}"
        "},\"required\":[\"uri\"]}");
    if (r != CD_OK) return r;

    r = cd_mcp_register_tool_ex(server, "material.extract_gltf",
        cd_mcp_handle_material_extract_gltf,
        "Extract materials from a glTF/GLB file into .cdmat files",
        "{\"type\":\"object\",\"properties\":{"
        "\"path\":{\"type\":\"string\",\"description\":\"Absolute path to the .glb/.gltf file\"},"
        "\"output_dir\":{\"type\":\"string\",\"description\":\"Output directory as res:// URI (default res://materials/)\"}"
        "},\"required\":[\"path\"]}");
    if (r != CD_OK) return r;

    r = cd_mcp_register_tool_ex(server, "material.list",
        cd_mcp_handle_material_list,
        "List all .cdmat material assets in the project",
        "{\"type\":\"object\",\"properties\":{"
        "\"prefix\":{\"type\":\"string\",\"description\":\"Filter by URI prefix\"}"
        "}}");
    if (r != CD_OK) return r;

    return CD_OK;
}
