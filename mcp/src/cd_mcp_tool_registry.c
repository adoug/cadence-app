/* cd_mcp_tool_registry.c - Centralized MCP tool module registration
 *
 * Provides a static table of all built-in tool modules and a single
 * function to register them all. Adding a new tool module requires only
 * adding one entry to g_builtin_modules[] below.
 *
 * Also provides cd_mcp_register_tool_module() for plugins to register
 * their own MCP tools dynamically at runtime.
 */

#include "cadence/cd_mcp_tools.h"

#include <stddef.h>
#include <string.h>

/* ============================================================================
 * Built-in tool module table
 *
 * Each entry maps a human-readable name to the registration function.
 * Terminated by a sentinel with NULL fields.
 * ============================================================================ */

static const cd_mcp_tool_module_t g_builtin_modules[] = {
    { "system",       cd_mcp_register_system_tools },
    { "scene",        cd_mcp_register_scene_tools },
    { "node",         cd_mcp_register_node_tools },
    { "prop",         cd_mcp_register_prop_tools },
    { "tag",          cd_mcp_register_tag_tools },
    { "txn",          cd_mcp_register_txn_tools },
    { "viewport",     cd_mcp_register_viewport_tools },
    { "asset",        cd_mcp_register_asset_tools },
    { "script",       cd_mcp_register_script_tools },
    { "observe",      cd_mcp_register_observe_tools },
    { "input",        cd_mcp_register_input_tools },
    { "play",         cd_mcp_register_play_tools },
    { "scene_save",   cd_mcp_register_scene_save_tools },
    { "file",         cd_mcp_register_file_tools },
    { "prefab",       cd_mcp_register_prefab_tools },
    { "gamespec",     cd_mcp_register_gamespec_tools },
    { "gamespec_plan", cd_mcp_register_gamespec_plan_tools },
    { "compose",      cd_mcp_register_compose_tools },
    { "build",        cd_mcp_register_build_tools },
    { "project",      cd_mcp_register_project_tools },
    { "asset_gen",    cd_mcp_register_asset_gen_tools },
    { "log",          cd_mcp_register_log_tools },
    { "net",          cd_mcp_register_net_tools },
    { "ui",           cd_mcp_register_ui_tools },
    { "sprite_anim",  cd_mcp_register_sprite_anim_tools },
    { "anim",         cd_mcp_register_anim_tools },
    { "camera",       cd_mcp_register_camera_tools },
    { "debug",        cd_mcp_register_debug_tools },
    { "terrain",      cd_mcp_register_terrain_tools },
    { "lock",         cd_mcp_register_lock_tools },
    { "ai",           cd_mcp_register_ai_tools },
    { "asset_pipeline", cd_mcp_register_asset_pipeline_tools },
    { "material",       cd_mcp_register_material_tools },
    { "cook",           cd_mcp_register_cook_tools },
    { "usd",            cd_mcp_register_usd_tools },
    { "audio_bus",      cd_mcp_register_audio_bus_tools },
    { "profiler",       cd_mcp_register_profiler_tools },
    { "scene_stream",   cd_mcp_register_scene_stream_tools },
    { "edit",           cd_mcp_register_edit_tools },
    { "nav",            cd_mcp_register_nav_tools },
    { "dialogue",       cd_mcp_register_dialogue_tools },
    { "inventory",      cd_mcp_register_inventory_tools },
    { "mesh",           cd_mcp_register_mesh_tools },
    { "scene_validate", cd_mcp_register_scene_validate_tools },
    { NULL, NULL }  /* sentinel */
};

/* ============================================================================
 * Dynamic module list for plugin-registered tool modules
 * ============================================================================ */

#define CD_MCP_MAX_DYNAMIC_MODULES 64

static cd_mcp_tool_module_t g_dynamic_modules[CD_MCP_MAX_DYNAMIC_MODULES];
static uint32_t g_dynamic_module_count = 0;

/* ============================================================================
 * Public API
 * ============================================================================ */

cd_result_t cd_mcp_register_all_tools(cd_mcp_server_t* server) {
    if (!server) return CD_ERR_NULL;

    /* Register all built-in modules */
    for (int i = 0; g_builtin_modules[i].register_fn != NULL; i++) {
        cd_result_t r = g_builtin_modules[i].register_fn(server);
        if (r != CD_OK) return r;
    }

    /* Register any dynamically-added plugin modules */
    for (uint32_t i = 0; i < g_dynamic_module_count; i++) {
        if (g_dynamic_modules[i].register_fn) {
            cd_result_t r = g_dynamic_modules[i].register_fn(server);
            if (r != CD_OK) return r;
        }
    }

    return CD_OK;
}

cd_result_t cd_mcp_register_tool_module(const cd_mcp_tool_module_t* module) {
    if (!module || !module->register_fn) return CD_ERR_NULL;
    if (g_dynamic_module_count >= CD_MCP_MAX_DYNAMIC_MODULES) {
        return CD_ERR_FULL;
    }

    g_dynamic_modules[g_dynamic_module_count] = *module;
    g_dynamic_module_count++;
    return CD_OK;
}

uint32_t cd_mcp_get_tool_module_count(void) {
    /* Count built-in modules (excluding sentinel) */
    uint32_t count = 0;
    for (int i = 0; g_builtin_modules[i].register_fn != NULL; i++) {
        count++;
    }
    return count + g_dynamic_module_count;
}
