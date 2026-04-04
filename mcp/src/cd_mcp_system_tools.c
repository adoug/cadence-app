/* cd_mcp_system_tools.c - Cadence Engine MCP system tool handlers
 *
 * Implements:
 *   - system.ping           : Health check with uptime
 *   - system.health         : Detailed engine status
 *   - system.capabilities   : Engine capability discovery
 *   - system.plugin_status  : List all plugins with health state
 *   - system.plugin_restart : Unload and reload a faulted/healthy plugin
 *
 * All handlers follow the cd_mcp_tool_handler_t signature.
 * system.plugin_restart mutates plugin manager state (unload + reload).
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_tool_state.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_platform.h"
#include "cadence/cd_plugin.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Internal: Start time tracking for uptime computation
 *
 * PB-3: Moved from static globals into cd_kernel_get_mcp_tool_state(kernel)->system.
 * Uses a module-level fallback for standalone usage.
 * ============================================================================ */

static cd_mcp_system_state_t s_fallback_system;
static cd_mcp_system_state_t* s_sys = &s_fallback_system;

static cd_mcp_system_state_t* get_system_state(struct cd_kernel_t* kernel) {
    if (kernel && cd_kernel_get_mcp_tool_state(kernel)) {
        return &cd_kernel_get_mcp_tool_state(kernel)->system;
    }
    return s_sys;
}

/* ============================================================================
 * Internal: Provide string for capability flag
 * ============================================================================ */

static const char* cd_provides_string(uint32_t provides) {
    if (provides & CD_PROVIDES_RENDERER)   return "renderer";
    if (provides & CD_PROVIDES_PHYSICS)    return "physics";
    if (provides & CD_PROVIDES_AUDIO)      return "audio";
    if (provides & CD_PROVIDES_SCRIPTING)  return "scripting";
    if (provides & CD_PROVIDES_INPUT)      return "input";
    if (provides & CD_PROVIDES_UI)         return "ui";
    if (provides & CD_PROVIDES_ASSET_PIPE) return "asset_pipeline";
    if (provides & CD_PROVIDES_CUSTOM)     return "custom";
    return "unknown";
}

/* ============================================================================
 * Internal: Engine mode to string
 * ============================================================================ */

static const char* cd_mode_string(cd_engine_mode_t mode) {
    switch (mode) {
        case CD_MODE_EDIT:  return "edit";
        case CD_MODE_PLAY:  return "play";
        case CD_MODE_PAUSE: return "pause";
        default:            return "unknown";
    }
}

/* ============================================================================
 * system.ping handler
 *
 * Returns: { "status": "ok", "uptime_seconds": <double> }
 * ============================================================================ */

static cJSON* cd_mcp_handle_system_ping(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)params;

    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Kernel not available";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");

    /* Compute uptime from the stored start time */
    double now = cd_platform_time();
    cd_mcp_system_state_t* sys = get_system_state(kernel);
    double uptime = sys->start_time_set ? (now - sys->start_time) : 0.0;
    cJSON_AddNumberToObject(result, "uptime_seconds", uptime);

    return result;
}

/* ============================================================================
 * system.health handler
 *
 * Returns detailed engine status with mode, headless flag, frame count,
 * loaded plugins, scene info, and memory usage.
 * ============================================================================ */

static cJSON* cd_mcp_handle_system_health(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)params;

    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Kernel not available";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "mode", cd_mode_string(cd_kernel_get_mode(kernel)));
    cJSON_AddBoolToObject(result, "headless", cd_kernel_get_config(kernel)->headless);
    cJSON_AddNumberToObject(result, "frame_count", (double)cd_kernel_get_frame_count(kernel));

    /* FPS: compute from delta_time if available, otherwise default to 0 */
    double fps = 0.0;
    if (cd_kernel_get_delta_time(kernel) > 0.0) {
        fps = 1.0 / cd_kernel_get_delta_time(kernel);
    }
    cJSON_AddNumberToObject(result, "fps", fps);

    /* Plugins array — includes health state and fault info */
    cJSON* plugins_array = cJSON_CreateArray();
    if (plugins_array != NULL) {
        for (uint32_t i = 0; i < cd_kernel_get_plugins(kernel)->count; i++) {
            cd_loaded_plugin_t* lp = &cd_kernel_get_plugins(kernel)->plugins[i];
            if (lp->loaded && lp->plugin != NULL) {
                cJSON* plugin_obj = cJSON_CreateObject();
                if (plugin_obj != NULL) {
                    cJSON_AddStringToObject(plugin_obj, "name",
                        lp->plugin->info.name ? lp->plugin->info.name : "unknown");
                    cJSON_AddStringToObject(plugin_obj, "version",
                        lp->plugin->info.version ? lp->plugin->info.version : "0.0.0");
                    cJSON_AddStringToObject(plugin_obj, "provides",
                        cd_provides_string(lp->plugin->info.provides));
                    cJSON_AddStringToObject(plugin_obj, "health",
                        cd_plugin_health_string(lp->health));
                    cJSON_AddNumberToObject(plugin_obj, "fault_count",
                        (double)lp->fault_count);
                    if (lp->health != CD_PLUGIN_HEALTHY && lp->fault_msg[0]) {
                        cJSON_AddStringToObject(plugin_obj, "fault_message",
                            lp->fault_msg);
                    }
                    cJSON_AddItemToArray(plugins_array, plugin_obj);
                }
            }
        }
        cJSON_AddItemToObject(result, "plugins", plugins_array);
    }

    /* Scene: placeholder (scene subsystem not yet fully wired) */
    cJSON* scene_obj = cJSON_CreateObject();
    if (scene_obj != NULL) {
        cJSON_AddNumberToObject(scene_obj, "node_count", 0);
        cJSON_AddStringToObject(scene_obj, "name", "");
        cJSON_AddItemToObject(result, "scene", scene_obj);
    }

    /* Memory: report from kernel memory tracking */
    double memory_mb = (double)cd_kernel_get_memory(kernel)->total_allocated / (1024.0 * 1024.0);
    cJSON_AddNumberToObject(result, "memory_mb", memory_mb);

    return result;
}

/* ============================================================================
 * system.capabilities handler
 *
 * Returns what the engine can do based on loaded plugins.
 * ============================================================================ */

static cJSON* cd_mcp_handle_system_capabilities(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)params;

    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Kernel not available";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    /* Scan loaded plugins for capability flags */
    bool has_renderer  = false;
    bool has_physics   = false;
    bool has_audio     = false;
    bool has_scripting = false;
    const char* renderer_type  = "none";
    const char* physics_type   = "none";
    const char* audio_type     = "none";
    const char* scripting_type = "none";

    for (uint32_t i = 0; i < cd_kernel_get_plugins(kernel)->count; i++) {
        cd_loaded_plugin_t* lp = &cd_kernel_get_plugins(kernel)->plugins[i];
        if (!lp->loaded || lp->plugin == NULL) {
            continue;
        }

        uint32_t provides = lp->plugin->info.provides;
        const char* name  = lp->plugin->info.name ? lp->plugin->info.name : "unknown";

        if (provides & CD_PROVIDES_RENDERER) {
            has_renderer = true;
            /* Derive renderer type from plugin name */
            if (strstr(name, "null") != NULL) {
                renderer_type = "null";
            } else if (strstr(name, "opengl") != NULL) {
                renderer_type = "opengl";
            } else {
                renderer_type = name;
            }
        }
        if (provides & CD_PROVIDES_PHYSICS) {
            has_physics = true;
            if (strstr(name, "jolt") != NULL) {
                physics_type = "jolt";
            } else {
                physics_type = name;
            }
        }
        if (provides & CD_PROVIDES_AUDIO) {
            has_audio = true;
            audio_type = name;
        }
        if (provides & CD_PROVIDES_SCRIPTING) {
            has_scripting = true;
            if (strstr(name, "lua") != NULL) {
                scripting_type = "lua";
            } else {
                scripting_type = name;
            }
        }
    }

    /* Renderer capability */
    cJSON* renderer_obj = cJSON_CreateObject();
    if (renderer_obj != NULL) {
        cJSON_AddStringToObject(renderer_obj, "type", renderer_type);
        /* Null renderer supports JSON viewport capture; real renderers use image */
        const char* capture_mode = "none";
        if (has_renderer) {
            if (strcmp(renderer_type, "null") == 0) {
                capture_mode = "json";
            } else {
                capture_mode = "image";
            }
        }
        cJSON_AddStringToObject(renderer_obj, "viewport_capture", capture_mode);
        cJSON_AddItemToObject(result, "renderer", renderer_obj);
    }

    /* Physics capability */
    cJSON* physics_obj = cJSON_CreateObject();
    if (physics_obj != NULL) {
        cJSON_AddStringToObject(physics_obj, "type", physics_type);
        cJSON_AddBoolToObject(physics_obj, "available", has_physics);
        cJSON_AddItemToObject(result, "physics", physics_obj);
    }

    /* Audio capability */
    cJSON* audio_obj = cJSON_CreateObject();
    if (audio_obj != NULL) {
        cJSON_AddStringToObject(audio_obj, "type", audio_type);
        cJSON_AddBoolToObject(audio_obj, "available", has_audio);
        cJSON_AddItemToObject(result, "audio", audio_obj);
    }

    /* Scripting capability */
    cJSON* scripting_obj = cJSON_CreateObject();
    if (scripting_obj != NULL) {
        cJSON_AddStringToObject(scripting_obj, "type", scripting_type);
        cJSON_AddBoolToObject(scripting_obj, "hot_reload", has_scripting);
        cJSON_AddItemToObject(result, "scripting", scripting_obj);
    }

    /* MCP protocol version */
    cJSON_AddStringToObject(result, "mcp_version", "1.0");

    return result;
}

/* ============================================================================
 * system.plugin_status handler
 *
 * Returns: { "plugins": [ { "name": ..., "health": ..., ... }, ... ] }
 * Lists all loaded plugins with their health state, version, and fault info.
 * ============================================================================ */

static cJSON* cd_mcp_handle_system_plugin_status(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)params;

    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Kernel not available";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON* plugins_array = cJSON_CreateArray();
    if (plugins_array == NULL) {
        cJSON_Delete(result);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON array";
        return NULL;
    }

    for (uint32_t i = 0; i < cd_kernel_get_plugins(kernel)->count; i++) {
        cd_loaded_plugin_t* lp = &cd_kernel_get_plugins(kernel)->plugins[i];
        if (!lp->plugin) continue;

        cJSON* plugin_obj = cJSON_CreateObject();
        if (plugin_obj == NULL) continue;

        cJSON_AddStringToObject(plugin_obj, "name",
            lp->plugin->info.name ? lp->plugin->info.name : "unknown");
        cJSON_AddStringToObject(plugin_obj, "version",
            lp->plugin->info.version ? lp->plugin->info.version : "0.0.0");
        cJSON_AddStringToObject(plugin_obj, "provides",
            cd_provides_string(lp->plugin->info.provides));
        cJSON_AddBoolToObject(plugin_obj, "loaded", lp->loaded);
        cJSON_AddStringToObject(plugin_obj, "health",
            cd_plugin_health_string(lp->health));
        cJSON_AddNumberToObject(plugin_obj, "fault_count",
            (double)lp->fault_count);
        if (lp->fault_msg[0]) {
            cJSON_AddStringToObject(plugin_obj, "fault_message",
                lp->fault_msg);
        }
        if (lp->path[0]) {
            cJSON_AddStringToObject(plugin_obj, "path", lp->path);
        }

        cJSON_AddItemToArray(plugins_array, plugin_obj);
    }

    cJSON_AddItemToObject(result, "plugins", plugins_array);
    cJSON_AddNumberToObject(result, "total", (double)cd_kernel_get_plugins(kernel)->count);

    return result;
}

/* ============================================================================
 * system.plugin_restart handler
 *
 * Input:  { "plugin_name": "renderer_opengl" }
 * Output: { "status": "ok", "plugin_name": "...", "health": "healthy" }
 *
 * Unloads the named plugin (skipping on_unload if faulted), clears its
 * health/fault state, reloads the DLL, and calls on_load on the fresh
 * instance.  Returns an error if the plugin is not found or reload fails.
 * ============================================================================ */

static cJSON* cd_mcp_handle_system_plugin_restart(
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

    /* Extract required parameter: plugin_name */
    const cJSON* name_item = cJSON_GetObjectItemCaseSensitive(params, "plugin_name");
    if (name_item == NULL || !cJSON_IsString(name_item) ||
        name_item->valuestring == NULL || name_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing or invalid 'plugin_name' parameter";
        return NULL;
    }

    const char* plugin_name = name_item->valuestring;
    cd_plugin_mgr_t* mgr = cd_kernel_get_plugins(kernel);

    /* Find the plugin */
    cd_loaded_plugin_t* lp = cd_plugin_mgr_find(mgr, plugin_name);
    if (lp == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Plugin not found";
        return NULL;
    }

    /* Save the path before unloading (we need it for reload) */
    char saved_path[1024];
    memcpy(saved_path, lp->path, sizeof(saved_path));

    bool was_faulted = (lp->health == CD_PLUGIN_FAULTED);

    /* Step 1: Call on_unload if the plugin is not faulted.
     * If faulted, skip on_unload since the plugin code may be in a bad state. */
    if (!was_faulted && lp->loaded && lp->plugin && lp->plugin->on_unload) {
        lp->plugin->on_unload(kernel, lp->state);
    }

    /* Step 2: Free the old state and close the old handle */
    if (lp->state) {
        free(lp->state);
        lp->state = NULL;
    }
    if (lp->handle) {
        cd_dynlib_close(lp->handle);
        lp->handle = NULL;
    }

    /* Step 3: Clear health/fault state */
    lp->health = CD_PLUGIN_HEALTHY;
    lp->fault_count = 0;
    lp->fault_msg[0] = '\0';
    lp->loaded = false;
    lp->plugin = NULL;

    /* Step 4: Reload the DLL from the saved path */
    cd_dynlib_t new_handle = cd_dynlib_open(saved_path);
    if (!new_handle) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to reload plugin DLL";
        return NULL;
    }

    /* Step 5: Look up cd_plugin_entry */
    cd_plugin_entry_fn entry_fn =
        (cd_plugin_entry_fn)cd_dynlib_symbol(new_handle, "cd_plugin_entry");
    if (!entry_fn) {
        cd_dynlib_close(new_handle);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Reloaded DLL has no cd_plugin_entry";
        return NULL;
    }

    cd_plugin_t* new_plugin = entry_fn();
    if (!new_plugin) {
        cd_dynlib_close(new_handle);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "cd_plugin_entry returned NULL on reload";
        return NULL;
    }

    /* Step 6: Verify ABI */
    if (new_plugin->info.abi_version != CD_PLUGIN_ABI_VERSION) {
        cd_dynlib_close(new_handle);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "ABI version mismatch on reload";
        return NULL;
    }

    /* Step 7: Allocate new plugin state */
    void* new_state = NULL;
    if (new_plugin->state_size > 0) {
        new_state = calloc(1, new_plugin->state_size);
        if (!new_state) {
            cd_dynlib_close(new_handle);
            *error_code = CD_JSONRPC_INTERNAL_ERROR;
            *error_msg  = "Failed to allocate plugin state on reload";
            return NULL;
        }
    }

    /* Step 8: Call on_load */
    if (new_plugin->on_load) {
        cd_result_t load_res = new_plugin->on_load(kernel, new_state);
        if (load_res != CD_OK) {
            free(new_state);
            cd_dynlib_close(new_handle);
            *error_code = CD_JSONRPC_INTERNAL_ERROR;
            *error_msg  = "Plugin on_load failed during restart";
            return NULL;
        }
    }

    /* Step 9: Update the slot in place */
    lp->handle     = new_handle;
    lp->plugin     = new_plugin;
    lp->state      = new_state;
    lp->loaded     = true;
    lp->health     = CD_PLUGIN_HEALTHY;
    lp->file_mtime = cd_fs_modified_time(saved_path);

    /* Build success response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "plugin_name", plugin_name);
    cJSON_AddStringToObject(result, "health",
        cd_plugin_health_string(lp->health));
    cJSON_AddBoolToObject(result, "was_faulted", was_faulted);

    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_system_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    /* Capture start time on first registration */
    if (!s_sys->start_time_set) {
        s_sys->start_time = cd_platform_time();
        s_sys->start_time_set = true;
    }

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "system.ping",
        cd_mcp_handle_system_ping,
        "Health check returning engine uptime",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "system.health",
        cd_mcp_handle_system_health,
        "Detailed engine status including mode, FPS, plugins, and memory",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "system.capabilities",
        cd_mcp_handle_system_capabilities,
        "Discover engine capabilities based on loaded plugins",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "system.plugin_status",
        cd_mcp_handle_system_plugin_status,
        "List all plugins with health state and fault info",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "system.plugin_restart",
        cd_mcp_handle_system_plugin_restart,
        "Unload and reload a plugin by name",
        "{\"type\":\"object\",\"properties\":{"
        "\"plugin_name\":{\"type\":\"string\",\"description\":\"Name of the plugin to restart\"}"
        "},\"required\":[\"plugin_name\"]}");
    if (res != CD_OK) return res;

    return CD_OK;
}
