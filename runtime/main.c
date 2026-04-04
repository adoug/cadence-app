/* runtime/main.c - Cadence Runtime entry point (stripped, no MCP/editor)
 *
 * Task 11.1: cadence-runtime binary
 * WP-10F: project.toml startup manifest support
 *
 * This is the game-only runtime. Unlike cadence-engine (the dev binary),
 * it does NOT include the MCP server, editor UI, hot-reload, or debug draw.
 * It loads a scene, enters play mode, and runs the game loop until exit.
 *
 * Usage:
 *   cadence-runtime [options]
 *
 * Options:
 *   --project <path>     Path to project root (default: ".")
 *   --scene <file>       Scene file to load (relative to project)
 *   --headless           Run without window or GPU rendering
 *   --help               Show usage information
 *
 * If --scene is not provided, the runtime looks for project.toml in the
 * project directory and reads game.entry_scene and autoload.scripts from it.
 */
#include "cadence/cd_api.h"
#include "cadence/cd_fx.h"
#include "cadence/cd_scene.h"
#include "cadence/cd_scene_io_api.h"
#include "cadence/cd_skinned_model.h"
#include "cadence/cd_events.h"
#include "cadence/cd_resource.h"
#include "cadence/cd_project.h"
#include "cadence/cd_memory.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_kernel_shared.h"
#include "cadence/cd_terrain_desc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

/* ============================================================================
 * Runtime configuration (extends cd_config_t with runtime-specific fields)
 * ============================================================================ */

typedef struct {
    const char* scene_path;    /**< Scene file to load on startup. */
    const char* window_title;  /**< Window title override (WP-9D). */
} cd_runtime_config_t;

/* ============================================================================
 * Usage / help
 * ============================================================================ */

static void print_usage(const char* program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("\n");
    printf("Cadence Runtime — standalone game player (no MCP, no editor)\n");
    printf("\n");
    printf("Options:\n");
    printf("  --project <path>     Path to project root (default: \".\")\n");
    printf("  --scene <file>       Scene file to load on startup\n");
    printf("  --title <text>       Window title (default: \"Cadence Game\")\n");
    printf("  --headless           Run without window or GPU rendering\n");
    printf("  --help               Show this help message\n");
}

/* ============================================================================
 * Argument parsing
 * ============================================================================ */

static bool parse_args(int argc, char* argv[],
                       cd_config_t* config,
                       cd_runtime_config_t* rt_config) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) {
            config->headless = true;
        } else if (strcmp(argv[i], "--project") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --project requires a path argument\n");
                return false;
            }
            config->project_path = argv[++i];
        } else if (strcmp(argv[i], "--scene") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --scene requires a file argument\n");
                return false;
            }
            rt_config->scene_path = argv[++i];
        } else if (strcmp(argv[i], "--title") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --title requires a text argument\n");
                return false;
            }
            rt_config->window_title = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return false;
        } else {
            fprintf(stderr, "Error: unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return false;
        }
    }
    return true;
}

/* ============================================================================
 * Resource system helpers (WP-10D)
 * ============================================================================ */

/** Known PAK file names to auto-mount from the project directory. */
static const char* s_pak_names[] = {
    "scenes.pak",
    "scripts.pak",
    "assets.pak",
};
#define CD_PAK_NAME_COUNT (sizeof(s_pak_names) / sizeof(s_pak_names[0]))

/**
 * Try to mount a PAK file. Returns true if mounted, false if not found or
 * on error (non-fatal -- logged and skipped).
 */
static bool try_mount_pak(cd_resource_t* resource, const char* project_dir,
                          const char* pak_name) {
    char pak_path[1024];
    snprintf(pak_path, sizeof(pak_path), "%s/%s", project_dir, pak_name);

    /* Probe whether the file exists before attempting to mount. */
    FILE* probe = fopen(pak_path, "rb");
    if (probe == NULL) {
        return false; /* PAK not present -- not an error */
    }
    fclose(probe);

    cd_result_t r = cd_resource_mount_pak(resource, pak_path);
    if (r != CD_OK) {
        fprintf(stderr, "[runtime] Warning: failed to mount %s (error %d)\n",
                pak_name, r);
        return false;
    }

    printf("[runtime] Mounted PAK: %s\n", pak_name);
    return true;
}

/* ============================================================================
 * project.toml parsing (WP-10F)
 *
 * A minimal TOML reader that extracts game.entry_scene (string) and
 * autoload.scripts (array of strings) from project.toml.  This avoids
 * pulling in the full gamespec parser or cJSON into the runtime binary.
 * ============================================================================ */

#define CD_PROJECT_MAX_AUTOLOAD 32
#define CD_PROJECT_PATH_MAX     512

typedef struct {
    char entry_scene[CD_PROJECT_PATH_MAX];
    char game_name[256];
    char game_version[64];
    char autoload_scripts[CD_PROJECT_MAX_AUTOLOAD][CD_PROJECT_PATH_MAX];
    int  autoload_count;
    bool valid;
} cd_project_config_t;

/**
 * Read entire file into a malloc'd buffer.  Returns NULL on failure.
 */
static char* cd_read_file(const char* path, size_t* out_len) {
    FILE* fp = fopen(path, "rb");
    if (fp == NULL) return NULL;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return NULL; }

    char* buf = (char*)calloc(1, (size_t)sz + 1);
    if (buf == NULL) { fclose(fp); return NULL; }

    size_t n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[n] = '\0';
    if (out_len != NULL) *out_len = n;
    return buf;
}

/**
 * Skip leading whitespace.
 */
static const char* cd_skip_ws(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/**
 * Extract a quoted string value from a TOML line after '='.
 * Copies into out (up to out_size) and returns true on success.
 */
static bool cd_extract_toml_string(const char* value_start,
                                    char* out, size_t out_size) {
    const char* p = cd_skip_ws(value_start);
    if (*p != '"') return false;
    p++;

    size_t i = 0;
    while (*p != '\0' && *p != '"' && i < out_size - 1) {
        if (*p == '\\' && *(p + 1) != '\0') {
            p++;
            switch (*p) {
                case '\\': out[i++] = '\\'; break;
                case '"':  out[i++] = '"';  break;
                case 'n':  out[i++] = '\n'; break;
                default:   out[i++] = *p;   break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return (*p == '"');
}

/**
 * Parse a TOML array of strings: ["a", "b", "c"]
 * Fills entries[] up to max_entries, returns the count.
 */
static int cd_parse_toml_string_array(const char* value_start,
                                       char entries[][CD_PROJECT_PATH_MAX],
                                       int max_entries) {
    const char* p = cd_skip_ws(value_start);
    if (*p != '[') return 0;
    p++;

    int count = 0;
    while (*p != '\0' && *p != ']' && count < max_entries) {
        p = cd_skip_ws(p);
        if (*p == '"') {
            char tmp[CD_PROJECT_PATH_MAX];
            if (cd_extract_toml_string(p, tmp, sizeof(tmp))) {
                snprintf(entries[count], CD_PROJECT_PATH_MAX, "%s", tmp);
                count++;
            }
            /* Advance past the closing quote */
            p++; /* skip opening quote */
            while (*p != '\0' && *p != '"') {
                if (*p == '\\') p++; /* skip escaped char */
                p++;
            }
            if (*p == '"') p++; /* skip closing quote */
        }
        /* Skip comma and whitespace */
        p = cd_skip_ws(p);
        if (*p == ',') p++;
    }

    return count;
}

/**
 * Parse project.toml and populate a cd_project_config_t.
 *
 * Returns true if the file was found and parsed, false otherwise.
 */
static bool cd_parse_project_toml(const char* project_path,
                                    cd_project_config_t* config) {
    memset(config, 0, sizeof(cd_project_config_t));

    char toml_path[1024];
    snprintf(toml_path, sizeof(toml_path), "%s/project.toml", project_path);

    size_t file_len = 0;
    char* content = cd_read_file(toml_path, &file_len);
    if (content == NULL) return false;

    /* Track which section we're in */
    enum { SEC_NONE, SEC_GAME, SEC_AUTOLOAD } current_section = SEC_NONE;

    /* Parse line by line */
    char* line = content;
    while (line != NULL && *line != '\0') {
        /* Find end of line */
        char* eol = strchr(line, '\n');
        if (eol != NULL) *eol = '\0';

        /* Trim trailing \r */
        size_t ll = strlen(line);
        if (ll > 0 && line[ll - 1] == '\r') line[ll - 1] = '\0';

        const char* trimmed = cd_skip_ws(line);

        /* Skip empty lines and comments */
        if (*trimmed == '\0' || *trimmed == '#') {
            line = eol ? eol + 1 : NULL;
            continue;
        }

        /* Section headers */
        if (*trimmed == '[') {
            if (strncmp(trimmed, "[game]", 6) == 0) {
                current_section = SEC_GAME;
            } else if (strncmp(trimmed, "[autoload]", 10) == 0) {
                current_section = SEC_AUTOLOAD;
            } else {
                current_section = SEC_NONE;
            }
            line = eol ? eol + 1 : NULL;
            continue;
        }

        /* Key = value pairs */
        const char* eq = strchr(trimmed, '=');
        if (eq != NULL) {
            /* Extract key (trimmed) */
            char key[128];
            size_t klen = 0;
            const char* kp = trimmed;
            while (kp < eq && klen < sizeof(key) - 1) {
                if (*kp != ' ' && *kp != '\t') {
                    key[klen++] = *kp;
                } else if (klen > 0) {
                    break;
                }
                kp++;
            }
            key[klen] = '\0';

            const char* val = eq + 1;

            if (current_section == SEC_GAME) {
                if (strcmp(key, "name") == 0) {
                    cd_extract_toml_string(val, config->game_name,
                                           sizeof(config->game_name));
                } else if (strcmp(key, "version") == 0) {
                    cd_extract_toml_string(val, config->game_version,
                                           sizeof(config->game_version));
                } else if (strcmp(key, "entry_scene") == 0) {
                    cd_extract_toml_string(val, config->entry_scene,
                                           sizeof(config->entry_scene));
                }
            } else if (current_section == SEC_AUTOLOAD) {
                if (strcmp(key, "scripts") == 0) {
                    config->autoload_count = cd_parse_toml_string_array(
                        val, config->autoload_scripts,
                        CD_PROJECT_MAX_AUTOLOAD);
                }
            }
        }

        line = eol ? eol + 1 : NULL;
    }

    free(content);
    config->valid = (config->entry_scene[0] != '\0');
    return config->valid;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char* argv[]) {
    printf("Cadence Runtime v%d.%d.%d\n",
           CD_VERSION_MAJOR,
           CD_VERSION_MINOR,
           CD_VERSION_PATCH);

    /* ---- Parse command-line arguments ---------------------------------- */
    cd_engine_t engine;
    memset(&engine, 0, sizeof(engine));
    /* WI-C.5: Allocate shared sub-struct and its arrays */
    engine.shared = calloc(1, sizeof(cd_kernel_shared_t));
    {
        cd_kernel_shared_t* shared = (cd_kernel_shared_t*)engine.shared;
        shared->skinned_models = calloc(CD_MAX_SKINNED_MODELS, sizeof(cd_skinned_model_t));
        shared->terrain_descs = calloc(CD_MAX_TERRAINS, sizeof(cd_terrain_desc_t));
        shared->terrain_desc_count = CD_MAX_TERRAINS;
    }

    engine.config.headless      = false;
    engine.config.project_path  = ".";
    engine.config.window_title  = NULL;

    cd_runtime_config_t rt_config;
    memset(&rt_config, 0, sizeof(rt_config));

    if (!parse_args(argc, argv, &engine.config, &rt_config)) {
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 ||
                strcmp(argv[i], "-h") == 0) {
                return 0;
            }
        }
        return 1;
    }

    /* ---- WP-10F: Resolve scene from --scene flag or project.toml ------- */
    cd_project_config_t project_config;
    memset(&project_config, 0, sizeof(project_config));

    const char* scene_to_load = rt_config.scene_path;

    if (scene_to_load == NULL) {
        /* No --scene flag: try reading project.toml */
        if (cd_parse_project_toml(engine.config.project_path,
                                   &project_config)) {
            scene_to_load = project_config.entry_scene;
            printf("[runtime] project.toml: game = \"%s\" v%s\n",
                   project_config.game_name,
                   project_config.game_version);
            printf("[runtime] project.toml: entry_scene = \"%s\"\n",
                   project_config.entry_scene);
            if (project_config.autoload_count > 0) {
                printf("[runtime] project.toml: %d autoload script(s)\n",
                       project_config.autoload_count);
            }

            /* Use game name as default window title if not overridden */
            if (rt_config.window_title == NULL &&
                project_config.game_name[0] != '\0') {
                rt_config.window_title = project_config.game_name;
            }
        } else {
            fprintf(stderr,
                "[runtime] Error: no --scene specified and no project.toml "
                "found in '%s'\n"
                "  Run 'gamespec.compose' first or use --scene <file>\n",
                engine.config.project_path);
            return 1;
        }
    }

    /* ---- Parse project.toml BEFORE engine init (BL-34: plugin config) --- */
    {
        char toml_path[1024];
        snprintf(toml_path, sizeof(toml_path), "%s/project.toml",
                 engine.config.project_path);
        cd_project_load((cd_project_t*)cd_kernel_get_project(&engine), toml_path);
    }

    /* ---- Initialize engine --------------------------------------------- */
    /* Disable hot-copy for packaged runtime — plugins load directly from
     * ./plugins/ without a staging directory. Hot-copy is only useful in
     * the editor for live DLL replacement while debugging. */
    engine.config.disable_hot_copy = true;

#ifdef _WIN32
    /* BL-33: Add plugins/ directory to DLL search path so inter-plugin
     * dependencies resolve when loading from the export layout. */
    {
        char exe_dir[1024];
        DWORD len = GetModuleFileNameA(NULL, exe_dir, sizeof(exe_dir));
        if (len > 0 && len < sizeof(exe_dir)) {
            /* Find last separator */
            char* last_sep = NULL;
            for (char* p = exe_dir; *p; p++) {
                if (*p == '\\' || *p == '/') last_sep = p;
            }
            if (last_sep) {
                *last_sep = '\0';
                char plugins_dir[1024];
                snprintf(plugins_dir, sizeof(plugins_dir), "%s\\plugins", exe_dir);
                SetDllDirectoryA(plugins_dir);
            }
        }
    }
#endif

    cd_result_t res = cd_engine_init(&engine);
    if (res != CD_OK) {
        fprintf(stderr, "[runtime] Engine initialization failed: %d\n", res);
        return 1;
    }
    cd_fx_state_init(cd_kernel_get_fx(&engine));

    printf("[runtime] Engine started%s\n",
           engine.config.headless ? " (headless mode)" : "");

    /* ---- WP-10D: Initialize resource system and mount PAK files ------- */
    cd_resource_t resource;
    res = cd_resource_init(&resource, engine.config.project_path);
    if (res != CD_OK) {
        fprintf(stderr, "[runtime] Resource system init failed: %d\n", res);
        cd_engine_shutdown(&engine);
        return 1;
    }

    /* Mount any PAK files found in the project directory */
    uint32_t paks_mounted = 0;
    for (size_t i = 0; i < CD_PAK_NAME_COUNT; i++) {
        if (try_mount_pak(&resource, engine.config.project_path, s_pak_names[i])) {
            paks_mounted++;
        }
    }

    if (paks_mounted > 0) {
        printf("[runtime] %u PAK file(s) mounted\n", paks_mounted);
    } else {
        printf("[runtime] No PAK files found; using filesystem loading\n");
    }

    /* Store resource pointer on engine so subsystems can access it */
    engine.resource = &resource;

    /* ---- WP-9D/S2.4: Create window with project display settings ------ */
    const cd_window_api_t* wapi = cd_kernel_get_window_api(&engine);
    cd_window_t* rt_window = NULL;
    if (!engine.config.headless && wapi) {
        res = wapi->init(wapi->userdata);
        if (res != CD_OK) {
            fprintf(stderr, "[runtime] Window system init failed: %d\n", res);
        }
        cd_window_config_t win_cfg;
        memset(&win_cfg, 0, sizeof(win_cfg));

        /* Use project.toml display settings if loaded (S2.4) */
        const cd_project_t* proj = cd_kernel_get_project(&engine);
        if (proj && proj->loaded) {
            win_cfg.width      = proj->display.width;
            win_cfg.height     = proj->display.height;
            win_cfg.fullscreen = proj->display.fullscreen;
            win_cfg.vsync      = proj->display.vsync;
            win_cfg.title      = cd_project_display_title(proj);
        } else {
            win_cfg.width      = 1280;
            win_cfg.height     = 720;
            win_cfg.vsync      = true;
            win_cfg.fullscreen = false;
            win_cfg.title      = "Cadence Game";
        }
        win_cfg.resizable  = true;

        /* CLI overrides */
        if (rt_config.window_title)
            win_cfg.title = rt_config.window_title;

        rt_window = (cd_window_t*)calloc(1, sizeof(cd_window_t));
        if (rt_window != NULL) {
            res = wapi->create(rt_window, &win_cfg, wapi->userdata);
            if (res != CD_OK) {
                fprintf(stderr, "[runtime] Window creation failed: %d\n", res);
                free(rt_window);
                rt_window = NULL;
            } else {
                engine.window = rt_window;

                /* Initialize renderer HAL -- GL context is now current */
                const cd_render_hal_t* hal = cd_render_hal_get();
                if (hal && hal->init) {
                    void* native = wapi->get_native_handle(rt_window, wapi->userdata);
                    uint32_t fb_w = 0, fb_h = 0;
                    wapi->get_framebuffer_size(rt_window, &fb_w, &fb_h, wapi->userdata);
                    res = hal->init(native, false, fb_w, fb_h);
                    if (res != CD_OK) {
                        fprintf(stderr, "[runtime] Renderer HAL init failed: %d\n", res);
                    }
                }
            }
        }
    }

    /* ---- Load scene ---------------------------------------------------- */
    if (scene_to_load != NULL) {
        /* Build full path: project_path / scene_path */
        char scene_full[1024];
        snprintf(scene_full, sizeof(scene_full), "%s/%s",
                 engine.config.project_path, scene_to_load);

        /* Allocate scene (cd_scene_load initializes it internally) */
        if (engine.scene != NULL) {
            cd_scene_shutdown(engine.scene);
            free(engine.scene);
        }
        engine.scene = (cd_scene_t*)calloc(1, sizeof(cd_scene_t));
        if (engine.scene == NULL) {
            fprintf(stderr, "[runtime] Failed to allocate scene\n");
            cd_engine_shutdown(&engine);
            return 1;
        }

        /* Wire type registry + kernel for component lifecycle callbacks (P0-1) */
        ((cd_scene_t*)engine.scene)->type_registry = (cd_type_registry_t*)engine.types;
        ((cd_scene_t*)engine.scene)->kernel = &engine;

        /* Try loading from filesystem first, then from PAK via resource system */
        const cd_scene_io_api_t* sio = cd_kernel_get_scene_io_api(&engine);
        if (!sio || !sio->load_scene) {
            fprintf(stderr, "[runtime] Scene I/O plugin not loaded -- cannot load scene\n");
            cd_engine_shutdown(&engine);
            return 1;
        }
        res = sio->load_scene(engine.scene, engine.types, scene_full,
                              sio->userdata);
        if (res != CD_OK && paks_mounted > 0) {
            /* Scene not on disk -- try reading from mounted PAK files.
             * Build a res:// URI from the scene path. */
            char scene_uri[1024];
            snprintf(scene_uri, sizeof(scene_uri), "res://%s", scene_to_load);

            void* scene_data = NULL;
            size_t scene_size = 0;
            cd_result_t pak_res = cd_resource_read(&resource, scene_uri,
                                                    &scene_data, &scene_size);
            if (pak_res == CD_OK && scene_data != NULL && sio->load_scene_from_string) {
                res = sio->load_scene_from_string(engine.scene, engine.types,
                                                    (const char*)scene_data,
                                                    scene_size, sio->userdata);
                cd_mem_free_tagged(scene_data);
            }
        }

        if (res != CD_OK) {
            fprintf(stderr, "[runtime] Failed to load scene '%s': %d\n",
                    scene_full, res);
            cd_engine_shutdown(&engine);
            return 1;
        }

        printf("[runtime] Scene loaded: %s\n", scene_to_load);

        /* Emit scene loaded event so plugins (e.g. scripting) discover
         * LuaScript components and call on_ready(). */
        cd_event_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = CD_EVT_SCENE_LOADED;
        cd_event_emit(cd_kernel_get_events(&engine), &evt);
    }

    /* ---- WP-10F: Execute autoload scripts ------------------------------ */
    if (project_config.valid && project_config.autoload_count > 0 &&
        rt_config.scene_path == NULL) {
        for (int i = 0; i < project_config.autoload_count; i++) {
            char script_full[1024];
            snprintf(script_full, sizeof(script_full), "%s/%s",
                     engine.config.project_path,
                     project_config.autoload_scripts[i]);
            printf("[runtime] Autoload: %s\n",
                   project_config.autoload_scripts[i]);
            /* Autoload scripts are loaded via the event bus -- the
             * scripting_lua plugin listens for CD_EVT_ASSET_CHANGED
             * and reloads/executes the file.  We emit immediately so
             * scripts run before the first frame. */
            cd_event_t evt;
            memset(&evt, 0, sizeof(evt));
            evt.type = CD_EVT_ASSET_CHANGED;
            evt.data.asset.uri = script_full;
            cd_event_emit_immediate(cd_kernel_get_events(&engine), &engine, &evt);
        }
    }

    /* ---- Enter play mode and run -------------------------------------- */
    res = cd_engine_play(&engine);
    if (res != CD_OK) {
        fprintf(stderr, "[runtime] Failed to enter play mode: %d\n", res);
        cd_engine_shutdown(&engine);
        return 1;
    }

    printf("[runtime] Entering game loop\n");

    if (rt_window != NULL && wapi != NULL) {
        /* Windowed mode: poll events, tick, swap buffers */
        while (engine.running && !wapi->should_close(rt_window, wapi->userdata)) {
            wapi->poll_events(wapi->userdata);
            res = cd_engine_tick(&engine);
            if (res != CD_OK) {
                fprintf(stderr, "[runtime] Game loop error: %d\n", res);
                break;
            }
            wapi->swap_buffers(rt_window, wapi->userdata);
        }
    } else {
        /* Headless mode: simple run loop */
        res = cd_engine_run(&engine);
        if (res != CD_OK) {
            fprintf(stderr, "[runtime] Game loop error: %d\n", res);
        }
    }

    /* ---- Shutdown ------------------------------------------------------ */
    if (rt_window != NULL && wapi != NULL) {
        engine.window = NULL;
        wapi->destroy(rt_window, wapi->userdata);
        free(rt_window);
        wapi->shutdown(wapi->userdata);
    }

    /* WP-10D: Shut down resource system before engine */
    engine.resource = NULL;
    cd_resource_shutdown(&resource);

    res = cd_engine_shutdown(&engine);
    if (res != CD_OK) {
        fprintf(stderr, "[runtime] Engine shutdown error: %d\n", res);
        return 1;
    }

    printf("[runtime] Shut down cleanly\n");
    return 0;
}
