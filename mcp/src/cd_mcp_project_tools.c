#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
/* cd_mcp_project_tools.c - Cadence Engine MCP project tool handlers
 *
 * Task 11.5: project.create + project.open + project.info MCP tools
 *
 * Implements:
 *   - project.create : Create a new project directory with scaffolding
 *   - project.open   : Set the kernel's active project path
 *   - project.info   : Return info about the current project
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_tool_state.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_asset_db.h"
#include "cadence/cd_project.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

/* ============================================================================
 * Helper: create directory (ignore if exists)
 * ============================================================================ */

static void ensure_dir(const char* path) {
    MKDIR(path);
}

/* ============================================================================
 * Helper: write a small file
 * ============================================================================ */

static int write_file(const char* path, const char* content) {
    FILE* f = fopen(path, "wb");
    if (f == NULL) return -1;
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return 0;
}

/* ============================================================================
 * Helper: check if a path exists (file or directory)
 * ============================================================================ */

static bool path_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f != NULL) {
        fclose(f);
        return true;
    }
    return false;
}

/* ============================================================================
 * project.create handler
 *
 * Input:  { "path": "<directory>", "name": "<project name>" }
 * Output: { "path": "...", "name": "...", "created": true }
 *
 * Creates:
 *   <path>/
 *     scenes/main.toml
 *     scripts/
 *     assets/
 * ============================================================================ */

static cJSON* cd_mcp_handle_project_create(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel;

    if (params == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing params";
        return NULL;
    }

    /* Require path */
    const cJSON* path_item = cJSON_GetObjectItemCaseSensitive(params, "path");
    if (path_item == NULL || !cJSON_IsString(path_item) ||
        path_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing required param: path";
        return NULL;
    }
    const char* proj_path = path_item->valuestring;

    /* Optional name (default to directory name) */
    const char* proj_name = proj_path;
    const cJSON* name_item = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (name_item != NULL && cJSON_IsString(name_item) &&
        name_item->valuestring[0] != '\0') {
        proj_name = name_item->valuestring;
    }

    /* Create project directory structure */
    ensure_dir(proj_path);

    char sub[1024];
    snprintf(sub, sizeof(sub), "%s/scenes", proj_path);
    ensure_dir(sub);

    snprintf(sub, sizeof(sub), "%s/scripts", proj_path);
    ensure_dir(sub);

    snprintf(sub, sizeof(sub), "%s/assets", proj_path);
    ensure_dir(sub);

    /* Create default scene */
    char scene_path[1024];
    snprintf(scene_path, sizeof(scene_path), "%s/scenes/main.toml", proj_path);
    if (!path_exists(scene_path)) {
        char scene_content[512];
        snprintf(scene_content, sizeof(scene_content),
            "[scene]\n"
            "name = \"%s\"\n"
            "\n"
            "[[nodes]]\n"
            "name = \"Root\"\n"
            "type = \"Node3D\"\n",
            proj_name);
        write_file(scene_path, scene_content);
    }

    /* Build response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "path", proj_path);
    cJSON_AddStringToObject(result, "name", proj_name);
    cJSON_AddBoolToObject(result, "created", true);

    return result;
}

/* ============================================================================
 * project.open handler
 *
 * Input:  { "path": "<directory>" }
 * Output: { "path": "...", "opened": true }
 *
 * Sets the kernel's project_path to the given directory.
 * Scans the asset database if available.
 * ============================================================================ */

/* PB-3: Project path buffer now in cd_kernel_get_mcp_tool_state(kernel)->project.
 * Fallback for when tool state is not available. */
static cd_mcp_project_state_t s_fallback_project;

static char* get_project_path_buf(struct cd_kernel_t* kernel) {
    if (kernel && cd_kernel_get_mcp_tool_state(kernel)) {
        return cd_kernel_get_mcp_tool_state(kernel)->project.project_path;
    }
    return s_fallback_project.project_path;
}

static cJSON* cd_mcp_handle_project_open(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "No kernel available";
        return NULL;
    }

    if (params == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing params";
        return NULL;
    }

    const cJSON* path_item = cJSON_GetObjectItemCaseSensitive(params, "path");
    if (path_item == NULL || !cJSON_IsString(path_item) ||
        path_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing required param: path";
        return NULL;
    }

    /* Copy path to persistent buffer so it survives cJSON cleanup */
    char* proj_path = get_project_path_buf(kernel);
    snprintf(proj_path, 1024, "%s", path_item->valuestring);

    /* Update kernel project path */
    cd_kernel_set_config_project_path(kernel, proj_path);

    /* Re-scan asset database if present */
    if (cd_kernel_get_asset_db(kernel) != NULL) {
        cd_asset_db_shutdown(cd_kernel_get_asset_db(kernel));
        cd_asset_db_init(cd_kernel_get_asset_db(kernel));
        cd_asset_db_scan(cd_kernel_get_asset_db(kernel), proj_path);
    }

    /* Build response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "path", proj_path);
    cJSON_AddBoolToObject(result, "opened", true);

    return result;
}

/* ============================================================================
 * project.info handler
 *
 * Input:  {} (no params required)
 * Output: { "path": "...", "has_scene": bool, "asset_count": N, "mode": "..." }
 * ============================================================================ */

static cJSON* cd_mcp_handle_project_info(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)params;

    if (kernel == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "No kernel available";
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    const char* proj = cd_kernel_get_config(kernel)->project_path;
    cJSON_AddStringToObject(result, "path", proj ? proj : ".");

    cJSON_AddBoolToObject(result, "has_scene", cd_kernel_get_scene(kernel) != NULL);

    uint32_t asset_count = 0;
    if (cd_kernel_get_asset_db(kernel) != NULL) {
        asset_count = cd_kernel_get_asset_db(kernel)->count;
    }
    cJSON_AddNumberToObject(result, "asset_count", (double)asset_count);

    const char* mode_str = "edit";
    if (cd_kernel_get_mode(kernel) == CD_MODE_PLAY) mode_str = "play";
    else if (cd_kernel_get_mode(kernel) == CD_MODE_PAUSE) mode_str = "pause";
    cJSON_AddStringToObject(result, "mode", mode_str);

    cJSON_AddBoolToObject(result, "headless", cd_kernel_get_config(kernel)->headless);

    /* S2.4: Include full project.toml data if loaded */
    if (cd_kernel_get_project(kernel)->loaded) {
        /* [game] section */
        cJSON* game = cJSON_CreateObject();
        if (game) {
            if (cd_kernel_get_project(kernel)->game.name[0])
                cJSON_AddStringToObject(game, "name", cd_kernel_get_project(kernel)->game.name);
            if (cd_kernel_get_project(kernel)->game.version[0])
                cJSON_AddStringToObject(game, "version", cd_kernel_get_project(kernel)->game.version);
            if (cd_kernel_get_project(kernel)->game.description[0])
                cJSON_AddStringToObject(game, "description", cd_kernel_get_project(kernel)->game.description);
            if (cd_kernel_get_project(kernel)->game.author[0])
                cJSON_AddStringToObject(game, "author", cd_kernel_get_project(kernel)->game.author);
            if (cd_kernel_get_project(kernel)->game.entry_scene[0])
                cJSON_AddStringToObject(game, "entry_scene", cd_kernel_get_project(kernel)->game.entry_scene);
            cJSON_AddItemToObject(result, "game", game);
        }

        /* [display] section */
        cJSON* display = cJSON_CreateObject();
        if (display) {
            cJSON_AddNumberToObject(display, "width", (double)cd_kernel_get_project(kernel)->display.width);
            cJSON_AddNumberToObject(display, "height", (double)cd_kernel_get_project(kernel)->display.height);
            cJSON_AddBoolToObject(display, "fullscreen", cd_kernel_get_project(kernel)->display.fullscreen);
            cJSON_AddBoolToObject(display, "vsync", cd_kernel_get_project(kernel)->display.vsync);
            const char* title = cd_project_display_title(cd_kernel_get_project(kernel));
            cJSON_AddStringToObject(display, "title", title);
            cJSON_AddItemToObject(result, "display", display);
        }

        /* [build] section */
        if (cd_kernel_get_project(kernel)->build.export_dir[0] ||
            cd_kernel_get_project(kernel)->build.target_platform_count > 0) {
            cJSON* build = cJSON_CreateObject();
            if (build) {
                if (cd_kernel_get_project(kernel)->build.export_dir[0])
                    cJSON_AddStringToObject(build, "export_dir", cd_kernel_get_project(kernel)->build.export_dir);
                if (cd_kernel_get_project(kernel)->build.target_platform_count > 0) {
                    cJSON* platforms = cJSON_CreateArray();
                    if (platforms) {
                        for (uint32_t i = 0; i < cd_kernel_get_project(kernel)->build.target_platform_count; i++) {
                            cJSON_AddItemToArray(platforms,
                                cJSON_CreateString(cd_kernel_get_project(kernel)->build.target_platforms[i]));
                        }
                        cJSON_AddItemToObject(build, "target_platforms", platforms);
                    }
                }
                cJSON_AddItemToObject(result, "build", build);
            }
        }

        /* [plugins] section */
        if (cd_kernel_get_project(kernel)->plugins.load_count > 0 ||
            cd_kernel_get_project(kernel)->plugins.order_count > 0) {
            cJSON* plugins = cJSON_CreateObject();
            if (plugins) {
                if (cd_kernel_get_project(kernel)->plugins.load_count > 0) {
                    cJSON* load_arr = cJSON_CreateArray();
                    if (load_arr) {
                        for (uint32_t i = 0; i < cd_kernel_get_project(kernel)->plugins.load_count; i++) {
                            cJSON_AddItemToArray(load_arr,
                                cJSON_CreateString(cd_kernel_get_project(kernel)->plugins.load[i]));
                        }
                        cJSON_AddItemToObject(plugins, "load", load_arr);
                    }
                }
                if (cd_kernel_get_project(kernel)->plugins.order_count > 0) {
                    cJSON* order_arr = cJSON_CreateArray();
                    if (order_arr) {
                        for (uint32_t i = 0; i < cd_kernel_get_project(kernel)->plugins.order_count; i++) {
                            cJSON_AddItemToArray(order_arr,
                                cJSON_CreateString(cd_kernel_get_project(kernel)->plugins.order[i]));
                        }
                        cJSON_AddItemToObject(plugins, "order", order_arr);
                    }
                }
                cJSON_AddItemToObject(result, "plugins", plugins);
            }
        }
    }

    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_project_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "project.create",
        cd_mcp_handle_project_create,
        "Create a new project directory with default scene and folder scaffolding",
        "{\"type\":\"object\",\"properties\":{"
        "\"path\":{\"type\":\"string\",\"description\":\"Directory path for the new project\"},"
        "\"name\":{\"type\":\"string\",\"description\":\"Project name (defaults to directory name)\"}"
        "},\"required\":[\"path\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "project.open",
        cd_mcp_handle_project_open,
        "Set the active project path and re-scan assets",
        "{\"type\":\"object\",\"properties\":{"
        "\"path\":{\"type\":\"string\",\"description\":\"Directory path of the project to open\"}"
        "},\"required\":[\"path\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "project.info",
        cd_mcp_handle_project_info,
        "Return information about the current project including config and asset counts",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    return CD_OK;
}
