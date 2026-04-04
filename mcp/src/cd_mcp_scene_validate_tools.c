/* cd_mcp_scene_validate_tools.c - Cadence Engine MCP scene validation tool
 *
 * Implements:
 *   - scene.validate : Validate a TOML scene file against the expected schema
 *
 * Checks:
 *   1. All referenced assets exist (mesh_asset URIs, script_uri paths)
 *   2. Node IDs are unique within the scene
 *   3. Parent references are valid (parent ID exists in the scene)
 *   4. Component types are registered in the type registry
 *   5. Warnings for: orphan nodes, missing transforms, unresolvable scripts
 *
 * The handler reads and parses the TOML file directly using the same
 * line-by-line parser approach as cd_scene_load.c, but does not construct
 * a scene graph -- it only validates the data.
 */

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_error.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_type_registry.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/stat.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define CD_VALIDATE_MAX_NODES       512
#define CD_VALIDATE_MAX_COMPONENTS  8
#define CD_VALIDATE_MAX_LINE        2048
#define CD_VALIDATE_MAX_ERRORS      64
#define CD_VALIDATE_MAX_WARNINGS    64
#define CD_VALIDATE_MAX_MSG         256

/* ============================================================================
 * Parsed node data for validation (lighter than full scene load)
 * ============================================================================ */

typedef struct {
    char type_name[64];
    /* Asset URI fields we care about for validation */
    char script_uri[256];
    char mesh_asset[256];
} cd_validate_component_t;

typedef struct {
    uint32_t                id;             /* saved integer ID */
    bool                    has_id;
    char                    name[64];
    uint32_t                parent_id;      /* saved parent integer ID */
    bool                    has_parent;
    bool                    has_transform;
    cd_validate_component_t components[CD_VALIDATE_MAX_COMPONENTS];
    uint32_t                component_count;
} cd_validate_node_t;

typedef struct {
    char                    scene_name[64];
    cd_validate_node_t      nodes[CD_VALIDATE_MAX_NODES];
    uint32_t                node_count;
} cd_validate_context_t;

/* ============================================================================
 * Validation result accumulator
 * ============================================================================ */

typedef struct {
    char errors[CD_VALIDATE_MAX_ERRORS][CD_VALIDATE_MAX_MSG];
    uint32_t error_count;
    char warnings[CD_VALIDATE_MAX_WARNINGS][CD_VALIDATE_MAX_MSG];
    uint32_t warning_count;
    uint32_t node_count;
    uint32_t component_count;
} cd_validate_result_t;

static void cd_validate_add_error(cd_validate_result_t* r, const char* fmt, ...) {
    if (r->error_count >= CD_VALIDATE_MAX_ERRORS) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(r->errors[r->error_count], CD_VALIDATE_MAX_MSG, fmt, args);
    va_end(args);
    r->error_count++;
}

static void cd_validate_add_warning(cd_validate_result_t* r, const char* fmt, ...) {
    if (r->warning_count >= CD_VALIDATE_MAX_WARNINGS) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(r->warnings[r->warning_count], CD_VALIDATE_MAX_MSG, fmt, args);
    va_end(args);
    r->warning_count++;
}

/* ============================================================================
 * String utility helpers (same patterns as cd_scene_load.c)
 * ============================================================================ */

static const char* cd_validate_skip_ws(const char* s) {
    while (*s != '\0' && isspace((unsigned char)*s)) s++;
    return s;
}

static void cd_validate_trim_trailing(char* s) {
    size_t len = strlen(s);
    while (len > 0 && (isspace((unsigned char)s[len - 1]) ||
           s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

/* Parse a TOML quoted string value into out. Returns pointer past closing quote. */
static const char* cd_validate_parse_string(const char* p, char* out, size_t out_size) {
    if (*p != '"') return NULL;
    p++;
    size_t i = 0;
    while (*p != '\0' && *p != '"') {
        if (*p == '\\' && *(p + 1) != '\0') {
            p++;
            char c;
            switch (*p) {
                case '\\': c = '\\'; break;
                case '"':  c = '"';  break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                default:   c = *p;   break;
            }
            if (i < out_size - 1) out[i++] = c;
            p++;
        } else {
            if (i < out_size - 1) out[i++] = *p;
            p++;
        }
    }
    out[i] = '\0';
    if (*p == '"') p++;
    else return NULL;
    return p;
}

/* Parse key = value from a line. Returns false if not a key=value line. */
static bool cd_validate_parse_kv(const char* line, char* key, size_t key_size,
                                  char* value, size_t value_size) {
    const char* p = cd_validate_skip_ws(line);
    if (*p == '\0' || *p == '#' || *p == '[') return false;

    size_t ki = 0;
    while (*p != '\0' && *p != '=' && !isspace((unsigned char)*p) && ki < key_size - 1) {
        key[ki++] = *p++;
    }
    key[ki] = '\0';

    p = cd_validate_skip_ws(p);
    if (*p != '=') return false;
    p++;
    p = cd_validate_skip_ws(p);

    size_t vi = 0;
    while (*p != '\0' && vi < value_size - 1) {
        value[vi++] = *p++;
    }
    value[vi] = '\0';
    cd_validate_trim_trailing(value);
    return true;
}

/* ============================================================================
 * TOML parsing state machine for validation
 * ============================================================================ */

typedef enum {
    VPARSE_NONE,
    VPARSE_SCENE,
    VPARSE_NODE,
    VPARSE_NODE_TRANSFORM,
    VPARSE_NODE_COMPONENT,
} cd_vparse_state_t;

static void cd_validate_parse_toml(const char* toml_str, size_t toml_len,
                                    cd_validate_context_t* ctx) {
    cd_vparse_state_t state = VPARSE_NONE;
    const char* pos = toml_str;
    const char* end = toml_str + toml_len;
    char line[CD_VALIDATE_MAX_LINE];

    /* Track current component type name for shorthand sections */
    char current_comp_type[64];
    current_comp_type[0] = '\0';

    while (pos < end) {
        const char* eol = pos;
        while (eol < end && *eol != '\n') eol++;

        size_t line_len = (size_t)(eol - pos);
        if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
        memcpy(line, pos, line_len);
        line[line_len] = '\0';
        cd_validate_trim_trailing(line);

        pos = (eol < end) ? eol + 1 : end;

        const char* trimmed = cd_validate_skip_ws(line);
        if (*trimmed == '\0' || *trimmed == '#') continue;

        /* Section headers */
        if (*trimmed == '[') {
            if (strncmp(trimmed, "[[node]]", 8) == 0) {
                if (ctx->node_count < CD_VALIDATE_MAX_NODES) {
                    memset(&ctx->nodes[ctx->node_count], 0, sizeof(cd_validate_node_t));
                    ctx->node_count++;
                }
                state = VPARSE_NODE;
                current_comp_type[0] = '\0';
            } else if (strncmp(trimmed, "[node.transform]", 16) == 0) {
                if (ctx->node_count > 0) {
                    ctx->nodes[ctx->node_count - 1].has_transform = true;
                }
                state = VPARSE_NODE_TRANSFORM;
            } else if (strncmp(trimmed, "[node.components.", 17) == 0) {
                /* Shorthand: [node.components.TypeName] */
                if (ctx->node_count > 0) {
                    cd_validate_node_t* node = &ctx->nodes[ctx->node_count - 1];
                    if (node->component_count < CD_VALIDATE_MAX_COMPONENTS) {
                        cd_validate_component_t* comp = &node->components[node->component_count];
                        memset(comp, 0, sizeof(*comp));
                        const char* type_start = trimmed + 17;
                        const char* type_end = strchr(type_start, ']');
                        if (type_end && (size_t)(type_end - type_start) < sizeof(comp->type_name)) {
                            memcpy(comp->type_name, type_start, (size_t)(type_end - type_start));
                            comp->type_name[type_end - type_start] = '\0';
                            memcpy(current_comp_type, comp->type_name, sizeof(current_comp_type));
                        }
                        node->component_count++;
                    }
                }
                state = VPARSE_NODE_COMPONENT;
            } else if (strncmp(trimmed, "[[node.component]]", 18) == 0) {
                /* Full form: [[node.component]] followed by type = "..." */
                if (ctx->node_count > 0) {
                    cd_validate_node_t* node = &ctx->nodes[ctx->node_count - 1];
                    if (node->component_count < CD_VALIDATE_MAX_COMPONENTS) {
                        memset(&node->components[node->component_count], 0,
                               sizeof(cd_validate_component_t));
                        node->component_count++;
                    }
                }
                state = VPARSE_NODE_COMPONENT;
                current_comp_type[0] = '\0';
            } else if (strncmp(trimmed, "[node.component.properties]", 27) == 0) {
                state = VPARSE_NODE_COMPONENT;
            } else if (strncmp(trimmed, "[scene]", 7) == 0) {
                state = VPARSE_SCENE;
            }
            continue;
        }

        /* Key = value parsing */
        char key[256], value[CD_VALIDATE_MAX_LINE];
        if (!cd_validate_parse_kv(trimmed, key, sizeof(key), value, sizeof(value))) {
            continue;
        }

        switch (state) {
            case VPARSE_SCENE: {
                if (strcmp(key, "name") == 0) {
                    cd_validate_parse_string(value, ctx->scene_name, sizeof(ctx->scene_name));
                }
                break;
            }
            case VPARSE_NODE: {
                if (ctx->node_count == 0) break;
                cd_validate_node_t* node = &ctx->nodes[ctx->node_count - 1];
                if (strcmp(key, "id") == 0) {
                    const char* v = cd_validate_skip_ws(value);
                    if (*v == '"') {
                        char id_str[128];
                        cd_validate_parse_string(v, id_str, sizeof(id_str));
                        /* For validation, we just need the index part */
                        unsigned idx = 0;
                        if (sscanf(id_str, "%u", &idx) == 1) {
                            node->id = idx;
                            node->has_id = true;
                        }
                    } else {
                        node->id = (uint32_t)strtoul(v, NULL, 10);
                        node->has_id = true;
                    }
                } else if (strcmp(key, "name") == 0) {
                    cd_validate_parse_string(value, node->name, sizeof(node->name));
                } else if (strcmp(key, "parent") == 0) {
                    const char* v = cd_validate_skip_ws(value);
                    if (*v == '"') {
                        char parent_str[128];
                        cd_validate_parse_string(v, parent_str, sizeof(parent_str));
                        unsigned idx = 0;
                        if (sscanf(parent_str, "%u", &idx) == 1) {
                            node->parent_id = idx;
                            node->has_parent = true;
                        }
                    } else if (*v != '\0' && *v != '#') {
                        node->parent_id = (uint32_t)strtoul(v, NULL, 10);
                        node->has_parent = true;
                    }
                }
                break;
            }
            case VPARSE_NODE_COMPONENT: {
                if (ctx->node_count == 0) break;
                cd_validate_node_t* node = &ctx->nodes[ctx->node_count - 1];
                if (node->component_count == 0) break;
                cd_validate_component_t* comp = &node->components[node->component_count - 1];

                if (strcmp(key, "type") == 0) {
                    cd_validate_parse_string(value, comp->type_name, sizeof(comp->type_name));
                } else if (strcmp(key, "script_uri") == 0) {
                    cd_validate_parse_string(value, comp->script_uri, sizeof(comp->script_uri));
                } else if (strcmp(key, "mesh_asset") == 0) {
                    cd_validate_parse_string(value, comp->mesh_asset, sizeof(comp->mesh_asset));
                }
                break;
            }
            default:
                break;
        }
    }
}

/* ============================================================================
 * Asset existence check
 *
 * Resolves a res:// URI to an absolute path relative to the project directory
 * and checks whether the file exists on disk.
 * ============================================================================ */

static bool cd_validate_file_exists(const char* project_dir, const char* res_uri) {
    if (res_uri == NULL || res_uri[0] == '\0') return false;

    char path[1024];

    if (strncmp(res_uri, "res://", 6) == 0) {
        /* Resolve res:// to project_dir + relative path */
        if (project_dir != NULL && project_dir[0] != '\0') {
            snprintf(path, sizeof(path), "%s/%s", project_dir, res_uri + 6);
        } else {
            snprintf(path, sizeof(path), "%s", res_uri + 6);
        }
    } else {
        /* Treat as relative path from project_dir */
        if (project_dir != NULL && project_dir[0] != '\0') {
            snprintf(path, sizeof(path), "%s/%s", project_dir, res_uri);
        } else {
            snprintf(path, sizeof(path), "%s", res_uri);
        }
    }

    struct stat st;
    return (stat(path, &st) == 0 && (st.st_mode & S_IFREG));
}

/* ============================================================================
 * Validation logic
 * ============================================================================ */

static void cd_validate_scene(const cd_validate_context_t* ctx,
                               const cd_type_registry_t* types,
                               const char* project_dir,
                               cd_validate_result_t* result) {
    result->node_count = ctx->node_count;

    /* Count total components */
    uint32_t total_components = 0;
    for (uint32_t i = 0; i < ctx->node_count; i++) {
        total_components += ctx->nodes[i].component_count;
    }
    result->component_count = total_components;

    /* Check 1: scene header must have a name */
    if (ctx->scene_name[0] == '\0') {
        cd_validate_add_warning(result, "Scene has no [scene] name field");
    }

    /* Check 2: Node ID uniqueness */
    for (uint32_t i = 0; i < ctx->node_count; i++) {
        const cd_validate_node_t* ni = &ctx->nodes[i];
        if (!ni->has_id) {
            cd_validate_add_error(result, "Node '%s' (index %u) has no id field",
                                  ni->name[0] ? ni->name : "<unnamed>", i);
            continue;
        }
        for (uint32_t j = i + 1; j < ctx->node_count; j++) {
            const cd_validate_node_t* nj = &ctx->nodes[j];
            if (nj->has_id && ni->id == nj->id) {
                cd_validate_add_error(result,
                    "Duplicate node id %u: '%s' and '%s'",
                    ni->id,
                    ni->name[0] ? ni->name : "<unnamed>",
                    nj->name[0] ? nj->name : "<unnamed>");
            }
        }
    }

    /* Check 3: Parent references are valid */
    for (uint32_t i = 0; i < ctx->node_count; i++) {
        const cd_validate_node_t* node = &ctx->nodes[i];
        if (!node->has_parent) continue;

        bool found = false;
        for (uint32_t j = 0; j < ctx->node_count; j++) {
            if (ctx->nodes[j].has_id && ctx->nodes[j].id == node->parent_id) {
                found = true;
                break;
            }
        }
        if (!found) {
            cd_validate_add_error(result,
                "Node '%s' (id %u) references parent id %u which does not exist",
                node->name[0] ? node->name : "<unnamed>",
                node->id, node->parent_id);
        }
    }

    /* Check 4: Component types are registered + Check asset existence */
    for (uint32_t i = 0; i < ctx->node_count; i++) {
        const cd_validate_node_t* node = &ctx->nodes[i];
        for (uint32_t c = 0; c < node->component_count; c++) {
            const cd_validate_component_t* comp = &node->components[c];

            /* Check component type registration */
            if (comp->type_name[0] != '\0' && types != NULL) {
                cd_id_t type_id = cd_type_find(
                    (cd_type_registry_t*)types, comp->type_name);
                if (!cd_id_is_valid(type_id)) {
                    cd_validate_add_warning(result,
                        "Node '%s': component type '%s' not registered in type registry",
                        node->name[0] ? node->name : "<unnamed>",
                        comp->type_name);
                }
            }

            /* Check script_uri exists */
            if (comp->script_uri[0] != '\0') {
                if (!cd_validate_file_exists(project_dir, comp->script_uri)) {
                    cd_validate_add_warning(result,
                        "Node '%s': script_uri '%s' not found on disk",
                        node->name[0] ? node->name : "<unnamed>",
                        comp->script_uri);
                }
            }

            /* Check mesh_asset exists */
            if (comp->mesh_asset[0] != '\0') {
                if (!cd_validate_file_exists(project_dir, comp->mesh_asset)) {
                    cd_validate_add_error(result,
                        "Node '%s': mesh_asset '%s' not found on disk",
                        node->name[0] ? node->name : "<unnamed>",
                        comp->mesh_asset);
                }
            }
        }
    }

    /* Check 5: Warnings for orphan nodes and missing transforms */
    /* Find the root node (one with no parent) */
    uint32_t root_id = 0;
    bool found_root = false;
    for (uint32_t i = 0; i < ctx->node_count; i++) {
        if (!ctx->nodes[i].has_parent) {
            root_id = ctx->nodes[i].id;
            found_root = true;
            break;
        }
    }

    for (uint32_t i = 0; i < ctx->node_count; i++) {
        const cd_validate_node_t* node = &ctx->nodes[i];

        /* Missing transform warning (skip root -- root often has no transform) */
        if (node->has_parent && !node->has_transform) {
            cd_validate_add_warning(result,
                "Node '%s' (id %u) has no [node.transform] section",
                node->name[0] ? node->name : "<unnamed>",
                node->id);
        }

        /* Orphan detection: nodes whose parent is root but aren't root themselves.
         * This is actually normal (direct children of root). Real orphans are nodes
         * whose parent doesn't resolve -- already caught by check 3.
         * Instead, warn about nodes that have neither parent nor are the first
         * root-like node. */
        if (!node->has_parent && found_root && node->id != root_id) {
            cd_validate_add_warning(result,
                "Node '%s' (id %u) has no parent (potential orphan root)",
                node->name[0] ? node->name : "<unnamed>",
                node->id);
        }
    }
}

/* ============================================================================
 * scene.validate handler
 *
 * Input:  { "path": "path/to/scene.toml" }
 * Output: { "valid": true/false, "errors": [...], "warnings": [...],
 *           "node_count": N, "component_count": N }
 * ============================================================================ */

static cJSON* cd_mcp_handle_scene_validate(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Kernel not available";
        return NULL;
    }

    /* Extract "path" parameter (required) */
    const cJSON* path_item = NULL;
    if (params != NULL) {
        path_item = cJSON_GetObjectItemCaseSensitive(params, "path");
    }
    if (path_item == NULL || !cJSON_IsString(path_item) ||
        path_item->valuestring == NULL || path_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: path";
        return NULL;
    }

    const char* scene_path = path_item->valuestring;

    /* Resolve the path relative to project directory */
    const char* project_dir = cd_kernel_get_config(kernel)->project_path;
    char resolved_path[1024];

    if (strncmp(scene_path, "res://", 6) == 0) {
        if (project_dir != NULL && project_dir[0] != '\0') {
            snprintf(resolved_path, sizeof(resolved_path), "%s/%s",
                     project_dir, scene_path + 6);
        } else {
            snprintf(resolved_path, sizeof(resolved_path), "%s", scene_path + 6);
        }
    } else {
        /* Treat as filesystem path */
        snprintf(resolved_path, sizeof(resolved_path), "%s", scene_path);
    }

    /* Read the file */
    FILE* fp = fopen(resolved_path, "rb");
    if (fp == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Scene file not found",
                          resolved_path,
                          "Provide a valid path to a .toml scene file");
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(fp);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Scene file is empty";
        return NULL;
    }

    char* buf = (char*)malloc((size_t)file_size + 1);
    if (buf == NULL) {
        fclose(fp);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Allocation failed";
        return NULL;
    }

    size_t bytes_read = fread(buf, 1, (size_t)file_size, fp);
    fclose(fp);
    buf[bytes_read] = '\0';

    /* Parse the TOML -- allocate on heap since context is large */
    cd_validate_context_t* ctx = (cd_validate_context_t*)calloc(1, sizeof(cd_validate_context_t));
    if (ctx == NULL) {
        free(buf);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Allocation failed";
        return NULL;
    }
    cd_validate_parse_toml(buf, bytes_read, ctx);
    free(buf);

    /* Run validation */
    cd_validate_result_t vresult;
    memset(&vresult, 0, sizeof(vresult));
    cd_validate_scene(ctx, cd_kernel_get_types(kernel), project_dir, &vresult);
    free(ctx);

    /* Build JSON response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddBoolToObject(result, "valid", vresult.error_count == 0);
    cJSON_AddNumberToObject(result, "node_count", (double)vresult.node_count);
    cJSON_AddNumberToObject(result, "component_count", (double)vresult.component_count);

    cJSON* errors_arr = cJSON_AddArrayToObject(result, "errors");
    for (uint32_t i = 0; i < vresult.error_count; i++) {
        cJSON_AddItemToArray(errors_arr, cJSON_CreateString(vresult.errors[i]));
    }

    cJSON* warnings_arr = cJSON_AddArrayToObject(result, "warnings");
    for (uint32_t i = 0; i < vresult.warning_count; i++) {
        cJSON_AddItemToArray(warnings_arr, cJSON_CreateString(vresult.warnings[i]));
    }

    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_scene_validate_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "scene.validate",
        cd_mcp_handle_scene_validate,
        "Validate a TOML scene file against the expected schema. "
        "Checks node ID uniqueness, parent references, component type registration, "
        "asset existence, and warns about missing transforms and orphan nodes.",
        "{\"type\":\"object\",\"properties\":{"
        "\"path\":{\"type\":\"string\",\"description\":"
        "\"Path to the scene TOML file. Can be a res:// URI or filesystem path.\"}"
        "},\"required\":[\"path\"]}");
    if (res != CD_OK) return res;

    return CD_OK;
}
