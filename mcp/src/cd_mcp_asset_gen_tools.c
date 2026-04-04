/* cd_mcp_asset_gen_tools.c - Cadence Engine MCP procedural asset generation tools
 *
 * Implements:
 *   - asset_gen.listTemplates  : List available procedural mesh templates
 *   - asset_gen.generate       : Generate a single procedural mesh
 *   - asset_gen.batchGenerate  : Generate multiple procedural meshes
 *
 * All handlers follow the cd_mcp_tool_handler_t signature.
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cd_asset_gen.h"
#include "cd_primitives.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

/* ============================================================================
 * Template metadata
 * ============================================================================ */

typedef struct {
    const char* name;
    const char* category;
    const char* param_names[8];
    uint32_t    param_count;
} cd_template_info_t;

static const cd_template_info_t cd_templates[] = {
    { "platform", "structural",
      { "width", "depth", "height", "bevel" }, 4 },
    { "rock", "natural",
      { "radius", "subdivisions", "roughness", "seed" }, 4 },
    { "wall", "structural",
      { "width", "height", "thickness", "has_doorway", "door_width", "door_height" }, 6 },
    { "ramp", "structural",
      { "width", "height", "depth" }, 3 },
    { "stairs", "structural",
      { "width", "height", "depth", "step_count" }, 4 },
};

#define CD_TEMPLATE_COUNT (sizeof(cd_templates) / sizeof(cd_templates[0]))

/* ============================================================================
 * Helper: Generate mesh data from template name + params JSON
 *
 * Parses the template name and optional params object, calls the appropriate
 * generator, and returns CD_OK with filled cd_mesh_data_t on success.
 * ============================================================================ */

static cd_result_t cd_generate_from_spec(const char* template_name,
                                          const cJSON* dimensions,
                                          const cJSON* params_obj,
                                          cd_mesh_data_t* out_mesh,
                                          uint32_t* out_vertex_count,
                                          uint32_t* out_index_count) {
    if (!template_name || !out_mesh) return CD_ERR_NULL;

    /* Helper macro: read a float from dimensions or params, with default */
    #define READ_FLOAT(json, key, default_val) \
        ((json) ? (cJSON_GetObjectItemCaseSensitive((json), (key)) && \
         cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive((json), (key)))) ? \
         (float)cJSON_GetObjectItemCaseSensitive((json), (key))->valuedouble : \
         (default_val) : (default_val))

    #define READ_UINT(json, key, default_val) \
        ((json) ? (cJSON_GetObjectItemCaseSensitive((json), (key)) && \
         cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive((json), (key)))) ? \
         (uint32_t)cJSON_GetObjectItemCaseSensitive((json), (key))->valuedouble : \
         (default_val) : (default_val))

    #define READ_BOOL(json, key, default_val) \
        ((json) ? (cJSON_GetObjectItemCaseSensitive((json), (key)) && \
         cJSON_IsBool(cJSON_GetObjectItemCaseSensitive((json), (key)))) ? \
         (bool)cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive((json), (key))) : \
         (default_val) : (default_val))

    cd_result_t r;

    if (strcmp(template_name, "platform") == 0) {
        cd_asset_gen_platform_params_t p = cd_asset_gen_platform_defaults();
        /* dimensions override width/depth/height */
        if (dimensions) {
            p.width  = READ_FLOAT(dimensions, "x", p.width);
            p.height = READ_FLOAT(dimensions, "y", p.height);
            p.depth  = READ_FLOAT(dimensions, "z", p.depth);
        }
        /* params override individual fields */
        p.width  = READ_FLOAT(params_obj, "width",  p.width);
        p.depth  = READ_FLOAT(params_obj, "depth",  p.depth);
        p.height = READ_FLOAT(params_obj, "height", p.height);
        p.bevel  = READ_FLOAT(params_obj, "bevel",  p.bevel);
        /* Also check bevel_width (spec uses this name) */
        p.bevel  = READ_FLOAT(params_obj, "bevel_width", p.bevel);

        r = cd_asset_gen_platform(out_mesh, &p);

    } else if (strcmp(template_name, "rock") == 0) {
        cd_asset_gen_rock_params_t p = cd_asset_gen_rock_defaults();
        if (dimensions) {
            /* Use average of dimensions as radius */
            float x = READ_FLOAT(dimensions, "x", 0);
            float y = READ_FLOAT(dimensions, "y", 0);
            float z = READ_FLOAT(dimensions, "z", 0);
            if (x > 0 || y > 0 || z > 0) {
                float sum = 0; int cnt = 0;
                if (x > 0) { sum += x; cnt++; }
                if (y > 0) { sum += y; cnt++; }
                if (z > 0) { sum += z; cnt++; }
                p.radius = (sum / (float)cnt) * 0.5f;
            }
        }
        p.radius       = READ_FLOAT(params_obj, "radius",       p.radius);
        p.subdivisions = READ_UINT(params_obj,  "subdivisions", p.subdivisions);
        p.roughness    = READ_FLOAT(params_obj, "roughness",    p.roughness);
        p.seed         = READ_UINT(params_obj,  "seed",         p.seed);

        r = cd_asset_gen_rock(out_mesh, &p);

    } else if (strcmp(template_name, "wall") == 0) {
        cd_asset_gen_wall_params_t p = cd_asset_gen_wall_defaults();
        if (dimensions) {
            p.width     = READ_FLOAT(dimensions, "x", p.width);
            p.height    = READ_FLOAT(dimensions, "y", p.height);
            p.thickness = READ_FLOAT(dimensions, "z", p.thickness);
        }
        p.width       = READ_FLOAT(params_obj, "width",       p.width);
        p.height      = READ_FLOAT(params_obj, "height",      p.height);
        p.thickness   = READ_FLOAT(params_obj, "thickness",   p.thickness);
        p.has_doorway = READ_BOOL(params_obj,  "has_doorway", p.has_doorway);
        p.door_width  = READ_FLOAT(params_obj, "door_width",  p.door_width);
        p.door_height = READ_FLOAT(params_obj, "door_height", p.door_height);

        r = cd_asset_gen_wall(out_mesh, &p);

    } else if (strcmp(template_name, "ramp") == 0) {
        cd_asset_gen_ramp_params_t p = cd_asset_gen_ramp_defaults();
        if (dimensions) {
            p.width  = READ_FLOAT(dimensions, "x", p.width);
            p.height = READ_FLOAT(dimensions, "y", p.height);
            p.depth  = READ_FLOAT(dimensions, "z", p.depth);
        }
        p.width  = READ_FLOAT(params_obj, "width",  p.width);
        p.height = READ_FLOAT(params_obj, "height", p.height);
        p.depth  = READ_FLOAT(params_obj, "depth",  p.depth);

        r = cd_asset_gen_ramp(out_mesh, &p);

    } else if (strcmp(template_name, "stairs") == 0) {
        cd_asset_gen_stairs_params_t p = cd_asset_gen_stairs_defaults();
        if (dimensions) {
            p.width  = READ_FLOAT(dimensions, "x", p.width);
            p.height = READ_FLOAT(dimensions, "y", p.height);
            p.depth  = READ_FLOAT(dimensions, "z", p.depth);
        }
        p.width      = READ_FLOAT(params_obj, "width",      p.width);
        p.height     = READ_FLOAT(params_obj, "height",     p.height);
        p.depth      = READ_FLOAT(params_obj, "depth",      p.depth);
        p.step_count = READ_UINT(params_obj,  "step_count", p.step_count);

        r = cd_asset_gen_stairs(out_mesh, &p);

    } else {
        return CD_ERR_INVALID;
    }

    #undef READ_FLOAT
    #undef READ_UINT
    #undef READ_BOOL

    if (r == CD_OK) {
        *out_vertex_count = out_mesh->vertex_count;
        *out_index_count  = out_mesh->index_count;
    }
    return r;
}

/* ============================================================================
 * Helper: Build a JSON result object for a generated mesh
 * ============================================================================ */

static cJSON* cd_build_gen_result(const char* name,
                                   uint32_t vertex_count,
                                   uint32_t index_count) {
    cJSON* obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddStringToObject(obj, "status", "ok");
    if (name) {
        cJSON_AddStringToObject(obj, "name", name);
    }
    cJSON_AddNumberToObject(obj, "vertexCount", (double)vertex_count);
    cJSON_AddNumberToObject(obj, "indexCount",  (double)index_count);
    cJSON_AddNumberToObject(obj, "triangleCount", (double)(index_count / 3));

    return obj;
}

/* ============================================================================
 * asset_gen.listTemplates handler
 *
 * Input: (none required)
 *
 * Output:
 *   { "templates": [
 *       { "name": "platform", "category": "structural",
 *         "params": ["width", "depth", "height", "bevel"] },
 *       ...
 *     ] }
 * ============================================================================ */

static cJSON* cd_mcp_handle_asset_gen_list_templates(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel;
    (void)params;

    cJSON* templates_arr = cJSON_CreateArray();
    if (!templates_arr) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    for (uint32_t i = 0; i < CD_TEMPLATE_COUNT; i++) {
        const cd_template_info_t* info = &cd_templates[i];

        cJSON* tmpl = cJSON_CreateObject();
        if (!tmpl) continue;

        cJSON_AddStringToObject(tmpl, "name", info->name);
        cJSON_AddStringToObject(tmpl, "category", info->category);

        cJSON* params_arr = cJSON_CreateArray();
        if (params_arr) {
            for (uint32_t j = 0; j < info->param_count; j++) {
                cJSON_AddItemToArray(params_arr,
                    cJSON_CreateString(info->param_names[j]));
            }
            cJSON_AddItemToObject(tmpl, "params", params_arr);
        }

        cJSON_AddItemToArray(templates_arr, tmpl);
    }

    cJSON* result = cJSON_CreateObject();
    if (!result) {
        cJSON_Delete(templates_arr);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddItemToObject(result, "templates", templates_arr);
    return result;
}

/* ============================================================================
 * asset_gen.generate handler
 *
 * Input:
 *   { "spec": {
 *       "name": "stone_platform_4x1x4",   (optional, for identification)
 *       "template": "platform",            (required)
 *       "dimensions": { "x": 4, "y": 1, "z": 4 },  (optional)
 *       "params": { "bevel_width": 0.1 }   (optional, template-specific)
 *     }
 *   }
 *
 * Output:
 *   { "status": "ok",
 *     "name": "stone_platform_4x1x4",
 *     "vertexCount": 40,
 *     "indexCount": 60,
 *     "triangleCount": 20 }
 *
 * Error cases:
 *   -32602 INVALID_PARAMS: missing spec or template, unknown template
 *   -32603 INTERNAL_ERROR: generation failed
 * ============================================================================ */

static cJSON* cd_mcp_handle_asset_gen_generate(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel;

    if (!params) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing required parameter: spec";
        return NULL;
    }

    const cJSON* spec = cJSON_GetObjectItemCaseSensitive(params, "spec");
    if (!spec || !cJSON_IsObject(spec)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or invalid required parameter: spec";
        return NULL;
    }

    /* template (required) */
    const cJSON* tmpl_item = cJSON_GetObjectItemCaseSensitive(spec, "template");
    if (!tmpl_item || !cJSON_IsString(tmpl_item) ||
        !tmpl_item->valuestring || tmpl_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required field: spec.template";
        return NULL;
    }
    const char* template_name = tmpl_item->valuestring;

    /* name (optional) */
    const char* asset_name = NULL;
    const cJSON* name_item = cJSON_GetObjectItemCaseSensitive(spec, "name");
    if (name_item && cJSON_IsString(name_item) && name_item->valuestring) {
        asset_name = name_item->valuestring;
    }

    /* dimensions (optional) */
    const cJSON* dimensions = cJSON_GetObjectItemCaseSensitive(spec, "dimensions");

    /* params (optional) */
    const cJSON* params_obj = cJSON_GetObjectItemCaseSensitive(spec, "params");

    /* Generate */
    cd_mesh_data_t mesh;
    uint32_t vc = 0, ic = 0;
    cd_result_t r = cd_generate_from_spec(template_name, dimensions,
                                            params_obj, &mesh, &vc, &ic);
    if (r == CD_ERR_INVALID) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Unknown template name";
        return NULL;
    }
    if (r != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Mesh generation failed";
        return NULL;
    }

    /* Free mesh data — we only report counts, the actual mesh data would be
     * uploaded to GPU in a full pipeline. For the MCP tool, we generate to
     * validate and report metadata. */
    cd_mesh_data_free(&mesh);

    cJSON* result = cd_build_gen_result(asset_name, vc, ic);
    if (!result) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    return result;
}

/* ============================================================================
 * asset_gen.batchGenerate handler
 *
 * Input:
 *   { "specs": [
 *       { "name": "...", "template": "platform", "params": {...} },
 *       { "name": "...", "template": "rock", "params": {...} },
 *       ...
 *     ]
 *   }
 *
 * Output:
 *   { "results": [
 *       { "status": "ok", "name": "...", "vertexCount": N, ... },
 *       { "status": "error", "name": "...", "error": "Unknown template" },
 *       ...
 *     ],
 *     "totalGenerated": 2,
 *     "totalFailed": 0
 *   }
 *
 * Error cases:
 *   -32602 INVALID_PARAMS: missing or invalid specs array
 * ============================================================================ */

static cJSON* cd_mcp_handle_asset_gen_batch_generate(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel;

    if (!params) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing required parameter: specs";
        return NULL;
    }

    const cJSON* specs = cJSON_GetObjectItemCaseSensitive(params, "specs");
    if (!specs || !cJSON_IsArray(specs)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or invalid required parameter: specs (must be array)";
        return NULL;
    }

    cJSON* results_arr = cJSON_CreateArray();
    if (!results_arr) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    uint32_t total_ok = 0;
    uint32_t total_fail = 0;

    const cJSON* spec;
    cJSON_ArrayForEach(spec, specs) {
        if (!cJSON_IsObject(spec)) {
            cJSON* err_obj = cJSON_CreateObject();
            if (err_obj) {
                cJSON_AddStringToObject(err_obj, "status", "error");
                cJSON_AddStringToObject(err_obj, "error", "Spec entry is not an object");
                cJSON_AddItemToArray(results_arr, err_obj);
            }
            total_fail++;
            continue;
        }

        /* template (required per spec) */
        const cJSON* tmpl_item = cJSON_GetObjectItemCaseSensitive(spec, "template");
        if (!tmpl_item || !cJSON_IsString(tmpl_item) ||
            !tmpl_item->valuestring || tmpl_item->valuestring[0] == '\0') {
            cJSON* err_obj = cJSON_CreateObject();
            if (err_obj) {
                cJSON_AddStringToObject(err_obj, "status", "error");
                cJSON_AddStringToObject(err_obj, "error", "Missing template field");
                cJSON_AddItemToArray(results_arr, err_obj);
            }
            total_fail++;
            continue;
        }
        const char* template_name = tmpl_item->valuestring;

        /* name (optional) */
        const char* asset_name = NULL;
        const cJSON* name_item = cJSON_GetObjectItemCaseSensitive(spec, "name");
        if (name_item && cJSON_IsString(name_item) && name_item->valuestring) {
            asset_name = name_item->valuestring;
        }

        /* dimensions (optional) */
        const cJSON* dimensions = cJSON_GetObjectItemCaseSensitive(spec, "dimensions");

        /* params (optional) */
        const cJSON* params_obj = cJSON_GetObjectItemCaseSensitive(spec, "params");

        /* Generate */
        cd_mesh_data_t mesh;
        uint32_t vc = 0, ic = 0;
        cd_result_t r = cd_generate_from_spec(template_name, dimensions,
                                                params_obj, &mesh, &vc, &ic);
        if (r != CD_OK) {
            cJSON* err_obj = cJSON_CreateObject();
            if (err_obj) {
                cJSON_AddStringToObject(err_obj, "status", "error");
                if (asset_name) {
                    cJSON_AddStringToObject(err_obj, "name", asset_name);
                }
                if (r == CD_ERR_INVALID) {
                    cJSON_AddStringToObject(err_obj, "error", "Unknown template name");
                } else {
                    cJSON_AddStringToObject(err_obj, "error", "Mesh generation failed");
                }
                cJSON_AddItemToArray(results_arr, err_obj);
            }
            total_fail++;
            continue;
        }

        cd_mesh_data_free(&mesh);

        cJSON* ok_obj = cd_build_gen_result(asset_name, vc, ic);
        if (ok_obj) {
            cJSON_AddItemToArray(results_arr, ok_obj);
        }
        total_ok++;
    }

    /* Build final response */
    cJSON* result = cJSON_CreateObject();
    if (!result) {
        cJSON_Delete(results_arr);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddItemToObject(result, "results", results_arr);
    cJSON_AddNumberToObject(result, "totalGenerated", (double)total_ok);
    cJSON_AddNumberToObject(result, "totalFailed",    (double)total_fail);

    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_asset_gen_tools(cd_mcp_server_t* server) {
    if (!server) return CD_ERR_NULL;

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "asset_gen.listTemplates",
        cd_mcp_handle_asset_gen_list_templates,
        "List available procedural mesh templates and their parameters",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "asset_gen.generate",
        cd_mcp_handle_asset_gen_generate,
        "Generate a single procedural mesh from a template spec",
        "{\"type\":\"object\",\"properties\":{"
        "\"spec\":{\"type\":\"object\",\"description\":\"Mesh spec with template, dimensions, and params\","
        "\"properties\":{"
        "\"template\":{\"type\":\"string\",\"description\":\"Template name (platform, rock, wall, ramp, stairs)\"},"
        "\"name\":{\"type\":\"string\",\"description\":\"Optional name for the generated asset\"},"
        "\"dimensions\":{\"type\":\"object\",\"description\":\"Size overrides {x, y, z}\"},"
        "\"params\":{\"type\":\"object\",\"description\":\"Template-specific parameters\"}"
        "},\"required\":[\"template\"]}"
        "},\"required\":[\"spec\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "asset_gen.batchGenerate",
        cd_mcp_handle_asset_gen_batch_generate,
        "Generate multiple procedural meshes from an array of specs",
        "{\"type\":\"object\",\"properties\":{"
        "\"specs\":{\"type\":\"array\",\"items\":{\"type\":\"object\"},\"description\":\"Array of mesh specs (same format as asset_gen.generate)\"}"
        "},\"required\":[\"specs\"]}");
    if (res != CD_OK) return res;

    return CD_OK;
}
