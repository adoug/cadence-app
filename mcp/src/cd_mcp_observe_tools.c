/* cd_mcp_observe_tools.c - Cadence Engine MCP observe tool handlers
 *
 * Implements:
 *   - observe.state : Read runtime state of nodes (Transform fields,
 *                     component properties, Lua self.* properties)
 *                     during or outside play mode.
 *   - observe.perf  : Returns real performance counters from kernel (Task 8.4).
 *
 * Key format for observe.state:
 *   "Transform.position"  -> reads node->local_transform.position (vec3)
 *   "Transform.rotation"  -> reads node->local_transform.rotation (quat)
 *   "Transform.scale"     -> reads node->local_transform.scale (vec3)
 *   "ComponentType.field" -> reads component property via type registry
 *   "LuaScript.<field>"   -> reads self.<field> from attached Lua scripts
 *
 * All handlers follow the cd_mcp_tool_handler_t signature.
 * This tool is read-only: no commands are enqueued and no scene state is
 * modified, so calling directly from the MCP thread is safe.
 *
 * Task 5.7 (initial), Task 8.3 (component/transform property reads).
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_tool_state.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_scene.h"
#include "cadence/cd_type_registry.h"
#include "cadence/cd_builtin_types.h"
#include "cadence/cd_script_instance.h"
#include "cJSON.h"

#include <lua.h>
#include <lauxlib.h>

#include <stdio.h>
#include <string.h>
#include <stddef.h>

/* ============================================================================
 * Global script manager pointer.
 *
 * The observe tools share the same global pointer as the script tools.
 * cd_mcp_observe_tools_set_mgr() is provided so that test harnesses and the
 * plugin initializer can supply the pointer without a hard link-time
 * dependency on cd_mcp_script_tools.c.
 * ============================================================================ */

static cd_script_mgr_t* s_fallback_observe_mgr = NULL;

void cd_mcp_observe_tools_set_mgr(void* mgr) {
    s_fallback_observe_mgr = (cd_script_mgr_t*)mgr;
}

void* cd_mcp_observe_tools_get_mgr(void) {
    return (void*)s_fallback_observe_mgr;
}

/** Get the observe script manager, preferring kernel tool state if available. */
static cd_script_mgr_t* get_observe_mgr(struct cd_kernel_t* kernel) {
    if (kernel && cd_kernel_get_mcp_tool_state(kernel) && cd_kernel_get_mcp_tool_state(kernel)->script.observe_script_mgr) {
        return cd_kernel_get_mcp_tool_state(kernel)->script.observe_script_mgr;
    }
    return s_fallback_observe_mgr;
}

/* ============================================================================
 * Internal: Parse "index:generation" string to cd_id_t
 * ============================================================================ */

static cd_id_t observe_id_parse(const char* str) {
    if (!str || str[0] == '\0') {
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
 * Internal: Convert the Lua value at the top of the stack to a cJSON value.
 *
 * Handles number, string, boolean, nil, and all other types (mapped to null).
 * The returned value is heap-allocated; caller must call cJSON_Delete().
 * Does NOT pop the Lua value -- the caller is responsible for that.
 * ============================================================================ */

static cJSON* lua_stack_top_to_cjson(lua_State* L) {
    int ltype = lua_type(L, -1);
    switch (ltype) {
        case LUA_TNUMBER: {
            lua_Number n = lua_tonumber(L, -1);
            return cJSON_CreateNumber((double)n);
        }
        case LUA_TSTRING: {
            const char* s = lua_tostring(L, -1);
            return cJSON_CreateString(s ? s : "");
        }
        case LUA_TBOOLEAN: {
            int b = lua_toboolean(L, -1);
            return b ? cJSON_CreateTrue() : cJSON_CreateFalse();
        }
        default:
            return cJSON_CreateNull();
    }
}

/* ============================================================================
 * Internal: Convert a field value at a memory pointer to cJSON.
 *
 * This is a local copy of the field_to_json logic from cd_mcp_prop_tools.c,
 * kept here to avoid cross-module coupling. Supports the field kinds
 * commonly used by Transform and game components.
 * ============================================================================ */

static cJSON* observe_field_to_json(const void* ptr, cd_field_kind_t kind) {
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
        char buf[24];
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
 * Internal: Resolve a component/transform property key for a given node.
 *
 * Uses the type registry to look up component types and fields.
 * For "Transform" keys, reads directly from node->local_transform.
 * For other component types, looks up the component on the node.
 *
 * Returns a cJSON value on success, or NULL if the key cannot be resolved
 * (caller should fall back to cJSON_CreateNull()).
 * ============================================================================ */

static cJSON* resolve_component_key(struct cd_kernel_t* kernel,
                                     cd_scene_t* scene,
                                     cd_id_t node_id,
                                     const char* key) {
    if (kernel == NULL || scene == NULL) {
        return NULL;
    }

    cd_type_registry_t* reg = cd_kernel_get_types(kernel);
    if (reg == NULL) {
        return NULL;
    }

    /* Parse key into "TypeName" and "field_path" on first '.' */
    char key_buf[256];
    size_t key_len = strlen(key);
    if (key_len == 0 || key_len >= sizeof(key_buf)) {
        return NULL;
    }
    memcpy(key_buf, key, key_len + 1);

    char* dot = strchr(key_buf, '.');
    if (dot == NULL) {
        return NULL; /* Must be "TypeName.field" */
    }
    *dot = '\0';
    char* type_name  = key_buf;
    char* field_path = dot + 1;

    if (field_path[0] == '\0') {
        return NULL;
    }

    /* Look up the node */
    cd_node_t* node = cd_node_get(scene, node_id);
    if (node == NULL) {
        return NULL;
    }

    /* Special case: Transform reads from node->local_transform */
    if (strcmp(type_name, "Transform") == 0) {
        cd_id_t type_id = cd_type_find(reg, "Transform");
        if (type_id == CD_ID_INVALID) {
            return NULL;
        }

        void* field_ptr = NULL;
        cd_field_t* field = NULL;

        cd_result_t res = cd_type_resolve_path(reg, type_id,
                                                &node->local_transform,
                                                field_path,
                                                &field_ptr, &field);
        if (res != CD_OK || field_ptr == NULL || field == NULL) {
            return NULL;
        }

        return observe_field_to_json(field_ptr, field->kind);
    }

    /* General component: look up type, get component data, resolve field */
    cd_id_t type_id = cd_type_find(reg, type_name);
    if (type_id == CD_ID_INVALID) {
        return NULL;
    }

    void* comp_data = cd_node_get_component(scene, node_id, type_id);
    if (comp_data == NULL) {
        return NULL;
    }

    void* field_ptr = NULL;
    cd_field_t* field = NULL;

    cd_result_t res = cd_type_resolve_path(reg, type_id, comp_data,
                                            field_path, &field_ptr, &field);
    if (res != CD_OK || field_ptr == NULL || field == NULL) {
        return NULL;
    }

    return observe_field_to_json(field_ptr, field->kind);
}

/* ============================================================================
 * Internal: Resolve a single key for a given node.
 *
 * Key format:
 *   "LuaScript.<field>"   -> read self.<field> from attached Lua scripts
 *   "Transform.<field>"   -> read from node->local_transform via type registry
 *   "<Component>.<field>" -> read component property via type registry
 *
 * Returns a newly allocated cJSON* (never NULL -- returns cJSON_CreateNull()
 * as the fallback).  Caller must call cJSON_Delete().
 * ============================================================================ */

#define LUASCRIPT_PREFIX     "LuaScript."
#define LUASCRIPT_PREFIX_LEN 10   /* strlen("LuaScript.") */

static cJSON* resolve_key_for_node(struct cd_kernel_t* kernel,
                                    cd_scene_t* scene,
                                    cd_id_t node_id,
                                    const char* key) {
    if (!key || key[0] == '\0') {
        return cJSON_CreateNull();
    }

    /* LuaScript.<field> path -- uses Lua VM directly */
    if (strncmp(key, LUASCRIPT_PREFIX, LUASCRIPT_PREFIX_LEN) == 0) {
        const char* field = key + LUASCRIPT_PREFIX_LEN;
        if (field[0] == '\0') {
            return cJSON_CreateNull();
        }

        if (get_observe_mgr(kernel) == NULL) {
            return cJSON_CreateNull();
        }

        /* Find the first instance that owns this field. */
        cd_id_t inst_id = cd_script_mgr_find_prop_instance(
            get_observe_mgr(kernel), node_id, field);

        if (!cd_id_is_valid(inst_id)) {
            return cJSON_CreateNull();
        }

        /* Push the Lua value onto the stack and convert it. */
        lua_State* L = get_observe_mgr(kernel)->L;
        int pushed = cd_script_instance_push_prop(get_observe_mgr(kernel),
                                                   inst_id, field);
        if (!pushed) {
            return cJSON_CreateNull();
        }

        cJSON* val = lua_stack_top_to_cjson(L);
        lua_pop(L, 1);

        return val ? val : cJSON_CreateNull();
    }

    /* Try to resolve as a component/transform property via type registry */
    cJSON* comp_val = resolve_component_key(kernel, scene, node_id, key);
    if (comp_val != NULL) {
        return comp_val;
    }

    /* Unrecognized key -- return null */
    return cJSON_CreateNull();
}

/* ============================================================================
 * observe.state handler
 *
 * Input:
 *   {
 *     "nodeIds": ["2:3", "2:4"],
 *     "keys": ["Transform.position", "RigidBody.velocity", "LuaScript.health"]
 *   }
 *
 * Output:
 *   {
 *     "states": {
 *       "2:3": {
 *         "Transform.position": { "x": 5.2, "y": 1.0, "z": -2.8 },
 *         "RigidBody.velocity": { "x": 0, "y": -0.1, "z": 2.5 },
 *         "LuaScript.health": 85
 *       },
 *       "2:4": { ... }
 *     },
 *     "frame": 1542,
 *     "time": 25.7
 *   }
 *
 * Error cases:
 *   CD_JSONRPC_INVALID_PARAMS: missing or empty nodeIds / keys arrays
 *   CD_JSONRPC_INTERNAL_ERROR: kernel not available
 * ============================================================================ */

static cJSON* cd_mcp_handle_observe_state(
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

    if (params == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing parameters";
        return NULL;
    }

    /* Extract "nodeIds" array (required) */
    const cJSON* node_ids_item = cJSON_GetObjectItemCaseSensitive(params, "nodeIds");
    if (node_ids_item == NULL || !cJSON_IsArray(node_ids_item)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or invalid required parameter: nodeIds (must be array)";
        return NULL;
    }

    int node_count = cJSON_GetArraySize(node_ids_item);
    if (node_count == 0) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "nodeIds array must not be empty";
        return NULL;
    }

    /* Extract "keys" array (required) */
    const cJSON* keys_item = cJSON_GetObjectItemCaseSensitive(params, "keys");
    if (keys_item == NULL || !cJSON_IsArray(keys_item)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or invalid required parameter: keys (must be array)";
        return NULL;
    }

    int key_count = cJSON_GetArraySize(keys_item);
    if (key_count == 0) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "keys array must not be empty";
        return NULL;
    }

    /* Build the result object */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON* states = cJSON_CreateObject();
    if (states == NULL) {
        cJSON_Delete(result);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate states object";
        return NULL;
    }
    cJSON_AddItemToObject(result, "states", states);

    /* For each node ID, build a per-node state object */
    const cJSON* node_id_elem = NULL;
    cJSON_ArrayForEach(node_id_elem, node_ids_item) {
        if (!cJSON_IsString(node_id_elem) || node_id_elem->valuestring == NULL) {
            continue;
        }

        const char* node_id_str = node_id_elem->valuestring;
        cd_id_t node_id = observe_id_parse(node_id_str);

        cJSON* node_state = cJSON_CreateObject();
        if (node_state == NULL) {
            continue;
        }

        /* Resolve each key for this node */
        const cJSON* key_elem = NULL;
        cJSON_ArrayForEach(key_elem, keys_item) {
            if (!cJSON_IsString(key_elem) || key_elem->valuestring == NULL) {
                continue;
            }
            const char* key = key_elem->valuestring;

            cJSON* val;
            if (cd_id_is_valid(node_id)) {
                val = resolve_key_for_node(kernel, cd_kernel_get_scene(kernel),
                                            node_id, key);
            } else {
                val = cJSON_CreateNull();
            }

            if (val == NULL) {
                val = cJSON_CreateNull();
            }

            cJSON_AddItemToObject(node_state, key, val);
        }

        cJSON_AddItemToObject(states, node_id_str, node_state);
    }

    /* frame and time counters -- populated from kernel when available */
    uint64_t frame_count = 0;
    double   time_elapsed = 0.0;
    if (kernel != NULL) {
        frame_count  = cd_kernel_get_frame_count(kernel);
        time_elapsed = cd_kernel_get_time(kernel);
    }
    cJSON_AddNumberToObject(result, "frame", (double)frame_count);
    cJSON_AddNumberToObject(result, "time",  time_elapsed);

    return result;
}

/* ============================================================================
 * observe.perf handler (Task 8.4)
 *
 * Returns real performance counters from kernel timing fields populated
 * by cd_engine_tick().
 *
 * Output:
 *   {
 *     "fps": <double>,
 *     "frame_time_ms": <double>,
 *     "physics_time_ms": <double>,
 *     "render_time_ms": <double>,
 *     "script_time_ms": <double>,
 *     "node_count": <int>,
 *     "memory_mb": 0.0
 *   }
 * ============================================================================ */

static cJSON* cd_mcp_handle_observe_perf(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)error_code;
    (void)error_msg;

    /* Check for reset_hwm parameter (Phase 3A-6) */
    if (params != NULL && kernel != NULL) {
        const cJSON* reset_item = cJSON_GetObjectItemCaseSensitive(params, "reset_hwm");
        if (reset_item != NULL && cJSON_IsTrue(reset_item)) {
            /* Reset high-water marks to current values */
            cd_kernel_set_hwm_node_count(kernel, cd_kernel_get_scene(kernel) ? cd_kernel_get_scene(kernel)->nodes.count : 0);
            cd_kernel_set_hwm_draw_items(kernel, cd_kernel_get_perf_draw_calls(kernel));
            cd_kernel_set_hwm_job_submissions(kernel, 0);
            cd_kernel_set_hwm_octree_items(kernel, 0);
        }
    }

    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    /* Read performance counters from the kernel (populated by cd_engine_tick). */
    double fps             = 0.0;
    double frame_time_ms   = 0.0;
    double physics_time_ms = 0.0;
    double render_time_ms  = 0.0;
    double script_time_ms  = 0.0;
    uint32_t node_count    = 0;

    if (kernel != NULL) {
        fps             = cd_kernel_get_perf_fps(kernel);
        frame_time_ms   = cd_kernel_get_perf_frame_time_ms(kernel);
        physics_time_ms = cd_kernel_get_perf_physics_time_ms(kernel);
        render_time_ms  = cd_kernel_get_perf_render_time_ms(kernel);
        script_time_ms  = cd_kernel_get_perf_script_time_ms(kernel);

        if (cd_kernel_get_scene(kernel) != NULL) {
            node_count = cd_kernel_get_scene(kernel)->nodes.count;
        }
    }

    cJSON_AddNumberToObject(result, "fps",              fps);
    cJSON_AddNumberToObject(result, "frame_time_ms",    frame_time_ms);
    cJSON_AddNumberToObject(result, "physics_time_ms",  physics_time_ms);
    cJSON_AddNumberToObject(result, "render_time_ms",   render_time_ms);
    cJSON_AddNumberToObject(result, "script_time_ms",   script_time_ms);
    cJSON_AddNumberToObject(result, "node_count",       (double)node_count);
    cJSON_AddNumberToObject(result, "memory_mb",        0.0);

    /* Draw call statistics (Step 4A.3) — written by renderer to kernel */
    if (kernel != NULL) {
        cJSON_AddNumberToObject(result, "draw_calls",       (double)cd_kernel_get_perf_draw_calls(kernel));
        cJSON_AddNumberToObject(result, "instanced_draws",  (double)cd_kernel_get_perf_instanced_draws(kernel));
        cJSON_AddNumberToObject(result, "instanced_items",  (double)cd_kernel_get_perf_instanced_items(kernel));
        cJSON_AddNumberToObject(result, "mdi_dispatches",   (double)cd_kernel_get_perf_mdi_dispatches(kernel));
        cJSON_AddNumberToObject(result, "mdi_commands",     (double)cd_kernel_get_perf_mdi_cmds(kernel));

        /* VRAM budget stats (Step 4D.3) */
        cJSON_AddNumberToObject(result, "vram_used_mb",
            (double)cd_kernel_get_perf_vram_used(kernel) / (1024.0 * 1024.0));
        cJSON_AddNumberToObject(result, "vram_budget_mb",
            (double)cd_kernel_get_perf_vram_budget(kernel) / (1024.0 * 1024.0));

        /* High-water mark telemetry (Phase 3A-6) */
        cJSON_AddNumberToObject(result, "hwm_node_count",      (double)cd_kernel_get_hwm_node_count(kernel));
        cJSON_AddNumberToObject(result, "hwm_draw_items",      (double)cd_kernel_get_hwm_draw_items(kernel));
        cJSON_AddNumberToObject(result, "hwm_job_submissions", (double)cd_kernel_get_hwm_job_submissions(kernel));
        cJSON_AddNumberToObject(result, "hwm_octree_items",    (double)cd_kernel_get_hwm_octree_items(kernel));

        /* Engine config from project.toml [engine] section (Phase 3A-6) */
        cJSON_AddNumberToObject(result, "cfg_max_nodes",       (double)cd_kernel_get_project(kernel)->engine.max_nodes);
        cJSON_AddNumberToObject(result, "cfg_max_draw_items",  (double)cd_kernel_get_project(kernel)->engine.max_draw_items);
        cJSON_AddNumberToObject(result, "cfg_vram_budget_mb",  (double)cd_kernel_get_project(kernel)->engine.vram_budget_mb);
    } else {
        cJSON_AddNumberToObject(result, "draw_calls",       0.0);
        cJSON_AddNumberToObject(result, "instanced_draws",  0.0);
        cJSON_AddNumberToObject(result, "instanced_items",  0.0);
        cJSON_AddNumberToObject(result, "mdi_dispatches",   0.0);
        cJSON_AddNumberToObject(result, "mdi_commands",     0.0);
        cJSON_AddNumberToObject(result, "vram_used_mb",     0.0);
        cJSON_AddNumberToObject(result, "vram_budget_mb",   0.0);
        cJSON_AddNumberToObject(result, "hwm_node_count",      0.0);
        cJSON_AddNumberToObject(result, "hwm_draw_items",      0.0);
        cJSON_AddNumberToObject(result, "hwm_job_submissions", 0.0);
        cJSON_AddNumberToObject(result, "hwm_octree_items",    0.0);
        cJSON_AddNumberToObject(result, "cfg_max_nodes",       0.0);
        cJSON_AddNumberToObject(result, "cfg_max_draw_items",  0.0);
        cJSON_AddNumberToObject(result, "cfg_vram_budget_mb",  0.0);
    }

    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_observe_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "observe.state", cd_mcp_handle_observe_state,
        "Read runtime state of nodes including transform, component, and Lua properties",
        "{\"type\":\"object\",\"properties\":{"
        "\"nodeIds\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Node IDs to observe (index:generation format)\"},"
        "\"keys\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Property keys to read (e.g. Transform.position, LuaScript.health)\"}"
        "},\"required\":[\"nodeIds\",\"keys\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "observe.perf", cd_mcp_handle_observe_perf,
        "Return real-time performance counters and high-water marks from the engine",
        "{\"type\":\"object\",\"properties\":{"
        "\"reset_hwm\":{\"type\":\"boolean\",\"description\":\"If true, reset all high-water marks to current values\"}"
        "}}");
    if (res != CD_OK) return res;

    return CD_OK;
}
