#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
/* cd_mcp_gamespec_tools.c - Cadence Engine MCP gamespec tool handlers
 *
 * Task 10.7: gamespec.validate MCP tool
 *
 * Implements:
 *   - gamespec.validate : Parse and validate a .gamespec.toml, reporting
 *                         missing required fields, undefined references,
 *                         and warnings.
 *
 * Validation checks:
 *   1. Required sections: [game] must exist
 *   2. Required fields: game.name, game.genre
 *   3. State references: states.initial must reference a defined state
 *   4. State transitions: transition targets must reference defined states
 *   5. Mechanic applies_to: referenced entities must be defined
 *   6. Mechanic depends_on: referenced mechanics must be defined
 *   7. Entity prefab: warns if prefab path is empty
 *   8. Level entity refs: layout entity references checked against entities
 *   9. Missing sections: warns if common sections are absent
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
#include <stdarg.h>

/* ============================================================================
 * Validation context — accumulates errors and warnings
 * ============================================================================ */

#define CD_VALIDATE_MAX_MESSAGES 64
#define CD_VALIDATE_MSG_LEN     256

typedef struct {
    char     errors[CD_VALIDATE_MAX_MESSAGES][CD_VALIDATE_MSG_LEN];
    uint32_t error_count;
    char     warnings[CD_VALIDATE_MAX_MESSAGES][CD_VALIDATE_MSG_LEN];
    uint32_t warning_count;
} cd_validate_ctx_t;

static void validate_error(cd_validate_ctx_t* ctx, const char* fmt, ...) {
    if (ctx->error_count >= CD_VALIDATE_MAX_MESSAGES) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(ctx->errors[ctx->error_count], CD_VALIDATE_MSG_LEN, fmt, args);
    va_end(args);
    ctx->error_count++;
}

static void validate_warning(cd_validate_ctx_t* ctx, const char* fmt, ...) {
    if (ctx->warning_count >= CD_VALIDATE_MAX_MESSAGES) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(ctx->warnings[ctx->warning_count], CD_VALIDATE_MSG_LEN, fmt, args);
    va_end(args);
    ctx->warning_count++;
}

/* ============================================================================
 * Validation: [game] section
 * ============================================================================ */

static void validate_game_section(const cd_gamespec_t* spec,
                                   cd_validate_ctx_t* ctx) {
    const cJSON* game = cd_gamespec_get_game(spec);
    if (game == NULL) {
        validate_error(ctx, "[game] section is required");
        return;
    }

    const cJSON* name = cJSON_GetObjectItemCaseSensitive(game, "name");
    if (name == NULL || !cJSON_IsString(name) ||
        name->valuestring[0] == '\0') {
        validate_error(ctx, "[game].name is required");
    }

    const cJSON* genre = cJSON_GetObjectItemCaseSensitive(game, "genre");
    if (genre == NULL || !cJSON_IsString(genre) ||
        genre->valuestring[0] == '\0') {
        validate_error(ctx, "[game].genre is required");
    }
}

/* ============================================================================
 * Validation: [states] section
 * ============================================================================ */

static void validate_states_section(const cd_gamespec_t* spec,
                                     cd_validate_ctx_t* ctx) {
    const cJSON* states = cd_gamespec_get_states(spec);
    if (states == NULL) {
        validate_warning(ctx, "[states] section not defined");
        return;
    }

    /* Check initial state references a defined state */
    const cJSON* initial = cJSON_GetObjectItemCaseSensitive(states, "initial");
    if (initial == NULL || !cJSON_IsString(initial)) {
        validate_error(ctx, "[states].initial is required");
    } else {
        const cJSON* state_def = cJSON_GetObjectItemCaseSensitive(
            states, initial->valuestring);
        if (state_def == NULL || !cJSON_IsObject(state_def)) {
            validate_error(ctx,
                "[states].initial = \"%s\" references undefined state",
                initial->valuestring);
        }
    }

    /* Check transition targets reference defined states */
    const cJSON* state = NULL;
    cJSON_ArrayForEach(state, states) {
        if (!cJSON_IsObject(state)) continue;
        const char* state_name = state->string;
        if (state_name == NULL) continue;

        const cJSON* transitions = cJSON_GetObjectItemCaseSensitive(
            state, "transitions");
        if (transitions == NULL || !cJSON_IsObject(transitions)) continue;

        const cJSON* trans = NULL;
        cJSON_ArrayForEach(trans, transitions) {
            if (!cJSON_IsObject(trans)) continue;
            const cJSON* target = cJSON_GetObjectItemCaseSensitive(
                trans, "target");
            if (target == NULL || !cJSON_IsString(target)) continue;

            /* "quit" is a terminal state, always valid */
            if (strcmp(target->valuestring, "quit") == 0) continue;

            const cJSON* target_def = cJSON_GetObjectItemCaseSensitive(
                states, target->valuestring);
            if (target_def == NULL || !cJSON_IsObject(target_def)) {
                validate_error(ctx,
                    "[states.%s.transitions.%s] target \"%s\" is undefined",
                    state_name, trans->string, target->valuestring);
            }
        }
    }
}

/* ============================================================================
 * Validation: [mechanics] section
 * ============================================================================ */

static void validate_mechanics_section(const cd_gamespec_t* spec,
                                        cd_validate_ctx_t* ctx) {
    const cJSON* mechanics = cd_gamespec_get_mechanics(spec);
    if (mechanics == NULL) return; /* optional */

    const cJSON* entities = cd_gamespec_get_entities(spec);
    const cJSON* mechanic = NULL;

    cJSON_ArrayForEach(mechanic, mechanics) {
        if (!cJSON_IsObject(mechanic)) continue;
        const char* mech_name = mechanic->string;
        if (mech_name == NULL) continue;

        /* Check applies_to references */
        const cJSON* applies = cJSON_GetObjectItemCaseSensitive(
            mechanic, "applies_to");
        if (applies != NULL && cJSON_IsArray(applies) && entities != NULL) {
            const cJSON* entity_ref = NULL;
            cJSON_ArrayForEach(entity_ref, applies) {
                if (!cJSON_IsString(entity_ref)) continue;
                const cJSON* entity_def = cJSON_GetObjectItemCaseSensitive(
                    entities, entity_ref->valuestring);
                if (entity_def == NULL) {
                    validate_error(ctx,
                        "[mechanics.%s].applies_to references "
                        "undefined entity \"%s\"",
                        mech_name, entity_ref->valuestring);
                }
            }
        }

        /* Check depends_on references */
        const cJSON* depends = cJSON_GetObjectItemCaseSensitive(
            mechanic, "depends_on");
        if (depends != NULL && cJSON_IsArray(depends)) {
            const cJSON* dep_ref = NULL;
            cJSON_ArrayForEach(dep_ref, depends) {
                if (!cJSON_IsString(dep_ref)) continue;
                const cJSON* dep_def = cJSON_GetObjectItemCaseSensitive(
                    mechanics, dep_ref->valuestring);
                if (dep_def == NULL) {
                    validate_error(ctx,
                        "[mechanics.%s].depends_on references "
                        "undefined mechanic \"%s\"",
                        mech_name, dep_ref->valuestring);
                }
            }
        }
    }
}

/* ============================================================================
 * Validation: [entities] section
 * ============================================================================ */

static void validate_entities_section(const cd_gamespec_t* spec,
                                       cd_validate_ctx_t* ctx) {
    const cJSON* entities = cd_gamespec_get_entities(spec);
    if (entities == NULL) return; /* optional */

    const cJSON* entity = NULL;
    cJSON_ArrayForEach(entity, entities) {
        if (!cJSON_IsObject(entity)) continue;
        const char* ent_name = entity->string;
        if (ent_name == NULL) continue;

        /* Warn if no prefab defined */
        const cJSON* prefab = cJSON_GetObjectItemCaseSensitive(
            entity, "prefab");
        if (prefab == NULL || !cJSON_IsString(prefab) ||
            prefab->valuestring[0] == '\0') {
            validate_warning(ctx,
                "[entities.%s] has no prefab path defined", ent_name);
        }
    }
}

/* ============================================================================
 * Validation: [levels] section — layout entity references
 * ============================================================================ */

static void validate_levels_section(const cd_gamespec_t* spec,
                                     cd_validate_ctx_t* ctx) {
    const cJSON* levels = cd_gamespec_get_levels(spec);
    if (levels == NULL) return; /* optional */

    const cJSON* entities = cd_gamespec_get_entities(spec);
    const cJSON* level = NULL;

    cJSON_ArrayForEach(level, levels) {
        if (!cJSON_IsObject(level)) continue;
        const char* level_name = level->string;
        if (level_name == NULL) continue;

        const cJSON* layout = cJSON_GetObjectItemCaseSensitive(
            level, "layout");
        if (layout == NULL || !cJSON_IsArray(layout)) continue;

        const cJSON* entry = NULL;
        cJSON_ArrayForEach(entry, layout) {
            if (!cJSON_IsObject(entry)) continue;
            const cJSON* entity_ref = cJSON_GetObjectItemCaseSensitive(
                entry, "entity");
            if (entity_ref == NULL || !cJSON_IsString(entity_ref)) continue;

            if (entities != NULL) {
                const cJSON* entity_def = cJSON_GetObjectItemCaseSensitive(
                    entities, entity_ref->valuestring);
                if (entity_def == NULL) {
                    validate_warning(ctx,
                        "[levels.%s.layout] references entity \"%s\" "
                        "not defined in [entities]",
                        level_name, entity_ref->valuestring);
                }
            }
        }
    }
}

/* ============================================================================
 * Validation: missing common sections warnings
 * ============================================================================ */

static void validate_common_sections(const cd_gamespec_t* spec,
                                      cd_validate_ctx_t* ctx) {
    if (cd_gamespec_get_entities(spec) == NULL) {
        validate_warning(ctx, "[entities] section not defined");
    }
    if (cd_gamespec_get_input(spec) == NULL) {
        validate_warning(ctx, "[input] section not defined");
    }
}

/* ============================================================================
 * Run all validations
 * ============================================================================ */

static void validate_spec(const cd_gamespec_t* spec,
                            cd_validate_ctx_t* ctx) {
    validate_game_section(spec, ctx);
    validate_states_section(spec, ctx);
    validate_mechanics_section(spec, ctx);
    validate_entities_section(spec, ctx);
    validate_levels_section(spec, ctx);
    validate_common_sections(spec, ctx);
}

/* ============================================================================
 * gamespec.validate handler
 *
 * Input:  { "spec": "<toml string>" }
 *    or:  { "filepath": "path/to/game.gamespec.toml" }
 * Output: { "valid": true/false, "errors": [...], "warnings": [...] }
 * ============================================================================ */

static cJSON* cd_mcp_handle_gamespec_validate(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel; /* validation doesn't need kernel state */

    if (params == NULL) {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Missing parameters";
        return NULL;
    }

    /* Parse spec from either "spec" string or "filepath" */
    cd_gamespec_t spec;
    cd_result_t res;

    const cJSON* spec_item = cJSON_GetObjectItemCaseSensitive(params, "spec");
    const cJSON* filepath_item = cJSON_GetObjectItemCaseSensitive(params, "filepath");

    if (spec_item != NULL && cJSON_IsString(spec_item) &&
        spec_item->valuestring != NULL) {
        res = cd_gamespec_load_string(&spec, spec_item->valuestring,
                                       strlen(spec_item->valuestring));
    } else if (filepath_item != NULL && cJSON_IsString(filepath_item) &&
               filepath_item->valuestring != NULL &&
               filepath_item->valuestring[0] != '\0') {
        res = cd_gamespec_load(&spec, filepath_item->valuestring);
    } else {
        *error_code = CD_JSONRPC_INVALID_PARAMS;
        *error_msg  = "Must provide 'spec' (TOML string) or 'filepath'";
        return NULL;
    }

    if (res != CD_OK) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        if (res == CD_ERR_IO) {
            *error_msg = "File not found or cannot be read";
        } else if (res == CD_ERR_PARSE) {
            *error_msg = "Failed to parse game spec TOML";
        } else {
            *error_msg = "Failed to load game spec";
        }
        return NULL;
    }

    /* Run validations */
    cd_validate_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    validate_spec(&spec, &ctx);

    /* Build response */
    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        cd_gamespec_free(&spec);
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON_AddBoolToObject(result, "valid", ctx.error_count == 0);

    /* Errors array */
    cJSON* errors_arr = cJSON_CreateArray();
    for (uint32_t i = 0; i < ctx.error_count; i++) {
        cJSON_AddItemToArray(errors_arr, cJSON_CreateString(ctx.errors[i]));
    }
    cJSON_AddItemToObject(result, "errors", errors_arr);

    /* Warnings array */
    cJSON* warnings_arr = cJSON_CreateArray();
    for (uint32_t i = 0; i < ctx.warning_count; i++) {
        cJSON_AddItemToArray(warnings_arr,
                              cJSON_CreateString(ctx.warnings[i]));
    }
    cJSON_AddItemToObject(result, "warnings", warnings_arr);

    cd_gamespec_free(&spec);
    return result;
}

/* ============================================================================
 * gamespec.listMechanics handler
 *
 * Input:  {} (no params)
 * Output: { "mechanics": [ { name, description, tags, param_schema }, ... ] }
 * ============================================================================ */

static cJSON* cd_mcp_handle_gamespec_list_mechanics(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel;
    (void)params;

    uint32_t count = 0;
    const cd_mechanic_template_t* templates = cd_mechanic_templates_builtin(&count);

    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON* arr = cJSON_CreateArray();
    for (uint32_t i = 0; i < count; i++) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", templates[i].name);
        cJSON_AddStringToObject(item, "description", templates[i].description);
        cJSON_AddStringToObject(item, "tags", templates[i].tags);
        /* Parse param_schema from string to embed as object */
        cJSON* schema = cJSON_Parse(templates[i].param_schema);
        if (schema != NULL) {
            cJSON_AddItemToObject(item, "param_schema", schema);
        } else {
            cJSON_AddStringToObject(item, "param_schema", templates[i].param_schema);
        }
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(result, "mechanics", arr);

    return result;
}

/* ============================================================================
 * gamespec.listTriggers handler
 *
 * Input:  {} (no params)
 * Output: { "triggers": [ { name, description, param_schema }, ... ] }
 * ============================================================================ */

static cJSON* cd_mcp_handle_gamespec_list_triggers(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel;
    (void)params;

    uint32_t count = 0;
    const cd_trigger_template_t* templates = cd_trigger_templates_builtin(&count);

    cJSON* result = cJSON_CreateObject();
    if (result == NULL) {
        *error_code = CD_JSONRPC_INTERNAL_ERROR;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cJSON* arr = cJSON_CreateArray();
    for (uint32_t i = 0; i < count; i++) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", templates[i].name);
        cJSON_AddStringToObject(item, "description", templates[i].description);
        /* Parse param_schema from string to embed as object */
        cJSON* schema = cJSON_Parse(templates[i].param_schema);
        if (schema != NULL) {
            cJSON_AddItemToObject(item, "param_schema", schema);
        } else {
            cJSON_AddStringToObject(item, "param_schema", templates[i].param_schema);
        }
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(result, "triggers", arr);

    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_gamespec_tools(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "gamespec.validate",
        cd_mcp_handle_gamespec_validate,
        "Validate a game spec TOML, reporting errors and warnings",
        "{\"type\":\"object\",\"properties\":{"
        "\"spec\":{\"type\":\"string\",\"description\":\"Inline TOML string to validate\"},"
        "\"filepath\":{\"type\":\"string\",\"description\":\"Path to a .gamespec.toml file\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "gamespec.listMechanics",
        cd_mcp_handle_gamespec_list_mechanics,
        "List all built-in mechanic templates with names, descriptions, tags, and parameter schemas",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "gamespec.listTriggers",
        cd_mcp_handle_gamespec_list_triggers,
        "List all built-in trigger templates with names, descriptions, and parameter schemas",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    return CD_OK;
}
