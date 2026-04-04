/* cd_mcp_prop_tools.c - Cadence Engine MCP property tool handlers
 *
 * Implements:
 *   - prop.set      : Set a component property by dot-path
 *   - prop.get      : Get a component property by dot-path
 *   - prop.batchSet : Set multiple properties atomically
 *
 * Path format: "ComponentType.field" or "ComponentType.field.subfield"
 *
 * Special case: "Transform" is resolved against node->local_transform
 * rather than a component entry, since Transform is a built-in.
 *
 * For general component types, the type registry is used to resolve
 * field paths to memory offsets.
 *
 * All handlers follow the cd_mcp_tool_handler_t signature.
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_error.h"
#include "cadence/cd_mcp_fuzzy_match.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_scene.h"
#include "cadence/cd_type_registry.h"
#include "cadence/cd_builtin_types.h"
#include "cadence/cd_commands.h"
#include "cadence/cd_error.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/* ============================================================================
 * Internal: Format a cd_id_t as "index:generation" string
 * ============================================================================ */

#define CD_ID_STR_BUF_SIZE 24

static cd_id_t cd_id_parse(const char* str) {
    if (str == NULL || str[0] == '\0') {
        return CD_ID_INVALID;
    }
    unsigned int index = 0;
    unsigned int gen   = 0;
    int scanned = sscanf(str, "%u:%u", &index, &gen);
    if (scanned != 2) {
        return CD_ID_INVALID;
    }
    return cd_id_make((uint32_t)gen, (uint32_t)index);
}

/* ============================================================================
 * Internal: Map cd_field_kind_t to a type name string for prop.get response
 * ============================================================================ */

static const char* field_kind_to_type_name(cd_field_kind_t kind) {
    switch (kind) {
    case CD_FIELD_INT32:    return "int32";
    case CD_FIELD_UINT32:   return "uint32";
    case CD_FIELD_INT64:    return "int64";
    case CD_FIELD_FLOAT:    return "float";
    case CD_FIELD_DOUBLE:   return "double";
    case CD_FIELD_BOOL:     return "bool";
    case CD_FIELD_STRING:   return "string";
    case CD_FIELD_VEC3:     return "vec3";
    case CD_FIELD_VEC4:     return "vec4";
    case CD_FIELD_QUAT:     return "quat";
    case CD_FIELD_MAT4:     return "mat4";
    case CD_FIELD_COLOR:    return "color";
    case CD_FIELD_ID:       return "id";
    case CD_FIELD_ASSET_URI: return "asset_uri";
    case CD_FIELD_ENUM:     return "enum";
    case CD_FIELD_STRUCT:   return "struct";
    case CD_FIELD_ARRAY:    return "array";
    }
    return "unknown";
}

/* ============================================================================
 * Internal: Read a field value from memory and convert to cJSON
 * ============================================================================ */

static cJSON* field_to_json(const void* ptr, cd_field_kind_t kind) {
    switch (kind) {
    case CD_FIELD_INT32:
    case CD_FIELD_ENUM: {
        int32_t val;
        memcpy(&val, ptr, sizeof(val));
        return cJSON_CreateNumber((double)val);
    }
    case CD_FIELD_UINT32: {
        uint32_t val;
        memcpy(&val, ptr, sizeof(val));
        return cJSON_CreateNumber((double)val);
    }
    case CD_FIELD_INT64: {
        int64_t val;
        memcpy(&val, ptr, sizeof(val));
        return cJSON_CreateNumber((double)val);
    }
    case CD_FIELD_FLOAT: {
        float val;
        memcpy(&val, ptr, sizeof(val));
        return cJSON_CreateNumber((double)val);
    }
    case CD_FIELD_DOUBLE: {
        double val;
        memcpy(&val, ptr, sizeof(val));
        return cJSON_CreateNumber(val);
    }
    case CD_FIELD_BOOL: {
        bool val;
        memcpy(&val, ptr, sizeof(val));
        return cJSON_CreateBool(val);
    }
    case CD_FIELD_STRING:
    case CD_FIELD_ASSET_URI: {
        const cd_name_t* name = (const cd_name_t*)ptr;
        return cJSON_CreateString(name->buf);
    }
    case CD_FIELD_VEC3: {
        float xyz[3];
        memcpy(xyz, ptr, sizeof(float) * 3);
        cJSON* obj = cJSON_CreateObject();
        if (obj == NULL) return NULL;
        cJSON_AddNumberToObject(obj, "x", (double)xyz[0]);
        cJSON_AddNumberToObject(obj, "y", (double)xyz[1]);
        cJSON_AddNumberToObject(obj, "z", (double)xyz[2]);
        return obj;
    }
    case CD_FIELD_VEC4:
    case CD_FIELD_COLOR: {
        float xyzw[4];
        memcpy(xyzw, ptr, sizeof(float) * 4);
        cJSON* obj = cJSON_CreateObject();
        if (obj == NULL) return NULL;
        if (kind == CD_FIELD_COLOR) {
            cJSON_AddNumberToObject(obj, "r", (double)xyzw[0]);
            cJSON_AddNumberToObject(obj, "g", (double)xyzw[1]);
            cJSON_AddNumberToObject(obj, "b", (double)xyzw[2]);
            cJSON_AddNumberToObject(obj, "a", (double)xyzw[3]);
        } else {
            cJSON_AddNumberToObject(obj, "x", (double)xyzw[0]);
            cJSON_AddNumberToObject(obj, "y", (double)xyzw[1]);
            cJSON_AddNumberToObject(obj, "z", (double)xyzw[2]);
            cJSON_AddNumberToObject(obj, "w", (double)xyzw[3]);
        }
        return obj;
    }
    case CD_FIELD_QUAT: {
        float xyzw[4];
        memcpy(xyzw, ptr, sizeof(float) * 4);
        cJSON* obj = cJSON_CreateObject();
        if (obj == NULL) return NULL;
        cJSON_AddNumberToObject(obj, "x", (double)xyzw[0]);
        cJSON_AddNumberToObject(obj, "y", (double)xyzw[1]);
        cJSON_AddNumberToObject(obj, "z", (double)xyzw[2]);
        cJSON_AddNumberToObject(obj, "w", (double)xyzw[3]);
        return obj;
    }
    case CD_FIELD_ID: {
        uint64_t val;
        memcpy(&val, ptr, sizeof(val));
        char buf[CD_ID_STR_BUF_SIZE];
        snprintf(buf, sizeof(buf), "%u:%u",
                 (uint32_t)(val & 0xFFFFFFFFU),
                 (uint32_t)(val >> 32U));
        return cJSON_CreateString(buf);
    }
    case CD_FIELD_MAT4:
    case CD_FIELD_STRUCT:
    case CD_FIELD_ARRAY:
        return cJSON_CreateNull();
    }
    return cJSON_CreateNull();
}

/* ============================================================================
 * Internal: Write a JSON value to a field in memory
 *
 * Returns true on success.
 * ============================================================================ */

static bool json_to_field(void* ptr, cd_field_kind_t kind, const cJSON* value) {
    switch (kind) {
    case CD_FIELD_INT32:
    case CD_FIELD_ENUM: {
        if (!cJSON_IsNumber(value)) return false;
        int32_t val = (int32_t)value->valuedouble;
        memcpy(ptr, &val, sizeof(val));
        return true;
    }
    case CD_FIELD_UINT32: {
        if (!cJSON_IsNumber(value)) return false;
        uint32_t val = (uint32_t)value->valuedouble;
        memcpy(ptr, &val, sizeof(val));
        return true;
    }
    case CD_FIELD_INT64: {
        if (!cJSON_IsNumber(value)) return false;
        int64_t val = (int64_t)value->valuedouble;
        memcpy(ptr, &val, sizeof(val));
        return true;
    }
    case CD_FIELD_FLOAT: {
        if (!cJSON_IsNumber(value)) return false;
        float val = (float)value->valuedouble;
        memcpy(ptr, &val, sizeof(val));
        return true;
    }
    case CD_FIELD_DOUBLE: {
        if (!cJSON_IsNumber(value)) return false;
        double val = value->valuedouble;
        memcpy(ptr, &val, sizeof(val));
        return true;
    }
    case CD_FIELD_BOOL: {
        if (!cJSON_IsBool(value)) return false;
        bool val = cJSON_IsTrue(value);
        memcpy(ptr, &val, sizeof(val));
        return true;
    }
    case CD_FIELD_STRING:
    case CD_FIELD_ASSET_URI: {
        if (!cJSON_IsString(value)) return false;
        cd_name_t name = cd_name_from_cstr(value->valuestring);
        memcpy(ptr, &name, sizeof(cd_name_t));
        return true;
    }
    case CD_FIELD_VEC3: {
        if (!cJSON_IsObject(value)) return false;
        float xyz[3];
        memcpy(xyz, ptr, sizeof(float) * 3); /* preserve existing */
        const cJSON* jx = cJSON_GetObjectItemCaseSensitive(value, "x");
        const cJSON* jy = cJSON_GetObjectItemCaseSensitive(value, "y");
        const cJSON* jz = cJSON_GetObjectItemCaseSensitive(value, "z");
        if (jx && cJSON_IsNumber(jx)) xyz[0] = (float)jx->valuedouble;
        if (jy && cJSON_IsNumber(jy)) xyz[1] = (float)jy->valuedouble;
        if (jz && cJSON_IsNumber(jz)) xyz[2] = (float)jz->valuedouble;
        memcpy(ptr, xyz, sizeof(float) * 3);
        return true;
    }
    case CD_FIELD_VEC4: {
        if (!cJSON_IsObject(value)) return false;
        float xyzw[4];
        memcpy(xyzw, ptr, sizeof(float) * 4);
        const cJSON* jx = cJSON_GetObjectItemCaseSensitive(value, "x");
        const cJSON* jy = cJSON_GetObjectItemCaseSensitive(value, "y");
        const cJSON* jz = cJSON_GetObjectItemCaseSensitive(value, "z");
        const cJSON* jw = cJSON_GetObjectItemCaseSensitive(value, "w");
        if (jx && cJSON_IsNumber(jx)) xyzw[0] = (float)jx->valuedouble;
        if (jy && cJSON_IsNumber(jy)) xyzw[1] = (float)jy->valuedouble;
        if (jz && cJSON_IsNumber(jz)) xyzw[2] = (float)jz->valuedouble;
        if (jw && cJSON_IsNumber(jw)) xyzw[3] = (float)jw->valuedouble;
        memcpy(ptr, xyzw, sizeof(float) * 4);
        return true;
    }
    case CD_FIELD_COLOR: {
        if (!cJSON_IsObject(value)) return false;
        float rgba[4];
        memcpy(rgba, ptr, sizeof(float) * 4);
        const cJSON* jr = cJSON_GetObjectItemCaseSensitive(value, "r");
        const cJSON* jg = cJSON_GetObjectItemCaseSensitive(value, "g");
        const cJSON* jb = cJSON_GetObjectItemCaseSensitive(value, "b");
        const cJSON* ja = cJSON_GetObjectItemCaseSensitive(value, "a");
        if (jr && cJSON_IsNumber(jr)) rgba[0] = (float)jr->valuedouble;
        if (jg && cJSON_IsNumber(jg)) rgba[1] = (float)jg->valuedouble;
        if (jb && cJSON_IsNumber(jb)) rgba[2] = (float)jb->valuedouble;
        if (ja && cJSON_IsNumber(ja)) rgba[3] = (float)ja->valuedouble;
        memcpy(ptr, rgba, sizeof(float) * 4);
        return true;
    }
    case CD_FIELD_QUAT: {
        if (!cJSON_IsObject(value)) return false;
        float xyzw[4];
        memcpy(xyzw, ptr, sizeof(float) * 4);
        const cJSON* jx = cJSON_GetObjectItemCaseSensitive(value, "x");
        const cJSON* jy = cJSON_GetObjectItemCaseSensitive(value, "y");
        const cJSON* jz = cJSON_GetObjectItemCaseSensitive(value, "z");
        const cJSON* jw = cJSON_GetObjectItemCaseSensitive(value, "w");
        if (jx && cJSON_IsNumber(jx)) xyzw[0] = (float)jx->valuedouble;
        if (jy && cJSON_IsNumber(jy)) xyzw[1] = (float)jy->valuedouble;
        if (jz && cJSON_IsNumber(jz)) xyzw[2] = (float)jz->valuedouble;
        if (jw && cJSON_IsNumber(jw)) xyzw[3] = (float)jw->valuedouble;
        memcpy(ptr, xyzw, sizeof(float) * 4);
        return true;
    }
    case CD_FIELD_ID: {
        if (!cJSON_IsNumber(value) && !cJSON_IsString(value)) return false;
        if (cJSON_IsNumber(value)) {
            uint64_t val = (uint64_t)value->valuedouble;
            memcpy(ptr, &val, sizeof(val));
        } else {
            cd_id_t val = cd_id_parse(value->valuestring);
            memcpy(ptr, &val, sizeof(val));
        }
        return true;
    }
    case CD_FIELD_MAT4:
    case CD_FIELD_STRUCT:
    case CD_FIELD_ARRAY:
        return false;
    }
    return false;
}

/* ============================================================================
 * Internal: Parse path "Component.field.subfield" and resolve to data pointer
 *
 * For "Transform" paths, resolves against node->local_transform using the
 * type registry's Transform type for field metadata.
 *
 * For component paths, looks up the component on the node and resolves
 * through the type registry.
 *
 * On success, sets *out_ptr, *out_field, and *out_is_transform.
 * out_is_transform indicates the caller should call
 * cd_node_set_local_transform() after writing to mark dirty.
 *
 * Returns true on success.
 * ============================================================================ */

typedef struct {
    void*          ptr;            /* Pointer to the resolved data */
    cd_field_t*    field;          /* Field descriptor for the leaf */
    bool           is_transform;   /* True if path targets Transform */
    cd_node_t*     node;           /* The resolved node */
    cd_id_t        node_id;        /* The node ID (for set_local_transform) */
} prop_resolve_t;

/* Subfield indices for vec3, vec4, quat, color */
static bool resolve_subfield_float(void* base_ptr, cd_field_kind_t kind,
                                    const char* subfield,
                                    void** out_ptr, cd_field_t* synthetic_field) {
    int component = -1;

    if (kind == CD_FIELD_VEC3) {
        if (strcmp(subfield, "x") == 0) component = 0;
        else if (strcmp(subfield, "y") == 0) component = 1;
        else if (strcmp(subfield, "z") == 0) component = 2;
    } else if (kind == CD_FIELD_VEC4 || kind == CD_FIELD_QUAT) {
        if (strcmp(subfield, "x") == 0) component = 0;
        else if (strcmp(subfield, "y") == 0) component = 1;
        else if (strcmp(subfield, "z") == 0) component = 2;
        else if (strcmp(subfield, "w") == 0) component = 3;
    } else if (kind == CD_FIELD_COLOR) {
        if (strcmp(subfield, "r") == 0) component = 0;
        else if (strcmp(subfield, "g") == 0) component = 1;
        else if (strcmp(subfield, "b") == 0) component = 2;
        else if (strcmp(subfield, "a") == 0) component = 3;
    }

    if (component < 0) return false;

    *out_ptr = (uint8_t*)base_ptr + (size_t)component * sizeof(float);
    /* Build a synthetic field descriptor for a single float */
    memset(synthetic_field, 0, sizeof(cd_field_t));
    synthetic_field->name = cd_name_from_cstr(subfield);
    synthetic_field->kind = CD_FIELD_FLOAT;
    synthetic_field->offset = 0;
    synthetic_field->size = sizeof(float);
    return true;
}

static bool resolve_prop_path(struct cd_kernel_t* kernel, cd_scene_t* scene,
                               cd_id_t node_id, const char* path,
                               prop_resolve_t* out,
                               int* error_code, const char** error_msg) {
    /* Parse path: split on first '.' to get component type name */
    if (path == NULL || path[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Empty property path",
                          "The 'path' parameter must be in 'Component.field' format.",
                          "Example: 'Transform.position' or 'MeshRenderer.visible'");
        return false;
    }

    /* Make a mutable copy */
    char path_buf[256];
    size_t path_len = strlen(path);
    if (path_len >= sizeof(path_buf)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Path too long (max 255 characters)";
        return false;
    }
    memcpy(path_buf, path, path_len + 1);

    /* Split on first '.' -> component_name, remaining */
    char* first_dot = strchr(path_buf, '.');
    if (first_dot == NULL) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "Path '%s' has no dot separator. Must be 'Component.field' format.", path);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Invalid property path", detail,
                          "Example: 'Transform.position' or 'Transform.position.x'");
        return false;
    }
    *first_dot = '\0';
    char* component_name = path_buf;
    char* remaining_path = first_dot + 1;

    if (remaining_path[0] == '\0') {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "Path '%s' ends with a dot. A field name is required after '%s.'.",
                 path, component_name);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Incomplete property path", detail, NULL);
        return false;
    }

    /* Look up the node */
    cd_node_t* node = cd_node_get(scene, node_id);
    if (node == NULL) {
        char id_str[24];
        snprintf(id_str, sizeof(id_str), "%u:%u",
                 cd_id_index(node_id), cd_id_generation(node_id));
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "Node with id '%s' does not exist in the scene.", id_str);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Node not found", detail,
                          "Use node.find to list available nodes.");
        return false;
    }

    out->node = node;
    out->node_id = node_id;

    /* Check if we have a type registry */
    cd_type_registry_t* reg = cd_kernel_get_types(kernel);

    /* Known Transform fields for fuzzy matching */
    static const char* transform_fields[] = {
        "position", "rotation", "scale"
    };
    static const uint32_t transform_field_count = 3;

    /* Special case: "Transform" maps to node->local_transform */
    if (strcmp(component_name, "Transform") == 0) {
        out->is_transform = true;

        if (reg == NULL) {
            *error_code = CD_JSONRPC_INTERNAL_ERROR;
            *error_msg  = "No type registry available";
            return false;
        }

        cd_id_t type_id = cd_type_find(reg, "Transform");
        if (type_id == CD_ID_INVALID) {
            *error_code = CD_JSONRPC_INTERNAL_ERROR;
            *error_msg  = "Transform type not registered";
            return false;
        }

        /* Use cd_type_resolve_path for field lookup */
        void* field_ptr = NULL;
        cd_field_t* field = NULL;

        /* Split remaining on '.' to check for subfields */
        char* sub_dot = strchr(remaining_path, '.');
        if (sub_dot != NULL) {
            /* We have a subfield, e.g. "position.y" */
            *sub_dot = '\0';
            char* field_name = remaining_path;
            char* subfield_name = sub_dot + 1;

            /* Resolve the field (e.g., "position") */
            cd_result_t res = cd_type_resolve_path(reg, type_id,
                                                    &node->local_transform,
                                                    field_name,
                                                    &field_ptr, &field);
            if (res != CD_OK) {
                char detail[256];
                const char* suggestion = cd_fuzzy_best_match(
                    field_name, transform_fields, transform_field_count);
                if (suggestion != NULL) {
                    snprintf(detail, sizeof(detail),
                             "Transform has no field '%s'. Did you mean '%s'?",
                             field_name, suggestion);
                } else {
                    snprintf(detail, sizeof(detail),
                             "Transform has no field '%s'. Available fields: position, rotation, scale.",
                             field_name);
                }
                *error_code = CD_JSONRPC_INVALID_PARAMS;
                *error_msg  = cd_mcp_error_fmt("Transform field not found", detail, NULL);
                return false;
            }

            /* Now resolve the subfield (e.g., "y" of vec3) */
            static cd_field_t synthetic_field;
            void* sub_ptr = NULL;
            if (!resolve_subfield_float(field_ptr, field->kind, subfield_name,
                                         &sub_ptr, &synthetic_field)) {
                char detail[256];
                snprintf(detail, sizeof(detail),
                         "Field 'Transform.%s' has no subfield '%s'.",
                         field_name, subfield_name);
                const char* sub_hint = NULL;
                if (field->kind == CD_FIELD_VEC3) {
                    sub_hint = "Valid subfields for vec3: x, y, z";
                } else if (field->kind == CD_FIELD_VEC4 || field->kind == CD_FIELD_QUAT) {
                    sub_hint = "Valid subfields: x, y, z, w";
                } else if (field->kind == CD_FIELD_COLOR) {
                    sub_hint = "Valid subfields for color: r, g, b, a";
                }
                *error_code = CD_JSONRPC_INVALID_PARAMS;
                *error_msg  = cd_mcp_error_fmt("Invalid subfield", detail, sub_hint);
                return false;
            }

            out->ptr = sub_ptr;
            out->field = &synthetic_field;
            return true;
        } else {
            /* No subfield -- resolve just the field name */
            cd_result_t res = cd_type_resolve_path(reg, type_id,
                                                    &node->local_transform,
                                                    remaining_path,
                                                    &field_ptr, &field);
            if (res != CD_OK) {
                char detail[256];
                const char* suggestion = cd_fuzzy_best_match(
                    remaining_path, transform_fields, transform_field_count);
                if (suggestion != NULL) {
                    snprintf(detail, sizeof(detail),
                             "Transform has no field '%s'. Did you mean '%s'?",
                             remaining_path, suggestion);
                } else {
                    snprintf(detail, sizeof(detail),
                             "Transform has no field '%s'. Available fields: position, rotation, scale.",
                             remaining_path);
                }
                *error_code = CD_JSONRPC_INVALID_PARAMS;
                *error_msg  = cd_mcp_error_fmt("Transform field not found", detail, NULL);
                return false;
            }

            out->ptr = field_ptr;
            out->field = field;
            return true;
        }
    }

    /* General component path resolution via type registry */
    out->is_transform = false;

    if (reg == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "No type registry available";
        return false;
    }

    cd_id_t type_id = cd_type_find(reg, component_name);
    if (type_id == CD_ID_INVALID) {
        /* Try fuzzy match against known component types */
        static const char* known_components[] = {
            "Transform", "MeshRenderer", "RigidBody", "Collider",
            "Camera", "Light", "AudioSource", "Script"
        };
        const char* suggestion = cd_fuzzy_best_match_ci(
            component_name, known_components, 8);

        char detail[256];
        if (suggestion != NULL) {
            snprintf(detail, sizeof(detail),
                     "Component type '%s' not found in registry. Did you mean '%s'?",
                     component_name, suggestion);
        } else {
            snprintf(detail, sizeof(detail),
                     "Component type '%s' not found in registry.",
                     component_name);
        }
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Component type not found", detail,
                          "Use system.capabilities to list available component types.");
        return false;
    }

    /* Get the component data from the node */
    void* comp_data = cd_node_get_component(scene, node_id, type_id);
    if (comp_data == NULL) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "Node '%s' (id=%u:%u) does not have a '%s' component.",
                 node->name.buf,
                 cd_id_index(node_id), cd_id_generation(node_id),
                 component_name);
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = cd_mcp_error_fmt("Component not found on node", detail,
                          "Attach the component first, or check the node's components with node.get.");
        return false;
    }

    /* Split remaining on '.' to check for subfields */
    char* sub_dot = strchr(remaining_path, '.');
    if (sub_dot != NULL) {
        *sub_dot = '\0';
        char* field_name = remaining_path;
        char* subfield_name = sub_dot + 1;

        void* field_ptr = NULL;
        cd_field_t* field = NULL;

        cd_result_t res = cd_type_resolve_path(reg, type_id, comp_data,
                                                field_name, &field_ptr, &field);
        if (res != CD_OK) {
            char detail[256];
            snprintf(detail, sizeof(detail),
                     "Component '%s' has no field '%s'.",
                     component_name, field_name);
            *error_code = CD_JSONRPC_INVALID_PARAMS;
            *error_msg  = cd_mcp_error_fmt("Component field not found", detail, NULL);
            return false;
        }

        static cd_field_t synthetic_comp_field;
        void* sub_ptr = NULL;
        if (!resolve_subfield_float(field_ptr, field->kind, subfield_name,
                                     &sub_ptr, &synthetic_comp_field)) {
            char detail[256];
            snprintf(detail, sizeof(detail),
                     "Field '%s.%s' has no subfield '%s'.",
                     component_name, field_name, subfield_name);
            *error_code = CD_JSONRPC_INVALID_PARAMS;
            *error_msg  = cd_mcp_error_fmt("Invalid subfield", detail, NULL);
            return false;
        }

        out->ptr = sub_ptr;
        out->field = &synthetic_comp_field;
        return true;
    } else {
        void* field_ptr = NULL;
        cd_field_t* field = NULL;

        cd_result_t res = cd_type_resolve_path(reg, type_id, comp_data,
                                                remaining_path,
                                                &field_ptr, &field);
        if (res != CD_OK) {
            char detail[256];
            snprintf(detail, sizeof(detail),
                     "Component '%s' has no field '%s'.",
                     component_name, remaining_path);
            *error_code = CD_JSONRPC_INVALID_PARAMS;
            *error_msg  = cd_mcp_error_fmt("Component field not found", detail, NULL);
            return false;
        }

        out->ptr = field_ptr;
        out->field = field;
        return true;
    }
}

/* ============================================================================
 * prop.set handler
 *
 * Input:  { "id": "2:3", "path": "Transform.position", "value": {...} }
 * Output: { "status": "ok" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_prop_set(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    /* Validate kernel */
    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Kernel not available";
        return NULL;
    }

    /* Validate scene exists */
    if (cd_kernel_get_scene(kernel) == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("No active scene",
                          "prop.set requires a scene to be loaded.",
                          "Call scene.create3d or scene.open to load a scene.");
        return NULL;
    }

    /* Extract "id" (required) */
    const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (id_item == NULL || !cJSON_IsString(id_item) ||
        id_item->valuestring == NULL || id_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: id";
        return NULL;
    }

    cd_id_t node_id = cd_id_parse(id_item->valuestring);
    if (!cd_id_is_valid(node_id)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Invalid id format (expected \"index:generation\")";
        return NULL;
    }

    /* Extract "path" (required) */
    const cJSON* path_item = cJSON_GetObjectItemCaseSensitive(params, "path");
    if (path_item == NULL || !cJSON_IsString(path_item) ||
        path_item->valuestring == NULL || path_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: path";
        return NULL;
    }

    /* Extract "value" (required) */
    const cJSON* value_item = cJSON_GetObjectItemCaseSensitive(params, "value");
    if (value_item == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing required parameter: value";
        return NULL;
    }

    /* Resolve the path */
    prop_resolve_t resolved;
    memset(&resolved, 0, sizeof(resolved));
    if (!resolve_prop_path(kernel, cd_kernel_get_scene(kernel), node_id,
                            path_item->valuestring,
                            &resolved, error_code, error_msg)) {
        return NULL;
    }

    /* For Transform fields, route through command queue to get undo/events.
     * Read the current transform, apply the JSON change, and enqueue. */
    if (resolved.is_transform) {
        /* Read the full current transform */
        cd_transform_t t = resolved.node->local_transform;

        /* Write the JSON value into our local copy */
        if (!json_to_field(
                (uint8_t*)&t + ((uint8_t*)resolved.ptr - (uint8_t*)&resolved.node->local_transform),
                resolved.field->kind, value_item)) {
            *error_code = CD_JSONRPC_INVALID_PARAMS;
            *error_msg  = "Value type mismatch for field";
            return NULL;
        }

        /* Enqueue SET_TRANSFORM command */
        cd_command_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = CD_CMD_SET_TRANSFORM;
        cmd.target = resolved.node_id;
        cmd.payload.set_transform.transform = t;

        cd_result_t cmd_res = cd_command_execute_sync(
            cd_kernel_get_commands(kernel), &cmd, cd_kernel_get_scene(kernel), cd_kernel_get_events(kernel));
        if (cmd_res != CD_OK) {
            const cd_error_context_t* err = cd_get_last_error();
            *error_code = CD_JSONRPC_INTERNAL_ERROR;
            *error_msg  = (err && err->message) ? err->message : "Failed to set transform via command";
            return NULL;
        }
    } else {
        /* Non-transform component property: write directly.
         * Component prop writes go through the resolved pointer. */
        if (!json_to_field(resolved.ptr, resolved.field->kind, value_item)) {
            *error_code = CD_JSONRPC_INVALID_PARAMS;
            *error_msg  = "Value type mismatch for field";
            return NULL;
        }
    }

    /* Build the response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    return result;
}

/* ============================================================================
 * prop.get handler
 *
 * Input:  { "id": "2:3", "path": "Transform.position" }
 * Output: { "value": {...}, "type": "vec3" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_prop_get(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    /* Validate kernel */
    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Kernel not available";
        return NULL;
    }

    /* Validate scene exists */
    if (cd_kernel_get_scene(kernel) == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = cd_mcp_error_fmt("No active scene",
                          "prop.get requires a scene to be loaded.",
                          "Call scene.create3d or scene.open to load a scene.");
        return NULL;
    }

    /* Extract "id" (required) */
    const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (id_item == NULL || !cJSON_IsString(id_item) ||
        id_item->valuestring == NULL || id_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: id";
        return NULL;
    }

    cd_id_t node_id = cd_id_parse(id_item->valuestring);
    if (!cd_id_is_valid(node_id)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Invalid id format (expected \"index:generation\")";
        return NULL;
    }

    /* Extract "path" (required) */
    const cJSON* path_item = cJSON_GetObjectItemCaseSensitive(params, "path");
    if (path_item == NULL || !cJSON_IsString(path_item) ||
        path_item->valuestring == NULL || path_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or empty required parameter: path";
        return NULL;
    }

    /* Resolve the path */
    prop_resolve_t resolved;
    memset(&resolved, 0, sizeof(resolved));
    if (!resolve_prop_path(kernel, cd_kernel_get_scene(kernel), node_id,
                            path_item->valuestring,
                            &resolved, error_code, error_msg)) {
        return NULL;
    }

    /* Read the value */
    cJSON* value = field_to_json(resolved.ptr, resolved.field->kind);
    if (value == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to serialize field value";
        return NULL;
    }

    /* Build the response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        cJSON_Delete(value);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddItemToObject(result, "value", value);
    cJSON_AddStringToObject(result, "type",
                            field_kind_to_type_name(resolved.field->kind));

    return result;
}

/* ============================================================================
 * prop.batchSet handler
 *
 * Input:  { "properties": [
 *            { "id": "2:3", "path": "Transform.position", "value": {...} },
 *            ...
 *          ] }
 * Output: { "status": "ok", "count": 3 }
 * ============================================================================ */

static cJSON* cd_mcp_handle_prop_batch_set(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    /* Validate kernel */
    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Kernel not available";
        return NULL;
    }

    /* Validate scene exists */
    if (cd_kernel_get_scene(kernel) == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "No active scene";
        return NULL;
    }

    /* Extract "properties" (required) */
    const cJSON* props_item = cJSON_GetObjectItemCaseSensitive(params, "properties");
    if (props_item == NULL || !cJSON_IsArray(props_item)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or invalid required parameter: properties (must be array)";
        return NULL;
    }

    int count = 0;
    const cJSON* entry = NULL;
    cJSON_ArrayForEach(entry, props_item) {
        if (!cJSON_IsObject(entry)) {
            *error_code = CD_JSONRPC_INVALID_PARAMS;
            *error_msg  = "Each property entry must be an object";
            return NULL;
        }

        /* Extract id */
        const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(entry, "id");
        if (id_item == NULL || !cJSON_IsString(id_item) ||
            id_item->valuestring == NULL || id_item->valuestring[0] == '\0') {
            *error_code = CD_JSONRPC_INVALID_PARAMS;
            *error_msg  = "Missing or empty id in property entry";
            return NULL;
        }

        cd_id_t node_id = cd_id_parse(id_item->valuestring);
        if (!cd_id_is_valid(node_id)) {
            *error_code = CD_JSONRPC_INVALID_PARAMS;
            *error_msg  = "Invalid id format in property entry";
            return NULL;
        }

        /* Extract path */
        const cJSON* path_item = cJSON_GetObjectItemCaseSensitive(entry, "path");
        if (path_item == NULL || !cJSON_IsString(path_item) ||
            path_item->valuestring == NULL || path_item->valuestring[0] == '\0') {
            *error_code = CD_JSONRPC_INVALID_PARAMS;
            *error_msg  = "Missing or empty path in property entry";
            return NULL;
        }

        /* Extract value */
        const cJSON* value_item = cJSON_GetObjectItemCaseSensitive(entry, "value");
        if (value_item == NULL) {
            *error_code = CD_JSONRPC_INVALID_PARAMS;
            *error_msg  = "Missing value in property entry";
            return NULL;
        }

        /* Resolve the path */
        prop_resolve_t resolved;
        memset(&resolved, 0, sizeof(resolved));
        if (!resolve_prop_path(kernel, cd_kernel_get_scene(kernel), node_id,
                                path_item->valuestring,
                                &resolved, error_code, error_msg)) {
            return NULL;
        }

        /* Route Transform writes through command queue */
        if (resolved.is_transform) {
            cd_transform_t t = resolved.node->local_transform;
            if (!json_to_field(
                    (uint8_t*)&t + ((uint8_t*)resolved.ptr - (uint8_t*)&resolved.node->local_transform),
                    resolved.field->kind, value_item)) {
                *error_code = CD_JSONRPC_INVALID_PARAMS;
                *error_msg  = "Value type mismatch in property entry";
                return NULL;
            }

            cd_command_t cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.type = CD_CMD_SET_TRANSFORM;
            cmd.target = resolved.node_id;
            cmd.payload.set_transform.transform = t;
            cmd.undo_policy = CD_UNDO_COALESCE; /* P2-9: reduce undo entries in batch */
            cd_command_execute_sync(
                cd_kernel_get_commands(kernel), &cmd, cd_kernel_get_scene(kernel), cd_kernel_get_events(kernel));
        } else {
            if (!json_to_field(resolved.ptr, resolved.field->kind, value_item)) {
                *error_code = CD_JSONRPC_INVALID_PARAMS;
                *error_msg  = "Value type mismatch in property entry";
                return NULL;
            }
        }

        count++;
    }

    /* Build the response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddNumberToObject(result, "count", (double)count);
    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_prop_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "prop.set",
        cd_mcp_handle_prop_set,
        "Set a component property by dot-path on a node",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\",\"description\":\"Node ID (index:generation)\"},"
        "\"path\":{\"type\":\"string\",\"description\":\"Property path (e.g. Transform.position)\"},"
        "\"value\":{\"description\":\"Value to set (type must match the field)\"}"
        "},\"required\":[\"id\",\"path\",\"value\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "prop.get",
        cd_mcp_handle_prop_get,
        "Get a component property value by dot-path from a node",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\",\"description\":\"Node ID (index:generation)\"},"
        "\"path\":{\"type\":\"string\",\"description\":\"Property path (e.g. Transform.position)\"}"
        "},\"required\":[\"id\",\"path\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "prop.batchSet",
        cd_mcp_handle_prop_batch_set,
        "Set multiple component properties atomically",
        "{\"type\":\"object\",\"properties\":{"
        "\"properties\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"value\":{}"
        "},\"required\":[\"id\",\"path\",\"value\"]},\"description\":\"Array of property entries to set\"}"
        "},\"required\":[\"properties\"]}");
    if (res != CD_OK) return res;

    return CD_OK;
}
