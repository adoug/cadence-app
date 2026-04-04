/* cd_mcp_nav_tools.c - MCP tool handlers for navigation/pathfinding
 *
 * Tools:
 *   nav.bake       - Trigger navgrid bake via Lua
 *   nav.find_path  - Find path between two points
 *   nav.debug_draw - Enable/disable debug visualization
 *
 * These tools invoke Lua functions on the global _nav_grid object
 * (created by nav_controller.lua or user scripts).
 */
#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

/* ============================================================================
 * Lua evaluation function pointer (set by scripting_lua plugin)
 * ============================================================================ */

typedef const char* (*cd_lua_eval_fn)(const char* code);
static cd_lua_eval_fn s_lua_eval = NULL;

void cd_mcp_nav_tools_set_lua_eval(void* fn) {
    s_lua_eval = (cd_lua_eval_fn)fn;
}

/* ============================================================================
 * nav.bake - Trigger navgrid obstacle bake
 *
 * Params (optional):
 *   obstacles: array of {min_x, min_z, max_x, max_z} boxes
 *
 * If no obstacles provided, calls nav:bake() (scene-based).
 * ============================================================================ */

static cJSON* handle_nav_bake(cd_kernel_t* kernel, const cJSON* params,
                               int* error_code, const char** error_msg) {
    (void)kernel;

    if (!s_lua_eval) {
        *error_code = -32603;
        *error_msg = "Lua eval not initialized (scripting_lua not loaded)";
        return NULL;
    }

    const cJSON* obstacles = cJSON_GetObjectItemCaseSensitive(params, "obstacles");
    if (obstacles && cJSON_IsArray(obstacles)) {
        /* Build Lua code to bake from obstacle list */
        char lua_buf[4096];
        int offset = 0;
        offset += snprintf(lua_buf + offset, sizeof(lua_buf) - (size_t)offset,
            "local nav = _G._nav_grid\n"
            "if not nav then return '{\"error\":\"no nav grid\"}' end\n"
            "nav:bake_from_obstacles({\n");

        const cJSON* obs;
        cJSON_ArrayForEach(obs, obstacles) {
            const cJSON* mx = cJSON_GetObjectItemCaseSensitive(obs, "min_x");
            const cJSON* mz = cJSON_GetObjectItemCaseSensitive(obs, "min_z");
            const cJSON* xx = cJSON_GetObjectItemCaseSensitive(obs, "max_x");
            const cJSON* xz = cJSON_GetObjectItemCaseSensitive(obs, "max_z");
            if (mx && mz && xx && xz) {
                offset += snprintf(lua_buf + offset, sizeof(lua_buf) - (size_t)offset,
                    "  {%f, %f, %f, %f},\n",
                    mx->valuedouble, mz->valuedouble,
                    xx->valuedouble, xz->valuedouble);
            }
            if (offset >= (int)sizeof(lua_buf) - 100) break;
        }
        offset += snprintf(lua_buf + offset, sizeof(lua_buf) - (size_t)offset,
            "})\n"
            "local s = nav:stats()\n"
            "return string.format('{\"ok\":true,\"blocked\":%%d,\"walkable\":%%d}', "
            "s.blocked_cells, s.walkable_cells)\n");

        const char* result_str = s_lua_eval(lua_buf);
        if (result_str) {
            cJSON* result = cJSON_Parse(result_str);
            if (result) return result;
        }

        cJSON* result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "ok", 1);
        return result;
    } else {
        /* No obstacles - call nav:bake() for scene-based baking */
        const char* result_str = s_lua_eval(
            "local nav = _G._nav_grid\n"
            "if not nav then return '{\"error\":\"no nav grid\"}' end\n"
            "nav:bake()\n"
            "local s = nav:stats()\n"
            "return string.format('{\"ok\":true,\"blocked\":%d,\"walkable\":%d}', "
            "s.blocked_cells, s.walkable_cells)\n"
        );
        if (result_str) {
            cJSON* result = cJSON_Parse(result_str);
            if (result) return result;
        }
        cJSON* result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "ok", 1);
        return result;
    }
}

/* ============================================================================
 * nav.find_path - Find path between two world positions
 *
 * Params:
 *   start_x, start_z: start position (required)
 *   goal_x, goal_z: goal position (required)
 *   smooth: bool (optional, default true)
 *
 * Returns:
 *   path: array of {x, y, z} waypoints, or null if no path
 * ============================================================================ */

static cJSON* handle_nav_find_path(cd_kernel_t* kernel, const cJSON* params,
                                     int* error_code, const char** error_msg) {
    (void)kernel;

    if (!s_lua_eval) {
        *error_code = -32603;
        *error_msg = "Lua eval not initialized";
        return NULL;
    }

    const cJSON* sx = cJSON_GetObjectItemCaseSensitive(params, "start_x");
    const cJSON* sz = cJSON_GetObjectItemCaseSensitive(params, "start_z");
    const cJSON* gx = cJSON_GetObjectItemCaseSensitive(params, "goal_x");
    const cJSON* gz = cJSON_GetObjectItemCaseSensitive(params, "goal_z");

    if (!sx || !sz || !gx || !gz) {
        *error_code = -32602;
        *error_msg = "Missing required parameters: start_x, start_z, goal_x, goal_z";
        return NULL;
    }

    const cJSON* smooth_json = cJSON_GetObjectItemCaseSensitive(params, "smooth");
    const char* smooth_str = (smooth_json && cJSON_IsFalse(smooth_json)) ? "false" : "true";

    char lua_buf[1024];
    snprintf(lua_buf, sizeof(lua_buf),
        "local nav = _G._nav_grid\n"
        "if not nav then return '{\"error\":\"no nav grid\"}' end\n"
        "local path = nav:find_path(%f, %f, %f, %f, {smooth=%s})\n"
        "if not path then return '{\"path\":null}' end\n"
        "local parts = {}\n"
        "for _, wp in ipairs(path) do\n"
        "  parts[#parts+1] = string.format('{\"x\":%%f,\"y\":%%f,\"z\":%%f}', wp.x, wp.y, wp.z)\n"
        "end\n"
        "return '{\"path\":[' .. table.concat(parts, ',') .. ']}'\n",
        sx->valuedouble, sz->valuedouble,
        gx->valuedouble, gz->valuedouble,
        smooth_str);

    const char* result_str = s_lua_eval(lua_buf);
    if (result_str) {
        cJSON* result = cJSON_Parse(result_str);
        if (result) return result;
    }

    /* Fallback */
    cJSON* result = cJSON_CreateObject();
    cJSON_AddNullToObject(result, "path");
    return result;
}

/* ============================================================================
 * nav.debug_draw - Enable/disable debug visualization
 *
 * Params:
 *   enabled: bool (required)
 * ============================================================================ */

static cJSON* handle_nav_debug_draw(cd_kernel_t* kernel, const cJSON* params,
                                      int* error_code, const char** error_msg) {
    (void)kernel;

    if (!s_lua_eval) {
        *error_code = -32603;
        *error_msg = "Lua eval not initialized";
        return NULL;
    }

    const cJSON* enabled = cJSON_GetObjectItemCaseSensitive(params, "enabled");
    if (!enabled || !cJSON_IsBool(enabled)) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: enabled (bool)";
        return NULL;
    }

    const char* val = cJSON_IsTrue(enabled) ? "true" : "false";
    char lua_buf[256];
    snprintf(lua_buf, sizeof(lua_buf),
        "local nav = _G._nav_grid\n"
        "if not nav then return '{\"error\":\"no nav grid\"}' end\n"
        "nav:set_debug_draw(%s)\n"
        "return '{\"ok\":true}'\n", val);

    s_lua_eval(lua_buf);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", 1);
    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_nav_tools(cd_mcp_server_t* server) {
    if (!server) return CD_ERR_NULL;

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "nav.bake", handle_nav_bake,
        "Trigger navgrid obstacle bake, optionally with explicit obstacle boxes.",
        "{\"type\":\"object\",\"properties\":{"
        "\"obstacles\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
        "\"min_x\":{\"type\":\"number\"},\"min_z\":{\"type\":\"number\"},"
        "\"max_x\":{\"type\":\"number\"},\"max_z\":{\"type\":\"number\"}"
        "}},\"description\":\"Obstacle AABB boxes\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "nav.find_path", handle_nav_find_path,
        "Find a path between two world positions on the navgrid.",
        "{\"type\":\"object\",\"properties\":{"
        "\"start_x\":{\"type\":\"number\"},\"start_z\":{\"type\":\"number\"},"
        "\"goal_x\":{\"type\":\"number\"},\"goal_z\":{\"type\":\"number\"},"
        "\"smooth\":{\"type\":\"boolean\",\"description\":\"Apply path smoothing (default true)\"}"
        "},\"required\":[\"start_x\",\"start_z\",\"goal_x\",\"goal_z\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "nav.debug_draw", handle_nav_debug_draw,
        "Enable or disable navgrid debug visualization.",
        "{\"type\":\"object\",\"properties\":{"
        "\"enabled\":{\"type\":\"boolean\"}"
        "},\"required\":[\"enabled\"]}");
    if (res != CD_OK) return res;

    return CD_OK;
}
