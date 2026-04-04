#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
/* cd_mcp_gamespec_plan_tools.c - Cadence Engine MCP gamespec.plan/diff tools
 *
 * Task 15.11: gamespec.plan + gamespec.diff MCP tools
 *
 * Implements:
 *   - gamespec.plan : Shows what files would be generated without writing
 *   - gamespec.diff : Shows what changed between spec and on-disk files
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

#define CD_PLAN_NUM_SECTIONS 9

typedef struct {
    const char* name;
    const char* output_path;  /* Non-NULL for single-file sections */
    bool        is_multi;     /* True for multi-file sections */
} cd_plan_section_def_t;

static const cd_plan_section_def_t g_plan_sections[CD_PLAN_NUM_SECTIONS] = {
    { "states",      "scripts/game_state_machine.lua", false },
    { "mechanics",   NULL,                             true  },
    { "entities",    NULL,                             true  },
    { "levels",      NULL,                             true  },
    { "events",      "scripts/event_system.lua",       false },
    { "ui",          NULL,                             true  },
    { "triggers",    "scripts/trigger_system.lua",     false },
    { "audio",       "scripts/audio_manager.lua",      false },
    { "progression", "scripts/save_manager.lua",       false },
};

/* ============================================================================
 * Helpers
 * ============================================================================ */

static int find_plan_section_index(const char* name) {
    for (int i = 0; i < CD_PLAN_NUM_SECTIONS; i++) {
        if (strcmp(g_plan_sections[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * Resolve a spec_path that may use res:// URI scheme.
 */
static void resolve_plan_spec_path(const char* spec_path,
                                    const char* project_path,
                                    char* out_path, size_t out_size) {
    if (strncmp(spec_path, "res://", 6) == 0) {
        snprintf(out_path, out_size, "%s/%s", project_path, spec_path + 6);
    } else {
        snprintf(out_path, out_size, "%s", spec_path);
    }
}

/**
 * Check if a section filter includes the given section name.
 * If filter is NULL (no filtering), all sections are included.
 */
static bool section_in_filter(const cJSON* sections_filter,
                               const char* section_name) {
    if (sections_filter == NULL) return true;
    const cJSON* item = NULL;
    cJSON_ArrayForEach(item, sections_filter) {
        if (cJSON_IsString(item) && item->valuestring != NULL &&
            strcmp(item->valuestring, section_name) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * Get the cJSON section from the spec for a given section name.
 */
static const cJSON* get_spec_section(const cd_gamespec_t* spec,
                                      const char* name) {
    if (strcmp(name, "states") == 0)      return cd_gamespec_get_states(spec);
    if (strcmp(name, "mechanics") == 0)   return cd_gamespec_get_mechanics(spec);
    if (strcmp(name, "entities") == 0)    return cd_gamespec_get_entities(spec);
    if (strcmp(name, "levels") == 0)      return cd_gamespec_get_levels(spec);
    if (strcmp(name, "events") == 0)      return cd_gamespec_get_events(spec);
    if (strcmp(name, "ui") == 0)          return cd_gamespec_get_ui(spec);
    if (strcmp(name, "triggers") == 0)    return cd_gamespec_get_triggers(spec);
    if (strcmp(name, "audio") == 0)       return cd_gamespec_get_audio(spec);
    if (strcmp(name, "progression") == 0) return cd_gamespec_get_progression(spec);
    return NULL;
}

/**
 * Count children in a cJSON object (only object children, not scalars).
 */
static int count_object_children(const cJSON* obj) {
    if (obj == NULL) return 0;
    int count = 0;
    const cJSON* child = NULL;
    cJSON_ArrayForEach(child, obj) {
        if (child->string != NULL) {
            count++;
        }
    }
    return count;
}

/**
 * Build multi-file path list for a section, returning a cJSON array.
 * Also sets *out_count to the number of files.
 */
static cJSON* build_multi_file_list(const cd_gamespec_t* spec,
                                     const char* section_name,
                                     int* out_count) {
    cJSON* arr = cJSON_CreateArray();
    int count = 0;

    if (strcmp(section_name, "mechanics") == 0) {
        const cJSON* section = cd_gamespec_get_mechanics(spec);
        const cJSON* item = NULL;
        cJSON_ArrayForEach(item, section) {
            if (item->string != NULL) {
                char path[512];
                snprintf(path, sizeof(path),
                         "scripts/mechanics/mechanic_%s.lua", item->string);
                cJSON_AddItemToArray(arr, cJSON_CreateString(path));
                count++;
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
                count++;
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
                count++;
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
                    count++;
                }
            }
        }
        /* Always include the framework file */
        cJSON_AddItemToArray(arr, cJSON_CreateString("scripts/ui/ui_framework.lua"));
        count++;
    }

    if (out_count) *out_count = count;
    return arr;
}

/**
 * Read an entire file into a malloc'd buffer. Returns NULL on failure.
 */
static char* read_file_contents(const char* filepath, size_t* out_len) {
    FILE* fp = fopen(filepath, "rb");
    if (fp == NULL) return NULL;

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (len < 0) { fclose(fp); return NULL; }

    char* buf = (char*)malloc((size_t)len + 1);
    if (buf == NULL) { fclose(fp); return NULL; }

    size_t read = fread(buf, 1, (size_t)len, fp);
    fclose(fp);

    buf[read] = '\0';
    if (out_len) *out_len = read;
    return buf;
}

/* ============================================================================
 * gamespec.plan handler
 *
 * Input:
 *   { "spec_path": "path/to/game.gamespec.toml",
 *     "sections": ["states", "mechanics", ...] }   (optional)
 *
 * Output:
 *   { "status": "ok",
 *     "plan": { section_name: { "would_generate": ..., "section_found": bool, "count": N }, ... },
 *     "total_files": N,
 *     "sections_found": N,
 *     "sections_missing": N }
 * ============================================================================ */

static cJSON* cd_mcp_handle_gamespec_plan(
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

    /* Extract optional sections filter */
    const cJSON* sections_filter = cJSON_GetObjectItemCaseSensitive(
        params, "sections");
    if (sections_filter != NULL && !cJSON_IsArray(sections_filter)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "'sections' must be an array of strings";
        return NULL;
    }

    /* Validate section names in filter */
    if (sections_filter != NULL) {
        const cJSON* sec = NULL;
        cJSON_ArrayForEach(sec, sections_filter) {
            if (!cJSON_IsString(sec) || sec->valuestring == NULL) {
                *error_code = CD_JSONRPC_INVALID_PARAMS;
                *error_msg  = "Each section must be a string";
                return NULL;
            }
            if (find_plan_section_index(sec->valuestring) < 0) {
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
    resolve_plan_spec_path(spec_path_item->valuestring, project_path,
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

    /* Build plan */
    cJSON* plan = cJSON_CreateObject();
    int total_files = 0;
    int sections_found = 0;
    int sections_missing = 0;

    for (int i = 0; i < CD_PLAN_NUM_SECTIONS; i++) {
        const char* name = g_plan_sections[i].name;

        if (!section_in_filter(sections_filter, name)) continue;

        const cJSON* section_data = get_spec_section(&spec, name);
        bool found = (section_data != NULL);

        cJSON* entry = cJSON_CreateObject();

        if (g_plan_sections[i].is_multi) {
            /* Multi-file section */
            int count = 0;
            cJSON* file_list = build_multi_file_list(&spec, name, &count);
            cJSON_AddItemToObject(entry, "would_generate", file_list);
            cJSON_AddBoolToObject(entry, "section_found", found);
            cJSON_AddNumberToObject(entry, "count", count);
            total_files += count;
        } else {
            /* Single-file section */
            cJSON_AddStringToObject(entry, "would_generate",
                                     g_plan_sections[i].output_path);
            cJSON_AddBoolToObject(entry, "section_found", found);
            if (found) total_files++;
        }

        if (found) {
            sections_found++;
        } else {
            sections_missing++;
        }

        cJSON_AddItemToObject(plan, name, entry);
    }

    /* Build result */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        cJSON_Delete(plan);
        cd_gamespec_free(&spec);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddItemToObject(result, "plan", plan);
    cJSON_AddNumberToObject(result, "total_files", total_files);
    cJSON_AddNumberToObject(result, "sections_found", sections_found);
    cJSON_AddNumberToObject(result, "sections_missing", sections_missing);

    cd_gamespec_free(&spec);
    return result;
}

/* ============================================================================
 * Diff helpers for single-file and multi-file sections
 * ============================================================================ */

/**
 * Generate content for a single-file section to a buffer.
 * Returns CD_OK on success. Caller must free *out_buf.
 */
static cd_result_t generate_single_to_buffer(const cd_gamespec_t* spec,
                                              const char* section_name,
                                              char** out_buf,
                                              size_t* out_len) {
    if (strcmp(section_name, "states") == 0)
        return cd_composer_generate_states_to_buffer(spec, out_buf, out_len);
    if (strcmp(section_name, "events") == 0)
        return cd_composer_generate_events_to_buffer(spec, out_buf, out_len);
    if (strcmp(section_name, "triggers") == 0)
        return cd_composer_generate_triggers_to_buffer(spec, out_buf, out_len);
    if (strcmp(section_name, "audio") == 0)
        return cd_composer_generate_audio_to_buffer(spec, out_buf, out_len);
    if (strcmp(section_name, "progression") == 0)
        return cd_composer_generate_progression_to_buffer(spec, out_buf, out_len);
    return CD_ERR_NOTFOUND;
}

/**
 * Diff a single-file section: compare generated content vs file on disk.
 * Returns "new", "unchanged", or "modified".
 */
static const char* diff_single_file(const cd_gamespec_t* spec,
                                     const char* section_name,
                                     const char* output_dir) {
    int idx = find_plan_section_index(section_name);
    if (idx < 0 || g_plan_sections[idx].output_path == NULL) return "new";

    /* Build full path to the expected output file */
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/%s",
             output_dir, g_plan_sections[idx].output_path);

    /* Read existing file */
    size_t existing_len = 0;
    char* existing = read_file_contents(filepath, &existing_len);
    if (existing == NULL) {
        return "new";
    }

    /* Generate to buffer */
    char* generated = NULL;
    size_t gen_len = 0;
    cd_result_t res = generate_single_to_buffer(spec, section_name,
                                                 &generated, &gen_len);
    if (res != CD_OK) {
        free(existing);
        return "new";
    }

    /* Compare */
    const char* status;
    if (gen_len == existing_len &&
        memcmp(generated, existing, gen_len) == 0) {
        status = "unchanged";
    } else {
        status = "modified";
    }

    free(existing);
    free(generated);
    return status;
}

/**
 * Generate content for a single item in a multi-file section.
 * Returns CD_OK on success. Caller must free *out_buf.
 */
static cd_result_t generate_multi_item_to_buffer(const cd_gamespec_t* spec,
                                                   const char* section_name,
                                                   const char* item_name,
                                                   char** out_buf,
                                                   size_t* out_len) {
    if (strcmp(section_name, "mechanics") == 0)
        return cd_composer_generate_mechanics_to_buffer(spec, item_name,
                                                         out_buf, out_len);
    if (strcmp(section_name, "entities") == 0)
        return cd_composer_generate_entity_to_buffer(spec, item_name,
                                                      out_buf, out_len);
    if (strcmp(section_name, "levels") == 0)
        return cd_composer_generate_level_to_buffer(spec, item_name,
                                                     out_buf, out_len);
    if (strcmp(section_name, "ui") == 0)
        return cd_composer_generate_ui_screen_to_buffer(spec, item_name,
                                                         out_buf, out_len);
    return CD_ERR_NOTFOUND;
}

/**
 * Get the file path pattern for a multi-file section item.
 */
static void get_multi_item_path(const char* section_name,
                                 const char* item_name,
                                 char* out_path, size_t out_size) {
    if (strcmp(section_name, "mechanics") == 0) {
        snprintf(out_path, out_size,
                 "scripts/mechanics/mechanic_%s.lua", item_name);
    } else if (strcmp(section_name, "entities") == 0) {
        snprintf(out_path, out_size,
                 "prefabs/entity_%s.prefab.toml", item_name);
    } else if (strcmp(section_name, "levels") == 0) {
        snprintf(out_path, out_size,
                 "scenes/%s.toml", item_name);
    } else if (strcmp(section_name, "ui") == 0) {
        snprintf(out_path, out_size,
                 "scripts/ui/ui_%s.lua", item_name);
    } else {
        out_path[0] = '\0';
    }
}

/**
 * Diff a multi-file section. Builds a cJSON object with:
 *   { "added": [...], "removed": [...], "modified": [...] }
 */
static cJSON* diff_multi_file(const cd_gamespec_t* spec,
                                const char* section_name,
                                const char* output_dir) {
    cJSON* result = cJSON_CreateObject();
    cJSON* added = cJSON_CreateArray();
    cJSON* removed = cJSON_CreateArray();
    cJSON* modified = cJSON_CreateArray();

    /* Get spec section items */
    const cJSON* section = get_spec_section(spec, section_name);

    /* For UI, iterate screens sub-object */
    const cJSON* items = section;
    if (strcmp(section_name, "ui") == 0 && section != NULL) {
        items = cJSON_GetObjectItemCaseSensitive(section, "screens");
    }

    /* Check each spec item against disk */
    const cJSON* item = NULL;
    if (items != NULL) {
        cJSON_ArrayForEach(item, items) {
            if (item->string == NULL) continue;

            char rel_path[512];
            get_multi_item_path(section_name, item->string,
                                 rel_path, sizeof(rel_path));

            char full_path[1024];
            snprintf(full_path, sizeof(full_path), "%s/%s",
                     output_dir, rel_path);

            /* Read existing file */
            size_t existing_len = 0;
            char* existing = read_file_contents(full_path, &existing_len);

            if (existing == NULL) {
                /* File doesn't exist on disk */
                cJSON_AddItemToArray(added,
                    cJSON_CreateString(item->string));
            } else {
                /* File exists - generate and compare */
                char* generated = NULL;
                size_t gen_len = 0;
                cd_result_t res = generate_multi_item_to_buffer(
                    spec, section_name, item->string,
                    &generated, &gen_len);

                if (res == CD_OK && generated != NULL) {
                    if (gen_len != existing_len ||
                        memcmp(generated, existing, gen_len) != 0) {
                        cJSON_AddItemToArray(modified,
                            cJSON_CreateString(item->string));
                    }
                    free(generated);
                }
                free(existing);
            }
        }
    }

    cJSON_AddItemToObject(result, "added", added);
    cJSON_AddItemToObject(result, "removed", removed);
    cJSON_AddItemToObject(result, "modified", modified);

    return result;
}

/* ============================================================================
 * gamespec.diff handler
 *
 * Input:
 *   { "spec_path": "path/to/game.gamespec.toml",
 *     "sections": ["states", "mechanics", ...] }   (optional)
 *
 * Output:
 *   { "status": "ok",
 *     "changes": { section_name: "new"|"unchanged"|"modified"|{...}, ... },
 *     "summary": { "new": N, "modified": N, "unchanged": N, "removed": N } }
 * ============================================================================ */

static cJSON* cd_mcp_handle_gamespec_diff(
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

    /* Extract optional sections filter */
    const cJSON* sections_filter = cJSON_GetObjectItemCaseSensitive(
        params, "sections");
    if (sections_filter != NULL && !cJSON_IsArray(sections_filter)) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "'sections' must be an array of strings";
        return NULL;
    }

    /* Validate section names in filter */
    if (sections_filter != NULL) {
        const cJSON* sec = NULL;
        cJSON_ArrayForEach(sec, sections_filter) {
            if (!cJSON_IsString(sec) || sec->valuestring == NULL) {
                *error_code = CD_JSONRPC_INVALID_PARAMS;
                *error_msg  = "Each section must be a string";
                return NULL;
            }
            if (find_plan_section_index(sec->valuestring) < 0) {
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
    resolve_plan_spec_path(spec_path_item->valuestring, project_path,
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

    /* Build changes and summary */
    cJSON* changes = cJSON_CreateObject();
    int summary_new = 0;
    int summary_modified = 0;
    int summary_unchanged = 0;
    int summary_removed = 0;

    const char* output_dir = project_path;

    for (int i = 0; i < CD_PLAN_NUM_SECTIONS; i++) {
        const char* name = g_plan_sections[i].name;

        if (!section_in_filter(sections_filter, name)) continue;

        const cJSON* section_data = get_spec_section(&spec, name);

        if (section_data == NULL) {
            /* Section not in spec - mark as unchanged (nothing to generate) */
            cJSON_AddStringToObject(changes, name, "unchanged");
            summary_unchanged++;
            continue;
        }

        if (g_plan_sections[i].is_multi) {
            /* Multi-file section */
            cJSON* diff = diff_multi_file(&spec, name, output_dir);

            /* Count added/modified for summary */
            cJSON* added_arr = cJSON_GetObjectItemCaseSensitive(diff, "added");
            cJSON* modified_arr = cJSON_GetObjectItemCaseSensitive(diff, "modified");
            cJSON* removed_arr = cJSON_GetObjectItemCaseSensitive(diff, "removed");

            int n_added = cJSON_GetArraySize(added_arr);
            int n_modified = cJSON_GetArraySize(modified_arr);
            int n_removed = cJSON_GetArraySize(removed_arr);

            if (n_added == 0 && n_modified == 0 && n_removed == 0) {
                /* All items unchanged */
                cJSON_Delete(diff);
                cJSON_AddStringToObject(changes, name, "unchanged");
                summary_unchanged++;
            } else {
                cJSON_AddItemToObject(changes, name, diff);
                if (n_added > 0 || n_modified > 0) {
                    if (n_modified > 0) {
                        summary_modified++;
                    } else {
                        summary_new++;
                    }
                }
                if (n_removed > 0) {
                    summary_removed++;
                }
            }
        } else {
            /* Single-file section */
            const char* status = diff_single_file(&spec, name, output_dir);

            cJSON_AddStringToObject(changes, name, status);
            if (strcmp(status, "new") == 0) {
                summary_new++;
            } else if (strcmp(status, "modified") == 0) {
                summary_modified++;
            } else {
                summary_unchanged++;
            }
        }
    }

    /* Build result */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        cJSON_Delete(changes);
        cd_gamespec_free(&spec);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddItemToObject(result, "changes", changes);

    cJSON* summary = cJSON_CreateObject();
    cJSON_AddNumberToObject(summary, "new", summary_new);
    cJSON_AddNumberToObject(summary, "modified", summary_modified);
    cJSON_AddNumberToObject(summary, "unchanged", summary_unchanged);
    cJSON_AddNumberToObject(summary, "removed", summary_removed);
    cJSON_AddItemToObject(result, "summary", summary);

    cd_gamespec_free(&spec);
    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_gamespec_plan_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "gamespec.plan",
        cd_mcp_handle_gamespec_plan,
        "Preview what files would be generated from a game spec without writing",
        "{\"type\":\"object\",\"properties\":{"
        "\"spec_path\":{\"type\":\"string\",\"description\":\"Path to the .gamespec.toml file\"},"
        "\"sections\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Optional filter: only plan these sections\"}"
        "},\"required\":[\"spec_path\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "gamespec.diff",
        cd_mcp_handle_gamespec_diff,
        "Show differences between a game spec and existing on-disk files",
        "{\"type\":\"object\",\"properties\":{"
        "\"spec_path\":{\"type\":\"string\",\"description\":\"Path to the .gamespec.toml file\"},"
        "\"sections\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Optional filter: only diff these sections\"}"
        "},\"required\":[\"spec_path\"]}");
    if (res != CD_OK) return res;

    return CD_OK;
}
