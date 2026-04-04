#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
/* cd_mcp_build_tools.c - Cadence Engine MCP build tool handlers
 *
 * Task 11.4: build.config + build.export MCP tools
 *
 * Implements:
 *   - build.config : Return project build configuration (project path, etc.)
 *   - build.export : Pack project assets into PAK files in an output directory
 *
 * build.export creates:
 *   <output>/scenes.pak    — all .toml scene files
 *   <output>/scripts.pak   — all .lua script files
 *   <output>/assets.pak    — all other asset files (meshes, textures, audio)
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_pak.h"
#include "cadence/cd_asset_db.h"
#include "cadence/cd_project.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif

/* ============================================================================
 * build.config handler
 *
 * Input:  {} (no params required)
 * Output: { "project_path": "...", "headless": bool, "mode": "edit"|"play" }
 * ============================================================================ */

static cJSON* cd_mcp_handle_build_config(
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

    cJSON_AddStringToObject(result, "project_path",
                             cd_kernel_get_config(kernel)->project_path ?
                             cd_kernel_get_config(kernel)->project_path : ".");
    cJSON_AddBoolToObject(result, "headless", cd_kernel_get_config(kernel)->headless);

    const char* mode_str = "edit";
    if (cd_kernel_get_mode(kernel) == CD_MODE_PLAY) mode_str = "play";
    else if (cd_kernel_get_mode(kernel) == CD_MODE_PAUSE) mode_str = "pause";
    cJSON_AddStringToObject(result, "mode", mode_str);

    cJSON_AddStringToObject(result, "engine_version", "0.1.0");

    /* Report available asset counts if asset_db exists */
    if (cd_kernel_get_asset_db(kernel) != NULL) {
        cJSON_AddNumberToObject(result, "asset_count",
                                 (double)cd_kernel_get_asset_db(kernel)->count);
    }

    return result;
}

/* ============================================================================
 * Helpers for build.export
 * ============================================================================ */

#ifdef _WIN32
#include <direct.h>
#define MKDIR_P(p) _mkdir(p)
#define POPEN(cmd, mode) _popen(cmd, mode)
#define PCLOSE(fp) _pclose(fp)
#else
#include <sys/stat.h>
#define MKDIR_P(p) mkdir(p, 0755)
#define POPEN(cmd, mode) popen(cmd, mode)
#define PCLOSE(fp) pclose(fp)
#endif

/** Copy a file using C stdio (portable, no shell escaping issues). */
static bool copy_file(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (!in) return false;
    FILE* out = fopen(dst, "wb");
    if (!out) { fclose(in); return false; }

    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in); fclose(out); return false;
        }
    }
    fclose(in);
    fclose(out);
    return true;
}

/** Create directory and parents (ignore if exists). */
static void ensure_dir(const char* path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            MKDIR_P(tmp);
            *p = saved;
        }
    }
    MKDIR_P(tmp);
}

/**
 * Recursively list files in a directory and add matching ones to a PAK writer.
 * Uses the asset_db to find files by kind rather than manual directory traversal.
 */
static uint32_t pack_assets_by_kind(cd_pak_writer_t* writer,
                                     const cd_asset_db_t* db,
                                     cd_asset_kind_t kind) {
    uint32_t packed = 0;
    if (db == NULL) return 0;

    for (uint32_t i = 0; i < db->count; i++) {
        const cd_asset_entry_t* entry = &db->entries[i];
        if (kind != CD_ASSET_UNKNOWN && entry->kind != kind) continue;

        cd_result_t r = cd_pak_writer_add_file(writer, entry->uri,
                                                entry->abs_path);
        if (r == CD_OK) {
            packed++;
        }
    }
    return packed;
}

/* ============================================================================
 * build.export handler (P1-2: Build Export Platform Completeness)
 *
 * Input:  { "output": "<directory>",    (optional, default: "builds/export")
 *           "strip_debug": bool }       (optional, default: false)
 *
 * Output: {
 *   "output_dir": "...",
 *   "total_packed": N,
 *   "paks":    [ { "name": "...", "entries": N }, ... ],
 *   "runtime": { "copied": bool, "file": "cadence-runtime.exe" },
 *   "plugins": [ { "name": "...", "copied": bool }, ... ],
 *   "project_toml": bool,
 *   "launch_script": "...",
 *   "stripped": bool
 * }
 * ============================================================================ */

/** Try to copy a plugin library from search dirs to the export plugins dir.
 *  Returns true if the file was found and copied. */
static bool copy_plugin_lib(const char* plugin_name,
                             const char* const* search_dirs,
                             const char* dll_prefix,
                             const char* dll_ext,
                             const char* plugins_dir) {
    for (int d = 0; search_dirs[d] != NULL; d++) {
        char src_lib[1024], dst_lib[1024];
        snprintf(src_lib, sizeof(src_lib), "%s/%s%s%s",
                 search_dirs[d], dll_prefix, plugin_name, dll_ext);
        snprintf(dst_lib, sizeof(dst_lib), "%s/%s%s%s",
                 plugins_dir, dll_prefix, plugin_name, dll_ext);
        if (copy_file(src_lib, dst_lib)) return true;
    }
    return false;
}

/** Write a platform-appropriate launch script into the output directory.
 *  Returns true on success. */
static bool write_launch_script(const char* output_dir,
                                 const char* runtime_name) {
    char script_path[1024];
    FILE* fp;

#ifdef _WIN32
    snprintf(script_path, sizeof(script_path), "%s/launch.bat", output_dir);
    fp = fopen(script_path, "w");
    if (fp == NULL) return false;
    fprintf(fp, "@echo off\r\n");
    fprintf(fp, "cd /d \"%%~dp0\"\r\n");
    fprintf(fp, "start \"\" \"%s\" --project . %%*\r\n", runtime_name);
    fclose(fp);
#else
    snprintf(script_path, sizeof(script_path), "%s/launch.sh", output_dir);
    fp = fopen(script_path, "w");
    if (fp == NULL) return false;
    fprintf(fp, "#!/bin/sh\n");
    fprintf(fp, "cd \"$(dirname \"$0\")\"\n");
    fprintf(fp, "export LD_LIBRARY_PATH=\"./plugins:$LD_LIBRARY_PATH\"\n");
    fprintf(fp, "./%s --project . \"$@\"\n", runtime_name);
    fclose(fp);
    /* Make executable */
    chmod(script_path, 0755);
#endif
    return true;
}

/** Run strip on a binary to remove debug symbols (non-Windows only). */
static void strip_binary(const char* path) {
#ifndef _WIN32
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "strip \"%s\" 2>/dev/null", path);
    (void)system(cmd);
#else
    (void)path;
#endif
}

static cJSON* cd_mcp_handle_build_export(
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

    /* ---- Parse parameters ---- */
    const char* output_dir = "builds/export";
    bool strip_debug = false;

    if (params != NULL) {
        const cJSON* out_item = cJSON_GetObjectItemCaseSensitive(params, "output");
        if (out_item != NULL && cJSON_IsString(out_item) &&
            out_item->valuestring[0] != '\0') {
            output_dir = out_item->valuestring;
        }

        const cJSON* strip_item = cJSON_GetObjectItemCaseSensitive(params, "strip_debug");
        if (strip_item != NULL && cJSON_IsBool(strip_item)) {
            strip_debug = cJSON_IsTrue(strip_item);
        }
    }

    /* ---- Scan project assets if not already done ---- */
    cd_asset_db_t local_db;
    cd_asset_db_t* db = cd_kernel_get_asset_db(kernel);
    bool local_db_used = false;

    if (db == NULL || db->count == 0) {
        cd_asset_db_init(&local_db);
        cd_result_t scan_res = cd_asset_db_scan(&local_db,
            cd_kernel_get_config(kernel)->project_path ? cd_kernel_get_config(kernel)->project_path : ".");
        if (scan_res != CD_OK) {
            cd_asset_db_shutdown(&local_db);
            *error_code = CD_JSONRPC_INTERNAL_ERROR;
            *error_msg  = "Failed to scan project assets";
            return NULL;
        }
        db = &local_db;
        local_db_used = true;
    }

    /* ---- Create output directory ---- */
    ensure_dir(output_dir);

    /* ---- Build PAK files ---- */
    char scenes_pak[1024], scripts_pak[1024], assets_pak[1024];
    snprintf(scenes_pak,  sizeof(scenes_pak),  "%s/scenes.pak",  output_dir);
    snprintf(scripts_pak, sizeof(scripts_pak), "%s/scripts.pak", output_dir);
    snprintf(assets_pak,  sizeof(assets_pak),  "%s/assets.pak",  output_dir);

    cJSON* paks_arr = cJSON_CreateArray();
    uint32_t total_packed = 0;

    /* Pack scenes (.toml) */
    {
        cd_pak_writer_t writer;
        cd_result_t r = cd_pak_writer_init(&writer, scenes_pak);
        if (r == CD_OK) {
            uint32_t n = pack_assets_by_kind(&writer, db, CD_ASSET_SCENE);
            cd_pak_writer_finish(&writer);
            total_packed += n;

            cJSON* info = cJSON_CreateObject();
            cJSON_AddStringToObject(info, "name", "scenes.pak");
            cJSON_AddNumberToObject(info, "entries", (double)n);
            cJSON_AddItemToArray(paks_arr, info);
        }
    }

    /* Pack scripts (.lua) */
    {
        cd_pak_writer_t writer;
        cd_result_t r = cd_pak_writer_init(&writer, scripts_pak);
        if (r == CD_OK) {
            uint32_t n = pack_assets_by_kind(&writer, db, CD_ASSET_SCRIPT);
            cd_pak_writer_finish(&writer);
            total_packed += n;

            cJSON* info = cJSON_CreateObject();
            cJSON_AddStringToObject(info, "name", "scripts.pak");
            cJSON_AddNumberToObject(info, "entries", (double)n);
            cJSON_AddItemToArray(paks_arr, info);
        }
    }

    /* Pack other assets (meshes, textures, audio) */
    {
        cd_pak_writer_t writer;
        cd_result_t r = cd_pak_writer_init(&writer, assets_pak);
        if (r == CD_OK) {
            uint32_t n = 0;
            n += pack_assets_by_kind(&writer, db, CD_ASSET_MESH);
            n += pack_assets_by_kind(&writer, db, CD_ASSET_TEXTURE);
            n += pack_assets_by_kind(&writer, db, CD_ASSET_AUDIO);
            cd_pak_writer_finish(&writer);
            total_packed += n;

            cJSON* info = cJSON_CreateObject();
            cJSON_AddStringToObject(info, "name", "assets.pak");
            cJSON_AddNumberToObject(info, "entries", (double)n);
            cJSON_AddItemToArray(paks_arr, info);
        }
    }

    if (local_db_used) {
        cd_asset_db_shutdown(&local_db);
    }

    /* ========================================================================
     * P1-2: Copy runtime binary, plugins, project.toml, launch script
     * ======================================================================== */

    /* ---- Platform-specific names ---- */
#ifdef _WIN32
    const char* runtime_name = "cadence-runtime.exe";
    const char* dll_ext      = ".dll";
    const char* dll_prefix   = "";
#elif defined(__APPLE__)
    const char* runtime_name = "cadence-runtime";
    const char* dll_ext      = ".dylib";
    const char* dll_prefix   = "lib";
#else  /* Linux / other POSIX */
    const char* runtime_name = "cadence-runtime";
    const char* dll_ext      = ".so";
    const char* dll_prefix   = "lib";
#endif

    /* ---- Search directories for runtime binary ---- */
    const char* cd_runtime_search_dirs[] = {
        ".", "build/bin/Debug", "build/bin/Release", "build/bin",
#ifdef _WIN32
        "bin",
#elif defined(__APPLE__)
        "/usr/local/bin",
        "/opt/homebrew/bin",
#else
        "/usr/bin",
        "/usr/local/bin",
#endif
        NULL
    };

    /* ---- Search directories for plugin libraries ---- */
    const char* cd_lib_search_dirs[] = {
        ".", "build/bin/Debug", "build/bin/Release", "build/bin",
        "./lib", "./plugins",
#ifdef _WIN32
        "bin",
#elif defined(__APPLE__)
        "/usr/local/lib/cadence",
        "/opt/homebrew/lib/cadence",
        "../Frameworks",
#else
        "/usr/lib/cadence",
        "/usr/local/lib/cadence",
        "/usr/lib64/cadence",
#endif
        NULL
    };

    /* ---- Copy runtime binary ---- */
    char dst_runtime[1024];
    snprintf(dst_runtime, sizeof(dst_runtime), "%s/%s", output_dir, runtime_name);
    bool copied_runtime = false;
    for (int d = 0; cd_runtime_search_dirs[d] != NULL; d++) {
        char src_runtime[1024];
        snprintf(src_runtime, sizeof(src_runtime), "%s/%s",
                 cd_runtime_search_dirs[d], runtime_name);
        if (copy_file(src_runtime, dst_runtime)) {
            copied_runtime = true;
            break;
        }
    }

    /* ---- Copy plugin libraries (BL-33: project-config-aware) ----
     * If [plugins].load is configured, copy exactly those plugins.
     * Otherwise fall back to loaded + well-known list approach. */
    char plugins_dir[1024];
    snprintf(plugins_dir, sizeof(plugins_dir), "%s/plugins", output_dir);
    ensure_dir(plugins_dir);

#define CD_EXPORT_MAX_PLUGINS 64
    struct { char name[256]; bool copied; } plugin_results[CD_EXPORT_MAX_PLUGINS];
    int plugin_result_count = 0;

    /* Names of plugins that were actually copied (for [plugins] section) */
    char export_plugin_names[CD_EXPORT_MAX_PLUGINS][256];
    int export_plugin_count = 0;

    bool use_project_config = (cd_kernel_get_project(kernel)->loaded &&
                                cd_kernel_get_project(kernel)->plugins.load_count > 0);

    if (use_project_config) {
        /* ---- Config-driven: copy only listed plugins ---- */
        for (uint32_t i = 0; i < cd_kernel_get_project(kernel)->plugins.load_count &&
                              plugin_result_count < CD_EXPORT_MAX_PLUGINS; i++) {
            const char* pname = cd_kernel_get_project(kernel)->plugins.load[i];
            if (pname[0] == '\0') continue;

            /* Skip editor */
            if (strcmp(pname, "editor") == 0) continue;

            bool ok = copy_plugin_lib(pname, cd_lib_search_dirs,
                                      dll_prefix, dll_ext, plugins_dir);

            /* Try loading from kernel's loaded plugin path as fallback */
            if (!ok) {
                for (uint32_t j = 0; j < cd_kernel_get_plugins(kernel)->count; j++) {
                    const cd_loaded_plugin_t* lp = &cd_kernel_get_plugins(kernel)->plugins[j];
                    if (lp->plugin && lp->plugin->info.name &&
                        strcmp(lp->plugin->info.name, pname) == 0 &&
                        lp->path[0] != '\0') {
                        char dst_lib[1024];
                        snprintf(dst_lib, sizeof(dst_lib), "%s/%s%s%s",
                                 plugins_dir, dll_prefix, pname, dll_ext);
                        ok = copy_file(lp->path, dst_lib);
                        break;
                    }
                }
            }

            snprintf(plugin_results[plugin_result_count].name,
                     sizeof(plugin_results[plugin_result_count].name),
                     "%s", pname);
            plugin_results[plugin_result_count].copied = ok;
            plugin_result_count++;

            if (ok && export_plugin_count < CD_EXPORT_MAX_PLUGINS) {
                snprintf(export_plugin_names[export_plugin_count],
                         sizeof(export_plugin_names[export_plugin_count]),
                         "%s", pname);
                export_plugin_count++;
            }
        }

        /* Also copy the active renderer (auto-detected, not in the list) */
        for (uint32_t i = 0; i < cd_kernel_get_plugins(kernel)->count &&
                              plugin_result_count < CD_EXPORT_MAX_PLUGINS; i++) {
            const cd_loaded_plugin_t* lp = &cd_kernel_get_plugins(kernel)->plugins[i];
            if (!lp->plugin || !lp->plugin->info.name) continue;
            const char* pname = lp->plugin->info.name;
            if (strncmp(pname, "renderer_", 9) != 0) continue;

            /* Check if already in list */
            bool already = false;
            for (int j = 0; j < plugin_result_count; j++) {
                if (strcmp(plugin_results[j].name, pname) == 0) {
                    already = true;
                    break;
                }
            }
            if (already) continue;

            bool ok = copy_plugin_lib(pname, cd_lib_search_dirs,
                                      dll_prefix, dll_ext, plugins_dir);
            if (!ok && lp->path[0] != '\0') {
                char dst_lib[1024];
                snprintf(dst_lib, sizeof(dst_lib), "%s/%s%s%s",
                         plugins_dir, dll_prefix, pname, dll_ext);
                ok = copy_file(lp->path, dst_lib);
            }

            snprintf(plugin_results[plugin_result_count].name,
                     sizeof(plugin_results[plugin_result_count].name),
                     "%s", pname);
            plugin_results[plugin_result_count].copied = ok;
            plugin_result_count++;

            if (ok && export_plugin_count < CD_EXPORT_MAX_PLUGINS) {
                snprintf(export_plugin_names[export_plugin_count],
                         sizeof(export_plugin_names[export_plugin_count]),
                         "%s", pname);
                export_plugin_count++;
            }
        }
        /* ---- Copy transitive DLL dependencies (BL-35) ----
         * Some plugins (e.g. scripting_lua) link against other plugin DLLs
         * at build time.  If those DLLs aren't in the export plugins/ dir,
         * Windows LoadLibrary fails.  Copy ALL loaded plugins + well-known
         * dependency DLLs that aren't already in the export set. */
        static const char* s_dependency_plugins[] = {
            "ui_runtime", "animation", "particles", "renderer_2d",
            "renderer_opengl", "networking",
            "audio", "audio_null",
            NULL
        };

        for (int d = 0; s_dependency_plugins[d] != NULL &&
                         plugin_result_count < CD_EXPORT_MAX_PLUGINS; d++) {
            const char* pname = s_dependency_plugins[d];

            /* Skip if already copied */
            bool already = false;
            for (int j = 0; j < plugin_result_count; j++) {
                if (strcmp(plugin_results[j].name, pname) == 0) {
                    already = true;
                    break;
                }
            }
            if (already) continue;

            bool ok = copy_plugin_lib(pname, cd_lib_search_dirs,
                                      dll_prefix, dll_ext, plugins_dir);
            if (!ok) {
                /* Try from kernel's loaded plugin paths */
                for (uint32_t k = 0; k < cd_kernel_get_plugins(kernel)->count; k++) {
                    const cd_loaded_plugin_t* lp = &cd_kernel_get_plugins(kernel)->plugins[k];
                    if (lp->plugin && lp->plugin->info.name &&
                        strcmp(lp->plugin->info.name, pname) == 0 &&
                        lp->path[0] != '\0') {
                        char dst_lib[1024];
                        snprintf(dst_lib, sizeof(dst_lib), "%s/%s%s%s",
                                 plugins_dir, dll_prefix, pname, dll_ext);
                        ok = copy_file(lp->path, dst_lib);
                        break;
                    }
                }
            }

            if (ok) {
                snprintf(plugin_results[plugin_result_count].name,
                         sizeof(plugin_results[plugin_result_count].name),
                         "%s", pname);
                plugin_results[plugin_result_count].copied = true;
                plugin_result_count++;
            }
            /* Not an error if a dependency DLL is missing — it may not be
             * needed at runtime if the code path isn't hit. */
        }
    } else {
        /* ---- Legacy: loaded + well-known list approach ---- */

        /* First pass: iterate loaded plugins from the kernel plugin manager */
        for (uint32_t i = 0; i < cd_kernel_get_plugins(kernel)->count &&
                              plugin_result_count < CD_EXPORT_MAX_PLUGINS; i++) {
            const cd_loaded_plugin_t* lp = &cd_kernel_get_plugins(kernel)->plugins[i];
            if (lp->plugin == NULL || lp->plugin->info.name == NULL) continue;

            const char* pname = lp->plugin->info.name;
            if (strcmp(pname, "editor") == 0) continue;

            bool ok = copy_plugin_lib(pname, cd_lib_search_dirs,
                                      dll_prefix, dll_ext, plugins_dir);
            if (!ok && lp->path[0] != '\0') {
                char dst_lib[1024];
                snprintf(dst_lib, sizeof(dst_lib), "%s/%s%s%s",
                         plugins_dir, dll_prefix, pname, dll_ext);
                ok = copy_file(lp->path, dst_lib);
            }

            snprintf(plugin_results[plugin_result_count].name,
                     sizeof(plugin_results[plugin_result_count].name),
                     "%s", pname);
            plugin_results[plugin_result_count].copied = ok;
            plugin_result_count++;

            if (ok && export_plugin_count < CD_EXPORT_MAX_PLUGINS) {
                snprintf(export_plugin_names[export_plugin_count],
                         sizeof(export_plugin_names[export_plugin_count]),
                         "%s", pname);
                export_plugin_count++;
            }
        }

        /* Second pass: try additional well-known plugins */
        static const char* s_all_known_plugins[] = {
            "scripting_lua", "renderer_opengl", "renderer_null",
            "renderer_software", "renderer_metal",
            "renderer_2d",
            "audio", "audio_null",
            "physics_jolt",
            "input", "input_sim",
            "scene_io", "ui_runtime",
            "animation", "particles",
            "networking",
            NULL
        };

        for (int p = 0; s_all_known_plugins[p] != NULL &&
                         plugin_result_count < CD_EXPORT_MAX_PLUGINS; p++) {
            const char* pname = s_all_known_plugins[p];

            bool already = false;
            for (int j = 0; j < plugin_result_count; j++) {
                if (strcmp(plugin_results[j].name, pname) == 0) {
                    already = true;
                    break;
                }
            }
            if (already) continue;

            bool ok = copy_plugin_lib(pname, cd_lib_search_dirs,
                                      dll_prefix, dll_ext, plugins_dir);
            if (ok) {
                snprintf(plugin_results[plugin_result_count].name,
                         sizeof(plugin_results[plugin_result_count].name),
                         "%s", pname);
                plugin_results[plugin_result_count].copied = true;
                plugin_result_count++;

                if (export_plugin_count < CD_EXPORT_MAX_PLUGINS) {
                    snprintf(export_plugin_names[export_plugin_count],
                             sizeof(export_plugin_names[export_plugin_count]),
                             "%s", pname);
                    export_plugin_count++;
                }
            }
        }
    }

    /* ---- Copy project.toml and append [plugins] section (BL-33) ---- */
    bool copied_toml = false;
    {
        const char* proj_path = cd_kernel_get_config(kernel)->project_path
                                ? cd_kernel_get_config(kernel)->project_path : ".";
        char src_toml[1024], dst_toml[1024];
        snprintf(src_toml, sizeof(src_toml), "%s/project.toml", proj_path);
        snprintf(dst_toml, sizeof(dst_toml), "%s/project.toml", output_dir);
        copied_toml = copy_file(src_toml, dst_toml);

        /* Append [plugins] section listing actually-copied plugins so the
         * runtime uses BL-34 config-driven loading instead of hardcoded defaults.
         * Skip if the file already has a [plugins] section. */
        if (copied_toml && export_plugin_count > 0) {
            bool has_plugins_section = false;
            FILE* check = fopen(dst_toml, "r");
            if (check) {
                char ln[256];
                while (fgets(ln, sizeof(ln), check)) {
                    const char* p = ln;
                    while (*p == ' ' || *p == '\t') p++;
                    if (strncmp(p, "[plugins]", 9) == 0) {
                        has_plugins_section = true;
                        break;
                    }
                }
                fclose(check);
            }

            if (!has_plugins_section) {
                FILE* fp = fopen(dst_toml, "a");
                if (fp != NULL) {
                    fprintf(fp, "\n[plugins]\nload = [");
                    for (int i = 0; i < export_plugin_count; i++) {
                        if (i > 0) fprintf(fp, ", ");
                        fprintf(fp, "\"%s\"", export_plugin_names[i]);
                    }
                    fprintf(fp, "]\n");
                    fclose(fp);
                }
            }
        }
    }

    /* ---- Copy Lua library modules (lua_lib/) ---- */
    /* These are required at runtime for require("fps_controller") etc.
     * The scripting_lua plugin sets package.path to find them.
     * In an exported build, we place them in lua_lib/ alongside project.toml
     * so the runtime can resolve them relative to the project directory. */
    int lua_lib_copied = 0;
    {
        static const char* s_lua_lib_files[] = {
            "fps_controller.lua",
            "ai_steering.lua",
            "ai_behavior_tree.lua",
            "ai_team.lua",
            "ai_utility.lua",
            "nav_grid.lua",
            "dialogue.lua",
            "inventory.lua",
            "quest.lua",
            NULL
        };

        /* Search for lua_lib directory in known locations */
        static const char* s_lua_lib_search[] = {
            "plugins/scripting_lua/lua_lib",
            "../plugins/scripting_lua/lua_lib",
            "lua_lib",
            NULL
        };

        const char* lua_lib_src = NULL;
        for (int d = 0; s_lua_lib_search[d] != NULL; d++) {
            char probe[1024];
            snprintf(probe, sizeof(probe), "%s/%s",
                     s_lua_lib_search[d], s_lua_lib_files[0]);
            FILE* fp = fopen(probe, "rb");
            if (fp != NULL) {
                fclose(fp);
                lua_lib_src = s_lua_lib_search[d];
                break;
            }
        }

        if (lua_lib_src != NULL) {
            char lua_lib_dst[1024];
            snprintf(lua_lib_dst, sizeof(lua_lib_dst), "%s/lua_lib", output_dir);
            ensure_dir(lua_lib_dst);

            for (int f = 0; s_lua_lib_files[f] != NULL; f++) {
                char src[1024], dst[1024];
                snprintf(src, sizeof(src), "%s/%s", lua_lib_src, s_lua_lib_files[f]);
                snprintf(dst, sizeof(dst), "%s/%s", lua_lib_dst, s_lua_lib_files[f]);
                if (copy_file(src, dst)) {
                    lua_lib_copied++;
                }
            }
        }
    }

    /* ---- Create launch script ---- */
    bool has_launch = write_launch_script(output_dir, runtime_name);

    /* ---- Strip debug symbols (Linux / macOS only) ---- */
    bool did_strip = false;
    if (strip_debug) {
#ifndef _WIN32
        /* Strip runtime */
        if (copied_runtime) {
            strip_binary(dst_runtime);
        }
        /* Strip plugins */
        for (int p = 0; p < plugin_result_count; p++) {
            if (plugin_results[p].copied) {
                char plib[1024];
                snprintf(plib, sizeof(plib), "%s/%s%s%s",
                         plugins_dir, dll_prefix,
                         plugin_results[p].name, dll_ext);
                strip_binary(plib);
            }
        }
        did_strip = true;
#endif
        /* On Windows, stripping is a build-config concern (Release vs Debug),
         * so we just report false here -- not an error. */
    }

    /* ---- Build response ---- */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        cJSON_Delete(paks_arr);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "output_dir", output_dir);
    cJSON_AddNumberToObject(result, "total_packed", (double)total_packed);
    cJSON_AddItemToObject(result, "paks", paks_arr);

    /* Runtime info */
    {
        cJSON* rt = cJSON_CreateObject();
        cJSON_AddBoolToObject(rt, "copied", copied_runtime);
        cJSON_AddStringToObject(rt, "file", runtime_name);
        cJSON_AddItemToObject(result, "runtime", rt);
    }

    /* Plugins info */
    {
        cJSON* plugins_arr = cJSON_CreateArray();
        for (int p = 0; p < plugin_result_count; p++) {
            cJSON* pi = cJSON_CreateObject();
            cJSON_AddStringToObject(pi, "name", plugin_results[p].name);
            cJSON_AddBoolToObject(pi, "copied", plugin_results[p].copied);
            cJSON_AddItemToArray(plugins_arr, pi);
        }
        cJSON_AddItemToObject(result, "plugins", plugins_arr);
    }

    cJSON_AddBoolToObject(result, "project_toml", copied_toml);

    if (has_launch) {
#ifdef _WIN32
        cJSON_AddStringToObject(result, "launch_script", "launch.bat");
#else
        cJSON_AddStringToObject(result, "launch_script", "launch.sh");
#endif
    }

    cJSON_AddBoolToObject(result, "stripped", did_strip);
    cJSON_AddNumberToObject(result, "lua_lib_copied", (double)lua_lib_copied);

    return result;
}

/* ============================================================================
 * build.rebuild handler
 *
 * Input:  { "config": "Debug"|"Release" (optional, default "Debug"),
 *           "target": "<cmake-target>"  (optional, builds all if omitted) }
 *
 * Runs cmake --build from the repo root.
 * ============================================================================ */

static cJSON* cd_mcp_handle_build_rebuild(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel;

    const char* config = "Debug";
    const char* target = NULL;

    if (params != NULL) {
        const cJSON* cfg = cJSON_GetObjectItemCaseSensitive(params, "config");
        if (cfg && cJSON_IsString(cfg) && cfg->valuestring[0]) {
            config = cfg->valuestring;
        }
        const cJSON* tgt = cJSON_GetObjectItemCaseSensitive(params, "target");
        if (tgt && cJSON_IsString(tgt) && tgt->valuestring[0]) {
            target = tgt->valuestring;
        }
    }

    /* Build cmake command */
    char cmd[2048];
    if (target != NULL) {
        snprintf(cmd, sizeof(cmd),
            "cmake --build build --config %s --target %s 2>&1",
            config, target);
    } else {
        snprintf(cmd, sizeof(cmd),
            "cmake --build build --config %s 2>&1", config);
    }

    /* Execute and capture output */
    FILE* proc = POPEN(cmd, "r");
    if (proc == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to execute cmake build command";
        return NULL;
    }

    /* Read output (last 4KB for response) */
    char output_buf[4096];
    size_t output_len = 0;
    char line[512];
    int error_count = 0;
    int warning_count = 0;

    while (fgets(line, sizeof(line), proc) != NULL) {
        /* Count errors and warnings */
        if (strstr(line, "error") != NULL || strstr(line, "FAILED") != NULL) {
            error_count++;
        }
        if (strstr(line, "warning") != NULL) {
            warning_count++;
        }
        /* Keep the tail of output */
        size_t line_len = strlen(line);
        if (output_len + line_len < sizeof(output_buf) - 1) {
            memcpy(output_buf + output_len, line, line_len);
            output_len += line_len;
        } else if (line_len < sizeof(output_buf) - 1) {
            /* Shift buffer to keep tail */
            size_t keep = sizeof(output_buf) - 1 - line_len;
            memmove(output_buf, output_buf + output_len - keep, keep);
            memcpy(output_buf + keep, line, line_len);
            output_len = keep + line_len;
        }
    }
    output_buf[output_len] = '\0';

    int exit_code = PCLOSE(proc);
#ifdef _WIN32
    /* pclose returns the process exit code on Windows */
#else
    exit_code = WEXITSTATUS(exit_code);
#endif

    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", exit_code == 0);
    cJSON_AddNumberToObject(result, "exit_code", (double)exit_code);
    cJSON_AddStringToObject(result, "config", config);
    if (target) {
        cJSON_AddStringToObject(result, "target", target);
    }
    cJSON_AddNumberToObject(result, "errors", (double)error_count);
    cJSON_AddNumberToObject(result, "warnings", (double)warning_count);
    cJSON_AddStringToObject(result, "output_tail", output_buf);

    return result;
}

/* ============================================================================
 * build.validate handler
 *
 * Input:  { "path": "<export-dir>" (optional, default "builds/export") }
 *
 * Validates an exported game directory has all required components.
 * ============================================================================ */

static cJSON* cd_mcp_handle_build_validate(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel; (void)error_code; (void)error_msg;

    const char* export_dir = "builds/export";
    if (params != NULL) {
        const cJSON* path = cJSON_GetObjectItemCaseSensitive(params, "path");
        if (path && cJSON_IsString(path) && path->valuestring[0]) {
            export_dir = path->valuestring;
        }
    }

    cJSON* result = cJSON_CreateObject();
    cJSON* checks = cJSON_CreateArray();
    int passed = 0, failed = 0;

    /* Helper macro for check results */
    #ifdef _MSC_VER
    #pragma warning(push)
    #pragma warning(disable: 4127) /* conditional expression is constant */
    #endif
    #define ADD_CHECK(name, ok, detail) do { \
        cJSON* c = cJSON_CreateObject(); \
        cJSON_AddStringToObject(c, "check", name); \
        cJSON_AddBoolToObject(c, "passed", ok); \
        if (detail) cJSON_AddStringToObject(c, "detail", detail); \
        cJSON_AddItemToArray(checks, c); \
        if (ok) passed++; else failed++; \
    } while (0)

    /* Check 1: Export directory exists */
    {
        char probe[1024];
        snprintf(probe, sizeof(probe), "%s/project.toml", export_dir);
        FILE* fp = fopen(probe, "rb");
        if (fp) { fclose(fp); ADD_CHECK("project.toml", true, NULL); }
        else { ADD_CHECK("project.toml", false, "project.toml not found"); }
    }

    /* Check 2-4: PAK files exist and have valid headers */
    {
        static const char* pak_names[] = { "scenes.pak", "scripts.pak", "assets.pak" };
        for (int i = 0; i < 3; i++) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", export_dir, pak_names[i]);
            FILE* fp = fopen(path, "rb");
            if (fp) {
                char magic[4] = {0};
                size_t n = fread(magic, 1, 4, fp);
                fclose(fp);
                if (n == 4 && memcmp(magic, "CDPK", 4) == 0) {
                    ADD_CHECK(pak_names[i], true, NULL);
                } else {
                    ADD_CHECK(pak_names[i], false, "Invalid PAK header");
                }
            } else {
                ADD_CHECK(pak_names[i], false, "File not found");
            }
        }
    }

    /* Check 5: Runtime binary */
    {
#ifdef _WIN32
        const char* rt = "cadence-runtime.exe";
#else
        const char* rt = "cadence-runtime";
#endif
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", export_dir, rt);
        FILE* fp = fopen(path, "rb");
        if (fp) { fclose(fp); ADD_CHECK("runtime_binary", true, NULL); }
        else { ADD_CHECK("runtime_binary", false, "Runtime binary not found"); }
    }

    /* Check 6: Launch script */
    {
#ifdef _WIN32
        const char* script = "launch.bat";
#else
        const char* script = "launch.sh";
#endif
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", export_dir, script);
        FILE* fp = fopen(path, "rb");
        if (fp) { fclose(fp); ADD_CHECK("launch_script", true, NULL); }
        else { ADD_CHECK("launch_script", false, "Launch script not found"); }
    }

    /* Check 7: Plugins — verify each plugin listed in project.toml exists */
    {
        /* Parse exported project.toml for [plugins].load list */
        cd_project_t export_project;
        char export_toml_path[1024];
        snprintf(export_toml_path, sizeof(export_toml_path),
                 "%s/project.toml", export_dir);
        cd_result_t pr = cd_project_load(&export_project, export_toml_path);

        if (pr == CD_OK && export_project.plugins.load_count > 0) {
            /* Check each listed plugin DLL exists */
#ifdef _WIN32
            const char* check_dll_ext = ".dll";
            const char* check_dll_prefix = "";
#elif defined(__APPLE__)
            const char* check_dll_ext = ".dylib";
            const char* check_dll_prefix = "lib";
#else
            const char* check_dll_ext = ".so";
            const char* check_dll_prefix = "lib";
#endif
            for (uint32_t pi = 0; pi < export_project.plugins.load_count; pi++) {
                const char* pname = export_project.plugins.load[pi];
                if (pname[0] == '\0') continue;

                char plugin_path[1024];
                snprintf(plugin_path, sizeof(plugin_path),
                         "%s/plugins/%s%s%s", export_dir,
                         check_dll_prefix, pname, check_dll_ext);
                FILE* fp = fopen(plugin_path, "rb");

                char check_name[300];
                snprintf(check_name, sizeof(check_name), "plugin_%s", pname);

                if (fp) {
                    fclose(fp);
                    ADD_CHECK(check_name, true, NULL);
                } else {
                    char detail[512];
                    snprintf(detail, sizeof(detail),
                             "%s%s%s not found in plugins/",
                             check_dll_prefix, pname, check_dll_ext);
                    ADD_CHECK(check_name, false, detail);
                }
            }
        } else {
            /* No plugins list — just check for core plugin */
            char path[1024];
#ifdef _WIN32
            snprintf(path, sizeof(path), "%s/plugins/scripting_lua.dll", export_dir);
#else
            snprintf(path, sizeof(path), "%s/plugins/libscripting_lua.so", export_dir);
#endif
            FILE* fp = fopen(path, "rb");
            if (fp) { fclose(fp); ADD_CHECK("plugins", true, NULL); }
            else { ADD_CHECK("plugins", false, "Core plugin not found in plugins/"); }
        }
    }

    #undef ADD_CHECK
    #ifdef _MSC_VER
    #pragma warning(pop)
    #endif

    cJSON_AddBoolToObject(result, "valid", failed == 0);
    cJSON_AddNumberToObject(result, "passed", (double)passed);
    cJSON_AddNumberToObject(result, "failed", (double)failed);
    cJSON_AddStringToObject(result, "path", export_dir);
    cJSON_AddItemToObject(result, "checks", checks);

    return result;
}

/* ============================================================================
 * build.run handler
 *
 * Input:  { "path": "<export-dir>" (optional, default "builds/export") }
 *
 * Launches the exported game in a background process.
 * ============================================================================ */

static cJSON* cd_mcp_handle_build_run(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel; (void)error_code; (void)error_msg;

    const char* export_dir = "builds/export";
    if (params != NULL) {
        const cJSON* path = cJSON_GetObjectItemCaseSensitive(params, "path");
        if (path && cJSON_IsString(path) && path->valuestring[0]) {
            export_dir = path->valuestring;
        }
    }

    /* Build command to launch the exported game */
    char cmd[2048];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd),
        "start \"\" /D \"%s\" cadence-runtime.exe --project .", export_dir);
#else
    snprintf(cmd, sizeof(cmd),
        "cd \"%s\" && LD_LIBRARY_PATH=./plugins:$LD_LIBRARY_PATH "
        "./cadence-runtime --project . &", export_dir);
#endif

    int ret = system(cmd);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", ret == 0);
    cJSON_AddStringToObject(result, "path", export_dir);
    cJSON_AddStringToObject(result, "command", cmd);

    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_build_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "build.config",
        cd_mcp_handle_build_config,
        "Return current project build configuration and engine info",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "build.export",
        cd_mcp_handle_build_export,
        "Pack project assets into PAK files and copy runtime for distribution",
        "{\"type\":\"object\",\"properties\":{"
        "\"output\":{\"type\":\"string\",\"description\":\"Output directory (default: builds/export)\"},"
        "\"strip_debug\":{\"type\":\"boolean\",\"description\":\"Strip debug symbols from binaries (Linux/macOS only)\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "build.rebuild",
        cd_mcp_handle_build_rebuild,
        "Run cmake build to compile engine and plugins",
        "{\"type\":\"object\",\"properties\":{"
        "\"config\":{\"type\":\"string\",\"enum\":[\"Debug\",\"Release\"],\"description\":\"Build configuration (default: Debug)\"},"
        "\"target\":{\"type\":\"string\",\"description\":\"CMake target to build (default: all)\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "build.validate",
        cd_mcp_handle_build_validate,
        "Validate an exported game directory has all required components",
        "{\"type\":\"object\",\"properties\":{"
        "\"path\":{\"type\":\"string\",\"description\":\"Export directory to validate (default: builds/export)\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "build.run",
        cd_mcp_handle_build_run,
        "Launch an exported game in a background process",
        "{\"type\":\"object\",\"properties\":{"
        "\"path\":{\"type\":\"string\",\"description\":\"Export directory to run (default: builds/export)\"}"
        "}}");
    if (res != CD_OK) return res;

    return CD_OK;
}
