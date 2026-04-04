#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
/* cd_mcp_compose_tools.c - Cadence Engine MCP gamespec.compose tool handler
 *
 * Task 15.10: gamespec.compose MCP tool
 *
 * Implements:
 *   - gamespec.compose : Call all 9 composer generators to produce a complete
 *                        game from a Game Spec. Supports full and incremental modes.
 *
 * Modes:
 *   - "full"        : Regenerate all sections (missing sections are warnings)
 *   - "incremental" : Only regenerate specified sections
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_gamespec.h"
#include "cadence/cd_composer.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Section definitions
 * ============================================================================ */

#define CD_COMPOSE_MAX_MESSAGES 64
#define CD_COMPOSE_MSG_LEN     256
#define CD_COMPOSE_NUM_SECTIONS 9

typedef struct {
    const char* name;
    cd_result_t (*generate)(const cd_gamespec_t* spec, const char* output_dir);
    const char* output_path;  /* Single-file generators */
} cd_compose_section_def_t;

static const cd_compose_section_def_t g_section_defs[CD_COMPOSE_NUM_SECTIONS] = {
    { "states",      cd_composer_generate_states,      "scripts/game_state_machine.lua" },
    { "mechanics",   cd_composer_generate_mechanics,   NULL },
    { "entities",    cd_composer_generate_entities,     NULL },
    { "levels",      cd_composer_generate_levels,       NULL },
    { "events",      cd_composer_generate_events,       "scripts/event_system.lua" },
    { "ui",          cd_composer_generate_ui,           NULL },
    { "triggers",    cd_composer_generate_triggers,     "scripts/trigger_system.lua" },
    { "audio",       cd_composer_generate_audio,        "scripts/audio_manager.lua" },
    { "progression", cd_composer_generate_progression,  "scripts/save_manager.lua" },
};

/* ============================================================================
 * Helpers
 * ============================================================================ */

static int find_section_index(const char* name) {
    for (int i = 0; i < CD_COMPOSE_NUM_SECTIONS; i++) {
        if (strcmp(g_section_defs[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * Resolve a spec_path that may use res:// URI scheme.
 * Writes the resolved absolute path into out_path.
 */
static void resolve_spec_path(const char* spec_path,
                               const char* project_path,
                               char* out_path, size_t out_size) {
    if (strncmp(spec_path, "res://", 6) == 0) {
        snprintf(out_path, out_size, "%s/%s", project_path, spec_path + 6);
    } else {
        snprintf(out_path, out_size, "%s", spec_path);
    }
}

/**
 * Build the "generated" object for a single-file section.
 */
static void add_generated_single(cJSON* generated, const char* section_name,
                                  const char* path) {
    cJSON_AddStringToObject(generated, section_name, path);
}

/**
 * Build the "generated" array for a multi-file section by enumerating
 * the spec section keys to produce the expected file paths.
 */
static void add_generated_multi(cJSON* generated, const char* section_name,
                                 const cd_gamespec_t* spec) {
    cJSON* arr = cJSON_CreateArray();

    if (strcmp(section_name, "mechanics") == 0) {
        const cJSON* section = cd_gamespec_get_mechanics(spec);
        const cJSON* item = NULL;
        cJSON_ArrayForEach(item, section) {
            if (item->string != NULL) {
                char path[512];
                snprintf(path, sizeof(path),
                         "scripts/mechanics/mechanic_%s.lua", item->string);
                cJSON_AddItemToArray(arr, cJSON_CreateString(path));
            }
        }
    } else if (strcmp(section_name, "entities") == 0) {
        const cJSON* section = cd_gamespec_get_entities(spec);
        const cJSON* item = NULL;
        cJSON_ArrayForEach(item, section) {
            if (item->string != NULL) {
                char path[512];
                snprintf(path, sizeof(path),
                         "prefabs/entity_%s.prefab.toml", item->string);
                cJSON_AddItemToArray(arr, cJSON_CreateString(path));
            }
        }
    } else if (strcmp(section_name, "levels") == 0) {
        const cJSON* section = cd_gamespec_get_levels(spec);
        const cJSON* item = NULL;
        cJSON_ArrayForEach(item, section) {
            if (item->string != NULL) {
                char path[512];
                snprintf(path, sizeof(path),
                         "scenes/%s.toml", item->string);
                cJSON_AddItemToArray(arr, cJSON_CreateString(path));
            }
        }
    } else if (strcmp(section_name, "ui") == 0) {
        const cJSON* section = cd_gamespec_get_ui(spec);
        const cJSON* screens = NULL;
        if (section != NULL) {
            screens = cJSON_GetObjectItemCaseSensitive(section, "screens");
        }
        if (screens != NULL) {
            const cJSON* item = NULL;
            cJSON_ArrayForEach(item, screens) {
                if (item->string != NULL) {
                    char path[512];
                    snprintf(path, sizeof(path),
                             "scripts/ui/ui_%s.lua", item->string);
                    cJSON_AddItemToArray(arr, cJSON_CreateString(path));
                }
            }
        }
        /* Also add the framework file */
        cJSON_AddItemToArray(arr, cJSON_CreateString("scripts/ui/ui_framework.lua"));
    }

    cJSON_AddItemToObject(generated, section_name, arr);
}

/* ============================================================================
 * gamespec.compose handler
 *
 * Input:
 *   { "spec_path": "res://game.gamespec.toml",
 *     "mode": "full" | "incremental",
 *     "sections": ["states", "mechanics", ...] }
 *
 * Output:
 *   { "status": "ok", "mode": "...", "output_dir": "...",
 *     "generated": { ... }, "errors": [...], "warnings": [...] }
 * ============================================================================ */

static cJSON* cd_mcp_handle_gamespec_compose(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    if (params == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing parameters";
        return NULL;
    }

    /* Extract spec_path (required) */
    const cJSON* spec_path_item = cJSON_GetObjectItemCaseSensitive(
        params, "spec_path");
    if (spec_path_item == NULL || !cJSON_IsString(spec_path_item) ||
        spec_path_item->valuestring == NULL ||
        spec_path_item->valuestring[0] == '\0') {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing required parameter 'spec_path'";
        return NULL;
    }

    /* Extract mode (optional, default "full") */
    const char* mode = "full";
    const cJSON* mode_item = cJSON_GetObjectItemCaseSensitive(params, "mode");
    if (mode_item != NULL && cJSON_IsString(mode_item) &&
        mode_item->valuestring != NULL) {
        mode = mode_item->valuestring;
    }

    if (strcmp(mode, "full") != 0 && strcmp(mode, "incremental") != 0) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Invalid mode: must be 'full' or 'incremental'";
        return NULL;
    }

    /* Extract sections (required for incremental mode) */
    const cJSON* sections_item = cJSON_GetObjectItemCaseSensitive(
        params, "sections");
    bool incremental = (strcmp(mode, "incremental") == 0);

    if (incremental) {
        if (sections_item == NULL || !cJSON_IsArray(sections_item) ||
            cJSON_GetArraySize(sections_item) == 0) {
            *error_code = CD_JSONRPC_INVALID_PARAMS;
            *error_msg  = "Incremental mode requires non-empty 'sections' array";
            return NULL;
        }

        /* Validate all section names */
        const cJSON* sec = NULL;
        cJSON_ArrayForEach(sec, sections_item) {
            if (!cJSON_IsString(sec) || sec->valuestring == NULL) {
                *error_code = CD_JSONRPC_INVALID_PARAMS;
                *error_msg  = "Each section must be a string";
                return NULL;
            }
            if (find_section_index(sec->valuestring) < 0) {
                *error_code = -32000;
                *error_msg  = "Invalid section name in 'sections' array";
                return NULL;
            }
        }
    }

    /* Resolve spec path */
    const char* project_path = ".";
    if (kernel != NULL && cd_kernel_get_config(kernel)->project_path != NULL) {
        project_path = cd_kernel_get_config(kernel)->project_path;
    }

    char resolved_path[1024];
    resolve_spec_path(spec_path_item->valuestring, project_path,
                      resolved_path, sizeof(resolved_path));

    /* Load the game spec */
    cd_gamespec_t spec;
    cd_result_t load_res = cd_gamespec_load(&spec, resolved_path);
    if (load_res != CD_OK) {
        *error_code = -32000;
        if (load_res == CD_ERR_IO) {
            *error_msg = "Game spec file not found or cannot be read";
        } else if (load_res == CD_ERR_PARSE) {
            *error_msg = "Failed to parse game spec TOML";
        } else {
            *error_msg = "Failed to load game spec";
        }
        return NULL;
    }

    /* Determine output directory */
    const char* output_dir = project_path;

    /* Build result arrays */
    char errors[CD_COMPOSE_MAX_MESSAGES][CD_COMPOSE_MSG_LEN];
    uint32_t error_count = 0;
    char warnings[CD_COMPOSE_MAX_MESSAGES][CD_COMPOSE_MSG_LEN];
    uint32_t warning_count = 0;

    cJSON* generated = cJSON_CreateObject();

    /* Determine which sections to run */
    bool run_section[CD_COMPOSE_NUM_SECTIONS];
    memset(run_section, 0, sizeof(run_section));

    if (incremental) {
        const cJSON* sec = NULL;
        cJSON_ArrayForEach(sec, sections_item) {
            int idx = find_section_index(sec->valuestring);
            if (idx >= 0) {
                run_section[idx] = true;
            }
        }
    } else {
        /* Full mode: run all sections */
        for (int i = 0; i < CD_COMPOSE_NUM_SECTIONS; i++) {
            run_section[i] = true;
        }
    }

    /* Run each selected generator */
    for (int i = 0; i < CD_COMPOSE_NUM_SECTIONS; i++) {
        if (!run_section[i]) continue;

        cd_result_t gen_res = g_section_defs[i].generate(&spec, output_dir);

        if (gen_res == CD_OK) {
            /* Add generated file(s) to the response */
            if (g_section_defs[i].output_path != NULL) {
                /* Single-file generator */
                add_generated_single(generated, g_section_defs[i].name,
                                      g_section_defs[i].output_path);
            } else {
                /* Multi-file generator */
                add_generated_multi(generated, g_section_defs[i].name,
                                     &spec);
            }
        } else if (gen_res == CD_ERR_NOTFOUND) {
            /* Section not found in spec - warning */
            if (warning_count < CD_COMPOSE_MAX_MESSAGES) {
                snprintf(warnings[warning_count], CD_COMPOSE_MSG_LEN,
                         "No [%s] section found in game spec",
                         g_section_defs[i].name);
                warning_count++;
            }
        } else {
            /* Other error */
            if (error_count < CD_COMPOSE_MAX_MESSAGES) {
                snprintf(errors[error_count], CD_COMPOSE_MSG_LEN,
                         "Failed to generate %s (error %d)",
                         g_section_defs[i].name, (int)gen_res);
                error_count++;
            }
        }
    }

    /* Write composer manifest (Task 15.12) */
    {
        cd_result_t manifest_res = cd_composer_manifest_write(
            output_dir, spec_path_item->valuestring, generated);
        if (manifest_res != CD_OK && warning_count < CD_COMPOSE_MAX_MESSAGES) {
            snprintf(warnings[warning_count], CD_COMPOSE_MSG_LEN,
                     "Failed to write composer manifest (error %d)",
                     (int)manifest_res);
            warning_count++;
        }
    }

    /* Generate project.toml startup manifest (WP-10F) */
    if (!incremental) {
        cd_result_t project_res = cd_composer_generate_project(
            &spec, output_dir);
        if (project_res == CD_OK) {
            add_generated_single(generated, "project", "project.toml");
        } else if (project_res == CD_ERR_NOTFOUND) {
            if (warning_count < CD_COMPOSE_MAX_MESSAGES) {
                snprintf(warnings[warning_count], CD_COMPOSE_MSG_LEN,
                         "No initial state found; project.toml not generated");
                warning_count++;
            }
        } else {
            if (error_count < CD_COMPOSE_MAX_MESSAGES) {
                snprintf(errors[error_count], CD_COMPOSE_MSG_LEN,
                         "Failed to generate project.toml (error %d)",
                         (int)project_res);
                error_count++;
            }
        }
    }

    /* Build JSON response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        cJSON_Delete(generated);
        cd_gamespec_free(&spec);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "mode", mode);
    cJSON_AddStringToObject(result, "output_dir", output_dir);
    cJSON_AddItemToObject(result, "generated", generated);

    /* Errors array */
    cJSON* errors_arr = cJSON_CreateArray();
    for (uint32_t i = 0; i < error_count; i++) {
        cJSON_AddItemToArray(errors_arr, cJSON_CreateString(errors[i]));
    }
    cJSON_AddItemToObject(result, "errors", errors_arr);

    /* Warnings array */
    cJSON* warnings_arr = cJSON_CreateArray();
    for (uint32_t i = 0; i < warning_count; i++) {
        cJSON_AddItemToArray(warnings_arr, cJSON_CreateString(warnings[i]));
    }
    cJSON_AddItemToObject(result, "warnings", warnings_arr);

    cd_gamespec_free(&spec);
    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_compose_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "gamespec.compose",
        cd_mcp_handle_gamespec_compose,
        "Generate a complete game from a game spec (full or incremental mode)",
        "{\"type\":\"object\",\"properties\":{"
        "\"spec_path\":{\"type\":\"string\",\"description\":\"Path to the .gamespec.toml file\"},"
        "\"mode\":{\"type\":\"string\",\"description\":\"Generation mode: full or incremental (default: full)\"},"
        "\"sections\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Sections to regenerate (required for incremental mode)\"}"
        "},\"required\":[\"spec_path\"]}");
    if (res != CD_OK) return res;

    return CD_OK;
}
