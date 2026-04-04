/* main/main.c - Cadence Engine entry point
 *
 * Parses command-line arguments, initializes the engine, runs the
 * main loop (or immediately exits in headless-without-MCP mode),
 * and shuts down cleanly.
 *
 * Usage:
 *   cadence-engine [options]
 *
 * Options:
 *   --headless           Run without window or GPU rendering
 *   --project <path>     Path to project root (default: ".")
 *   --help               Show usage information
 */
#include "cadence/cd_api.h"
#include "cadence/cd_scene.h"
#include "cadence/cd_scene_io_api.h"
#include "cadence/cd_events.h"
#include "cadence/cd_project.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_async.h"
#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp_tool_state.h"
#include "cadence/cd_mcp_socket.h"
#include "cadence/cd_skinned_model.h"
#include "cadence/cd_asset_db.h"
#include "cadence/cd_net_types.h"
#include "cadence/cd_net_ext_api.h"
#include "cadence/cd_render_hal.h"
#include "cadence/cd_net_session.h"
#include "cadence/cd_log.h"
#include "cadence/cd_fx.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_kernel_shared.h"
#include "cadence/cd_kernel_callbacks.h"
#include "cadence/cd_terrain_desc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(CD_HAS_CIMGUI)
#include "cadence/cd_editor.h"
#endif

/* glad is needed for gladLoadGLLoader in headed mode (editor GL pointers) */
#include <glad/glad.h>

/* Wrapper for gladLoadGLLoader: adapts (const char*, void*) to (const char*).
 * GLAD expects GLADloadproc = void*(*)(const char*), but our window API's
 * get_proc_address now takes a void* ud parameter. This wrapper captures the
 * active window API pointer so glad can load GL function pointers. */
static const cd_window_api_t* s_glad_wapi = NULL;
static void* cd_glad_proc_wrapper(const char* name) {
    if (s_glad_wapi && s_glad_wapi->get_proc_address)
        return s_glad_wapi->get_proc_address(name, s_glad_wapi->userdata);
    return NULL;
}

/* ============================================================================
 * Pre-tick callback — polls MCP server + socket transport each frame
 * ============================================================================ */

/** Aggregated pre-tick context for all MCP transports. */
typedef struct {
    cd_mcp_server_t*         mcp;
    cd_mcp_socket_server_t*  socket_srv;  /* NULL if not using socket transport */
} cd_mcp_pre_tick_ctx_t;

static cd_mcp_pre_tick_ctx_t s_mcp_ctx;

static void mcp_pre_tick(cd_kernel_t* kernel, void* userdata) {
    cd_mcp_pre_tick_ctx_t* ctx = (cd_mcp_pre_tick_ctx_t*)userdata;
    if (!ctx) return;

    if (ctx->mcp && ctx->mcp->initialized) {
        cd_mcp_server_poll(ctx->mcp, kernel);
    }

    if (ctx->socket_srv) {
        cd_mcp_socket_poll(ctx->socket_srv, ctx->mcp, kernel);
    }

    /* P2-4: Poll for completed async tool tasks and send their responses */
    if (ctx->mcp && ctx->mcp->initialized) {
        cd_mcp_async_poll(&ctx->mcp->async_pool, ctx->mcp);
    }
}

/* ============================================================================
 * MCP server initialization
 * ============================================================================ */

/* Tool registration is now handled by cd_mcp_register_all_tools() in the
 * MCP library (cd_mcp_tool_registry.c).  No individual calls needed here. */

/* ============================================================================
 * Usage / help
 * ============================================================================ */

static void print_usage(const char* program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("\n");
    printf("Options:\n");
    printf("  --headless                Run without window or GPU rendering\n");
    printf("  --renderer=<backend>      Force renderer: opengl, vulkan, software, null\n");
    printf("                            (default: auto-detect best available)\n");
    printf("  --software-render         Alias for --renderer=software\n");
    printf("  --project <path>          Path to project root (default: \".\")\n");
    printf("  --mcp-token <token>       Require auth token for TCP/WS MCP clients\n");
    printf("  --mcp-socket <path>       Local IPC transport (named pipe on Windows,\n");
    printf("                            Unix domain socket on POSIX)\n");
    printf("  --server                  Dedicated server mode (implies --headless)\n");
    printf("  --net-host <port>         Host on port (implies --server --headless)\n");
    printf("  --net-max-clients <n>     Max connected clients (default 8)\n");
    printf("  --help                    Show this help message\n");
}

/* ============================================================================
 * Scene auto-loading from project.toml
 *
 * Uses the parsed project.game.entry_scene field to load the initial scene.
 * Used in headed mode so the engine displays something on startup.
 * ============================================================================ */

static bool cd_load_entry_scene(cd_engine_t* engine) {
    const char* entry_scene = cd_kernel_get_project((const cd_kernel_t*)engine)->game.entry_scene;
    if (!cd_kernel_get_project((const cd_kernel_t*)engine)->loaded || entry_scene[0] == '\0') {
        /* Fallback: try parsing project.toml if not already loaded */
        if (!cd_kernel_get_project((const cd_kernel_t*)engine)->loaded) {
            char toml_path[1024];
            snprintf(toml_path, sizeof(toml_path), "%s/project.toml",
                     engine->config.project_path);
            cd_project_load((cd_project_t*)cd_kernel_get_project(engine), toml_path);
            entry_scene = cd_kernel_get_project((const cd_kernel_t*)engine)->game.entry_scene;
        }
        if (entry_scene[0] == '\0') return false;
    }

    /* Build full path and load */
    char scene_full[1024];
    snprintf(scene_full, sizeof(scene_full), "%s/%s",
             engine->config.project_path, entry_scene);

    engine->scene = (cd_scene_t*)calloc(1, sizeof(cd_scene_t));
    if (!engine->scene) return false;

    const cd_scene_io_api_t* sio = cd_kernel_get_scene_io_api(engine);
    if (!sio || !sio->load_scene) {
        CD_LOG_ERROR("cadence", "Scene I/O plugin not loaded -- cannot load scene");
        free(engine->scene);
        engine->scene = NULL;
        return false;
    }
    cd_result_t r = sio->load_scene(engine->scene, engine->types, scene_full,
                                    sio->userdata);
    if (r != CD_OK) {
        CD_LOG_ERROR("cadence", "Failed to load entry scene '%s': %d",
                scene_full, r);
        free(engine->scene);
        engine->scene = NULL;
        return false;
    }

    /* Wire type registry + kernel AFTER load_scene (load_scene calls
     * cd_scene_init which zeroes these fields) */
    ((cd_scene_t*)engine->scene)->type_registry = (cd_type_registry_t*)engine->types;
    ((cd_scene_t*)engine->scene)->kernel = engine;

    CD_LOG_INFO("cadence", "Loaded entry scene: %s", entry_scene);

    /* Emit scene loaded event so plugins (e.g. scripting) can initialize. */
    cd_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CD_EVT_SCENE_LOADED;
    cd_event_emit(cd_kernel_get_events(engine), &evt);

    return true;
}

/* ============================================================================
 * Argument parsing
 * ============================================================================ */

static bool parse_args(int argc, char* argv[], cd_config_t* config) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) {
            config->headless = true;
        } else if (strncmp(argv[i], "--renderer=", 11) == 0) {
            const char* backend = argv[i] + 11;
            if (strcmp(backend, "opengl") == 0) {
                config->renderer = CD_RENDERER_OPENGL;
            } else if (strcmp(backend, "software") == 0) {
                config->renderer = CD_RENDERER_SOFTWARE;
            } else if (strcmp(backend, "null") == 0) {
                config->renderer = CD_RENDERER_NULL;
            } else if (strcmp(backend, "vulkan") == 0) {
                config->renderer = CD_RENDERER_VULKAN;
            } else {
                fprintf(stderr, "Error: unknown renderer backend: %s\n", backend);
                fprintf(stderr, "Valid options: opengl, software, null, vulkan\n");
                return false;
            }
        } else if (strcmp(argv[i], "--software-render") == 0) {
            config->software_render = true;
        } else if (strcmp(argv[i], "--project") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --project requires a path argument\n");
                return false;
            }
            i++;
            config->project_path = argv[i];
        } else if (strcmp(argv[i], "--mcp-token") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --mcp-token requires a token argument\n");
                return false;
            }
            i++;
            config->mcp_token = argv[i];
        } else if (strcmp(argv[i], "--mcp-socket") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --mcp-socket requires a path argument\n");
                return false;
            }
            i++;
            config->mcp_mode = CD_MCP_SOCKET;
            config->mcp_socket_path = argv[i];
        } else if (strcmp(argv[i], "--server") == 0) {
            config->dedicated_server = true;
            config->headless = true;
        } else if (strcmp(argv[i], "--net-host") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --net-host requires a port argument\n");
                return false;
            }
            i++;
            config->net_port = (uint16_t)atoi(argv[i]);
            config->dedicated_server = true;
            config->headless = true;
        } else if (strcmp(argv[i], "--net-max-clients") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --net-max-clients requires a number argument\n");
                return false;
            }
            i++;
            config->net_max_clients = (uint32_t)atoi(argv[i]);
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            /* Signal that we should exit cleanly (not an error) */
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
 * Main
 * ============================================================================ */

int main(int argc, char* argv[]) {
    fprintf(stderr, "Cadence Engine v%d.%d.%d\n",
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

    /* Default config */
    engine.config.headless       = false;
    engine.config.project_path   = ".";
    engine.config.mcp_mode       = CD_MCP_STDIO;
    engine.config.net_port       = 7777;
    engine.config.net_max_clients = 8;

    if (!parse_args(argc, argv, &engine.config)) {
        /* --help was shown, or there was a parse error.
         * For --help, exit 0; for errors, exit 1.
         * Simple heuristic: if argc > 1 and last arg was --help, exit 0. */
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 ||
                strcmp(argv[i], "-h") == 0) {
                return 0;
            }
        }
        return 1;
    }

    /* ---- Parse project.toml BEFORE engine init (BL-34: plugin config) --- */
    {
        char toml_path[1024];
        snprintf(toml_path, sizeof(toml_path), "%s/project.toml",
                 engine.config.project_path);
        cd_result_t pr = cd_project_load((cd_project_t*)cd_kernel_get_project(&engine), toml_path);
        if (pr == CD_OK) {
            CD_LOG_INFO("cadence", "Loaded project: %s v%s",
                   cd_kernel_get_project(&engine)->game.name[0] ? cd_kernel_get_project(&engine)->game.name : "(unnamed)",
                   cd_kernel_get_project(&engine)->game.version[0] ? cd_kernel_get_project(&engine)->game.version : "0.0.0");
        } else {
            CD_LOG_WARN("cadence", "No project.toml found at '%s' (non-fatal)",
                    toml_path);
        }
    }

    /* ---- Allocate MCP tool state early (before plugins load) ----------- */
    /* Plugins (e.g. scripting_lua) wire into mcp_tool_state during on_load,
     * which happens inside cd_engine_init().  Must exist before that. */
    {
        cd_kernel_shared_t* shared = (cd_kernel_shared_t*)engine.shared;
        shared->mcp_tool_state = (cd_mcp_tool_state_t*)calloc(1, sizeof(cd_mcp_tool_state_t));
        if (shared->mcp_tool_state) {
            cd_mcp_tool_state_init(shared->mcp_tool_state);
        }
    }

    /* ---- Initialize engine --------------------------------------------- */
    cd_result_t res = cd_engine_init(&engine);
    if (res != CD_OK) {
        CD_LOG_ERROR("cadence", "Engine initialization failed: %d", res);
        return 1;
    }
    cd_fx_state_init(cd_kernel_get_fx(&engine));

    /* ---- Initialize asset database ------------------------------------- */
    static cd_asset_db_t s_asset_db;
    cd_asset_db_init(&s_asset_db);
    res = cd_asset_db_scan(&s_asset_db, engine.config.project_path);
    if (res == CD_OK) {
        engine.asset_db = &s_asset_db;
        CD_LOG_INFO("cadence", "Asset database scanned (%u assets in '%s')",
                s_asset_db.count, engine.config.project_path);
    } else {
        CD_LOG_WARN("cadence", "Asset DB scan failed (non-fatal): %d", res);
    }

    /* ---- Initialize MCP server (heap-allocated, decoupled from kernel) --- */
    cd_mcp_server_t* mcp = (cd_mcp_server_t*)calloc(1, sizeof(cd_mcp_server_t));
    if (!mcp) {
        CD_LOG_WARN("cadence", "MCP server alloc failed (non-fatal)");
    } else {
        res = cd_mcp_server_init(mcp);
        if (res != CD_OK) {
            CD_LOG_WARN("cadence", "MCP server init failed: %d (non-fatal)", res);
            free(mcp);
            mcp = NULL;
        } else {
            cd_mcp_register_all_tools(mcp);
            cd_mcp_mark_async_tools(mcp);  /* P2-4: tag long-running tools */
            CD_LOG_INFO("cadence", "MCP server ready (%u tools on stdio)",
                    mcp->tool_count);
            ((cd_kernel_shared_t*)engine.shared)->mcp = mcp;

            /* Wire pre-tick context */
            s_mcp_ctx.mcp        = mcp;
            s_mcp_ctx.socket_srv = NULL;
            {
                cd_kernel_callbacks_t* cb = (cd_kernel_callbacks_t*)engine.callbacks;
                if (!cb) {
                    engine.callbacks = calloc(1, sizeof(cd_kernel_callbacks_t));
                    cb = (cd_kernel_callbacks_t*)engine.callbacks;
                }
                cb->pre_tick_fn       = mcp_pre_tick;
                cb->pre_tick_userdata = &s_mcp_ctx;
            }
        }
    }

    /* ---- Initialize local IPC transport (--mcp-socket) ------------------- */
    static cd_mcp_socket_server_t s_socket_srv;
    if (mcp && engine.config.mcp_socket_path != NULL) {
        res = cd_mcp_socket_init(&s_socket_srv, engine.config.mcp_socket_path);
        if (res == CD_OK) {
            s_mcp_ctx.socket_srv = &s_socket_srv;
            if (engine.config.mcp_token) {
                cd_mcp_socket_set_auth_token(&s_socket_srv, engine.config.mcp_token);
            }
            CD_LOG_INFO("cadence", "MCP socket transport ready on '%s'",
                    engine.config.mcp_socket_path);
        } else {
            CD_LOG_WARN("cadence", "MCP socket transport init failed: %d (non-fatal)",
                    res);
        }
    }

    /* ---- Initialize hot-reload (P0-2) ---------------------------------- */
    {
        cd_result_t hr_res = cd_hot_reload_init(cd_kernel_get_hot_reload(&engine), &engine);
        if (hr_res == CD_OK) {
            /* Watch project scripts directory for .lua changes */
            char scripts_dir[1024];
            snprintf(scripts_dir, sizeof(scripts_dir), "%s/scripts",
                     engine.config.project_path);
            cd_hot_reload_watch_dir(cd_kernel_get_hot_reload(&engine), scripts_dir,
                                    CD_HOT_RELOAD_SCRIPT);

            /* Watch project assets directory for asset changes */
            char assets_dir[1024];
            snprintf(assets_dir, sizeof(assets_dir), "%s/assets",
                     engine.config.project_path);
            cd_hot_reload_watch_dir(cd_kernel_get_hot_reload(&engine), assets_dir,
                                    CD_HOT_RELOAD_SCRIPT);

            /* Watch plugin search paths for C plugin DLL hot-reload */
            cd_hot_reload_watch_plugin_dirs(cd_kernel_get_hot_reload(&engine),
                                            &engine.plugins);

            CD_LOG_INFO("cadence", "Hot-reload initialized");
        } else {
            CD_LOG_WARN("cadence", "Hot-reload init failed: %d (non-fatal)",
                    hr_res);
        }
    }

    /* ---- Main loop ----------------------------------------------------- */
    if (engine.config.headless) {
        /* Load entry scene (needed for game logic in headless/server mode) */
        cd_load_entry_scene(&engine);

        /* Dedicated server: auto-host and enter play mode */
        if (engine.config.dedicated_server) {
            const cd_net_ext_api_t* net_api = cd_kernel_get_net_ext_api((cd_kernel_t*)&engine);
            cd_net_session_t* session = net_api ? (cd_net_session_t*)net_api->get_session(net_api->userdata) : NULL;
            if (session) {
                cd_result_t host_res = cd_net_session_host(
                    session, engine.config.net_port,
                    engine.config.net_max_clients, "Dedicated Server");
                if (host_res == CD_OK) {
                    session->server_is_dedicated = true;
                    if (net_api) net_api->set_active(true, net_api->userdata);
                    cd_engine_play((cd_kernel_t*)&engine);
                    CD_LOG_INFO("cadence", "Dedicated server started on port %d (max %d clients)",
                           (int)engine.config.net_port,
                           (int)engine.config.net_max_clients);
                } else {
                    CD_LOG_ERROR("cadence", "Failed to start dedicated server hosting: %d",
                            host_res);
                }
            } else {
                CD_LOG_ERROR("cadence", "Networking plugin not loaded -- cannot start dedicated server");
            }
        }

        CD_LOG_INFO("cadence", "Engine started (headless mode)");
        fflush(stderr);
        /* Run the engine loop — MCP polling happens inside cd_engine_tick() */
        res = cd_engine_run((cd_kernel_t*)&engine);
        if (res != CD_OK) {
            CD_LOG_ERROR("cadence", "Engine run error: %d", res);
        }
    } else {
        /* ---- Headed (windowed) mode ------------------------------------ */
        const cd_window_api_t* wapi = cd_kernel_get_window_api(&engine);
        if (!wapi) {
            CD_LOG_ERROR("cadence", "Windowed mode not available (no window API registered); exiting.");
        } else {
            /* 1. Initialize windowing subsystem */
            res = wapi->init(wapi->userdata);
            if (res != CD_OK) {
                CD_LOG_ERROR("cadence", "Window system init failed: %d", res);
                cd_engine_shutdown(&engine);
                return 1;
            }

            /* 2. Create window — use display settings from project.toml (S2.4) */
            cd_window_config_t win_cfg;
            memset(&win_cfg, 0, sizeof(win_cfg));
            win_cfg.title      = cd_project_display_title(cd_kernel_get_project(&engine));
            win_cfg.width      = cd_kernel_get_project(&engine)->display.width;
            win_cfg.height     = cd_kernel_get_project(&engine)->display.height;
            win_cfg.fullscreen = cd_kernel_get_project(&engine)->display.fullscreen;
            win_cfg.vsync      = cd_kernel_get_project(&engine)->display.vsync;
            win_cfg.resizable  = true;

            /* CLI window title override takes precedence */
            if (engine.config.window_title)
                win_cfg.title = engine.config.window_title;

            cd_window_t window;
            memset(&window, 0, sizeof(window));
            res = wapi->create(&window, &win_cfg, wapi->userdata);
            if (res != CD_OK) {
                CD_LOG_ERROR("cadence", "Window creation failed: %d", res);
                wapi->shutdown(wapi->userdata);
                cd_engine_shutdown(&engine);
                return 1;
            }
            engine.window = &window;

            /* 3. Init renderer HAL (OpenGL) -- GL context is now current */
            const cd_render_hal_t* hal = cd_render_hal_get();
            if (hal && hal->init) {
                void* native = wapi->get_native_handle(&window, wapi->userdata);
                uint32_t fb_w = 0, fb_h = 0;
                wapi->get_framebuffer_size(&window, &fb_w, &fb_h, wapi->userdata);
                res = hal->init(native, false, fb_w, fb_h);
                if (res != CD_OK) {
                    CD_LOG_ERROR("cadence", "Renderer HAL init failed: %d", res);
                    engine.window = NULL;
                    wapi->destroy(&window, wapi->userdata);
                    wapi->shutdown(wapi->userdata);
                    cd_engine_shutdown(&engine);
                    return 1;
                }
            }

            /* 3b. Load GLAD in the exe so editor code has GL function pointers.
             * The renderer_opengl DLL loads its own copy; this covers the exe's.
             * Skip for non-OpenGL renderers (e.g. Tellusim). */
            bool is_opengl_renderer = (wapi->get_proc_address != NULL
                                       && wapi->get_proc_address("glCreateShader", wapi->userdata) != NULL);
            if (is_opengl_renderer) {
                s_glad_wapi = wapi;
                gladLoadGLLoader((GLADloadproc)cd_glad_proc_wrapper);
            }

            /* 4. Load entry scene from project.toml (non-fatal) */
            cd_load_entry_scene(&engine);

#if defined(CD_HAS_CIMGUI)
            /* 5. Init editor (ImGui) -- non-fatal if fails.
             * Supported for OpenGL and Vulkan renderers. */
            cd_editor_t editor;
            {
                const char* backend_name = (hal && hal->get_backend_name)
                                            ? hal->get_backend_name() : "";
                bool is_vulkan = (backend_name[0] == 'V'); /* "Vulkan 1.3" */

                if (is_opengl_renderer || is_vulkan) {
                    cd_editor_init(&editor, (cd_kernel_t*)&engine);
                } else {
                    memset(&editor, 0, sizeof(editor));
                    CD_LOG_WARN("cadence", "Editor disabled (unsupported renderer: %s)",
                            backend_name);
                }
            }
#endif

            CD_LOG_INFO("cadence", "Engine started (windowed mode)");

            /* 6. Main loop */
            while (engine.running && !wapi->should_close(&window, wapi->userdata)) {
                wapi->poll_events(wapi->userdata);
                cd_engine_tick((cd_kernel_t*)&engine);

#if defined(CD_HAS_CIMGUI)
                if (editor.initialized) {
                    cd_editor_begin_frame(&editor);
                    cd_editor_draw_panels(&editor, (cd_kernel_t*)&engine);
                    cd_editor_end_frame(&editor);
                }
#endif

                wapi->swap_buffers(&window, wapi->userdata);
            }

            /* 7. Shutdown (reverse order) */
#if defined(CD_HAS_CIMGUI)
            cd_editor_shutdown(&editor);
#endif
            engine.window = NULL;
            wapi->destroy(&window, wapi->userdata);
            wapi->shutdown(wapi->userdata);
        }
    }

    /* ---- Shutdown ------------------------------------------------------ */
    /* Shut down hot-reload before engine (P0-2) */
    cd_hot_reload_shutdown(cd_kernel_get_hot_reload(&engine));

    /* Shut down socket transport before MCP server */
    if (s_mcp_ctx.socket_srv) {
        cd_mcp_socket_shutdown(s_mcp_ctx.socket_srv);
        s_mcp_ctx.socket_srv = NULL;
    }

    if (mcp && mcp->initialized) {
        cd_mcp_server_shutdown(mcp);
    }
    free(mcp);
    mcp = NULL;
    if (engine.shared) {
        cd_kernel_shared_t* shared = (cd_kernel_shared_t*)engine.shared;
        shared->mcp = NULL;
        free(shared->mcp_tool_state);
        shared->mcp_tool_state = NULL;
    }

    /* Free scene if we loaded one */
    if (engine.scene) {
        cd_scene_shutdown(engine.scene);
        free(engine.scene);
        engine.scene = NULL;
    }

    res = cd_engine_shutdown(&engine);
    if (res != CD_OK) {
        CD_LOG_ERROR("cadence", "Engine shutdown error: %d", res);
        return 1;
    }

    CD_LOG_INFO("cadence", "Engine shut down cleanly");
    return 0;
}
