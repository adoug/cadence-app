/* cd_mcp_dialogue_tools.c - MCP tool handlers for dialogue and quest systems
 *
 * Tools:
 *   dialogue.start    - Start a dialogue by ID via DialogueManager
 *   dialogue.advance  - Advance or choose in active dialogue
 *   dialogue.status   - Get current dialogue state
 *   quest.status      - Get quest tracker state (all quests and objectives)
 *   quest.update      - Update a quest objective (for testing)
 *
 * These tools invoke Lua functions on the global _dialogue_manager and
 * _quest_tracker objects (created by user scripts).
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

void cd_mcp_dialogue_tools_set_lua_eval(void* fn) {
    s_lua_eval = (cd_lua_eval_fn)fn;
}

/* ============================================================================
 * dialogue.start - Start a dialogue by registered ID
 *
 * Params:
 *   id: string - dialogue identifier (must be registered with _dialogue_manager)
 *
 * Returns: { ok, speaker, text, choices }
 * ============================================================================ */

static cJSON* handle_dialogue_start(cd_kernel_t* kernel, const cJSON* params,
                                     int* error_code, const char** error_msg) {
    (void)kernel;

    if (!s_lua_eval) {
        *error_code = -32603;
        *error_msg = "Lua eval not initialized";
        return NULL;
    }

    const cJSON* id_json = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (!id_json || !cJSON_IsString(id_json)) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: id (string)";
        return NULL;
    }

    char lua_buf[2048];
    snprintf(lua_buf, sizeof(lua_buf),
        "local mgr = _G._dialogue_manager\n"
        "if not mgr then return '{\"error\":\"no dialogue manager\"}' end\n"
        "local state = _G._game_state or {}\n"
        "local dlg = mgr:start('%s', state)\n"
        "if not dlg then return '{\"error\":\"dialogue not found\"}' end\n"
        "local node = dlg:current()\n"
        "if not node then return '{\"ok\":true,\"ended\":true}' end\n"
        "local choices = dlg:get_choices(state)\n"
        "local c_str = ''\n"
        "for i, c in ipairs(choices) do\n"
        "  if i > 1 then c_str = c_str .. ',' end\n"
        "  c_str = c_str .. string.format('{\"index\":%%d,\"text\":\"%%s\"}', c.index, c.text:gsub('\"','\\\\\"'))\n"
        "end\n"
        "return string.format('{\"ok\":true,\"speaker\":\"%%s\",\"text\":\"%%s\",\"choices\":[%%s]}',\n"
        "  (node.speaker or ''):gsub('\"','\\\\\"'),\n"
        "  (node.text or ''):gsub('\"','\\\\\"'),\n"
        "  c_str)\n",
        id_json->valuestring);

    const char* result_str = s_lua_eval(lua_buf);
    if (result_str) {
        cJSON* result = cJSON_Parse(result_str);
        if (result) return result;
    }

    cJSON* fallback = cJSON_CreateObject();
    cJSON_AddBoolToObject(fallback, "ok", 1);
    return fallback;
}

/* ============================================================================
 * dialogue.advance - Advance or choose in the active dialogue
 *
 * Params:
 *   choice: number (optional) - 1-based choice index. If omitted, advance linearly.
 *
 * Returns: { ok, speaker, text, choices, ended }
 * ============================================================================ */

static cJSON* handle_dialogue_advance(cd_kernel_t* kernel, const cJSON* params,
                                       int* error_code, const char** error_msg) {
    (void)kernel;

    if (!s_lua_eval) {
        *error_code = -32603;
        *error_msg = "Lua eval not initialized";
        return NULL;
    }

    const cJSON* choice_json = cJSON_GetObjectItemCaseSensitive(params, "choice");
    int has_choice = (choice_json && cJSON_IsNumber(choice_json));
    int choice_idx = has_choice ? (int)choice_json->valuedouble : 0;

    char lua_buf[2048];
    snprintf(lua_buf, sizeof(lua_buf),
        "local mgr = _G._dialogue_manager\n"
        "if not mgr then return '{\"error\":\"no dialogue manager\"}' end\n"
        "local dlg = mgr:active()\n"
        "if not dlg then return '{\"error\":\"no active dialogue\"}' end\n"
        "local state = _G._game_state or {}\n"
        "local node\n"
        "if %d > 0 then\n"
        "  node = dlg:choose(%d, state)\n"
        "else\n"
        "  node = dlg:advance(state)\n"
        "end\n"
        "if not node or not dlg:is_active() then\n"
        "  mgr:end_active()\n"
        "  return '{\"ok\":true,\"ended\":true}'\n"
        "end\n"
        "local choices = dlg:get_choices(state)\n"
        "local c_str = ''\n"
        "for i, c in ipairs(choices) do\n"
        "  if i > 1 then c_str = c_str .. ',' end\n"
        "  c_str = c_str .. string.format('{\"index\":%%d,\"text\":\"%%s\"}', c.index, c.text:gsub('\"','\\\\\"'))\n"
        "end\n"
        "return string.format('{\"ok\":true,\"speaker\":\"%%s\",\"text\":\"%%s\",\"choices\":[%%s]}',\n"
        "  (node.speaker or ''):gsub('\"','\\\\\"'),\n"
        "  (node.text or ''):gsub('\"','\\\\\"'),\n"
        "  c_str)\n",
        choice_idx, choice_idx);

    const char* result_str = s_lua_eval(lua_buf);
    if (result_str) {
        cJSON* result = cJSON_Parse(result_str);
        if (result) return result;
    }

    cJSON* fallback = cJSON_CreateObject();
    cJSON_AddBoolToObject(fallback, "ok", 1);
    cJSON_AddBoolToObject(fallback, "ended", 1);
    return fallback;
}

/* ============================================================================
 * dialogue.status - Get current dialogue state
 *
 * Returns: { active, id, speaker, text, choices } or { active: false }
 * ============================================================================ */

static cJSON* handle_dialogue_status(cd_kernel_t* kernel, const cJSON* params,
                                      int* error_code, const char** error_msg) {
    (void)kernel; (void)params;

    if (!s_lua_eval) {
        *error_code = -32603;
        *error_msg = "Lua eval not initialized";
        return NULL;
    }

    const char* lua_code =
        "local mgr = _G._dialogue_manager\n"
        "if not mgr then return '{\"active\":false,\"error\":\"no dialogue manager\"}' end\n"
        "local dlg = mgr:active()\n"
        "if not dlg or not dlg:is_active() then return '{\"active\":false}' end\n"
        "local id = mgr:active_id() or ''\n"
        "local node = dlg:current()\n"
        "if not node then return '{\"active\":false}' end\n"
        "local state = _G._game_state or {}\n"
        "local choices = dlg:get_choices(state)\n"
        "local c_str = ''\n"
        "for i, c in ipairs(choices) do\n"
        "  if i > 1 then c_str = c_str .. ',' end\n"
        "  c_str = c_str .. string.format('{\"index\":%d,\"text\":\"%s\"}', c.index, c.text:gsub('\"','\\\\\"'))\n"
        "end\n"
        "return string.format('{\"active\":true,\"id\":\"%s\",\"speaker\":\"%s\",\"text\":\"%s\",\"choices\":[%s]}',\n"
        "  id, (node.speaker or ''):gsub('\"','\\\\\"'),\n"
        "  (node.text or ''):gsub('\"','\\\\\"'), c_str)\n";

    const char* result_str = s_lua_eval(lua_code);
    if (result_str) {
        cJSON* result = cJSON_Parse(result_str);
        if (result) return result;
    }

    cJSON* fallback = cJSON_CreateObject();
    cJSON_AddBoolToObject(fallback, "active", 0);
    return fallback;
}

/* ============================================================================
 * quest.status - Get quest tracker state
 *
 * Params (optional):
 *   quest_id: string - specific quest, or omit for all
 *
 * Returns: { quests: [...] } with status and objective progress
 * ============================================================================ */

static cJSON* handle_quest_status(cd_kernel_t* kernel, const cJSON* params,
                                   int* error_code, const char** error_msg) {
    (void)kernel;

    if (!s_lua_eval) {
        *error_code = -32603;
        *error_msg = "Lua eval not initialized";
        return NULL;
    }

    const cJSON* qid_json = cJSON_GetObjectItemCaseSensitive(params, "quest_id");
    const char* quest_id = (qid_json && cJSON_IsString(qid_json)) ? qid_json->valuestring : NULL;

    char lua_buf[2048];
    if (quest_id) {
        snprintf(lua_buf, sizeof(lua_buf),
            "local t = _G._quest_tracker\n"
            "if not t then return '{\"error\":\"no quest tracker\"}' end\n"
            "local q = t:get_quest('%s')\n"
            "if not q then return '{\"error\":\"quest not found\"}' end\n"
            "local objs = ''\n"
            "for i, o in ipairs(q.objectives) do\n"
            "  if i > 1 then objs = objs .. ',' end\n"
            "  objs = objs .. string.format(\n"
            "    '{\"id\":\"%%s\",\"description\":\"%%s\",\"current\":%%d,\"required\":%%d,\"completed\":%%s}',\n"
            "    o.id, o.description:gsub('\"','\\\\\"'), o.current, o.required, tostring(o.completed))\n"
            "end\n"
            "return string.format(\n"
            "  '{\"id\":\"%%s\",\"name\":\"%%s\",\"status\":\"%%s\",\"objectives\":[%%s]}',\n"
            "  q.id, q.name:gsub('\"','\\\\\"'), q.status, objs)\n",
            quest_id);
    } else {
        snprintf(lua_buf, sizeof(lua_buf),
            "local t = _G._quest_tracker\n"
            "if not t then return '{\"quests\":[]}' end\n"
            "local all = t:get_quests()\n"
            "local parts = {}\n"
            "for _, q in ipairs(all) do\n"
            "  local objs = ''\n"
            "  for i, o in ipairs(q.objectives) do\n"
            "    if i > 1 then objs = objs .. ',' end\n"
            "    objs = objs .. string.format(\n"
            "      '{\"id\":\"%%s\",\"current\":%%d,\"required\":%%d,\"completed\":%%s}',\n"
            "      o.id, o.current, o.required, tostring(o.completed))\n"
            "  end\n"
            "  table.insert(parts, string.format(\n"
            "    '{\"id\":\"%%s\",\"name\":\"%%s\",\"status\":\"%%s\",\"objectives\":[%%s]}',\n"
            "    q.id, q.name:gsub('\"','\\\\\"'), q.status, objs))\n"
            "end\n"
            "return '{\"quests\":[' .. table.concat(parts, ',') .. ']}'\n");
    }

    const char* result_str = s_lua_eval(lua_buf);
    if (result_str) {
        cJSON* result = cJSON_Parse(result_str);
        if (result) return result;
    }

    cJSON* fallback = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(fallback, "quests");
    (void)arr;
    return fallback;
}

/* ============================================================================
 * quest.update - Update a quest objective
 *
 * Params:
 *   quest_id: string
 *   objective_id: string
 *   amount: number (optional, default 1)
 *
 * Returns: { ok, completed }
 * ============================================================================ */

static cJSON* handle_quest_update(cd_kernel_t* kernel, const cJSON* params,
                                   int* error_code, const char** error_msg) {
    (void)kernel;

    if (!s_lua_eval) {
        *error_code = -32603;
        *error_msg = "Lua eval not initialized";
        return NULL;
    }

    const cJSON* qid = cJSON_GetObjectItemCaseSensitive(params, "quest_id");
    const cJSON* oid = cJSON_GetObjectItemCaseSensitive(params, "objective_id");
    if (!qid || !cJSON_IsString(qid) || !oid || !cJSON_IsString(oid)) {
        *error_code = -32602;
        *error_msg = "Missing required parameters: quest_id, objective_id";
        return NULL;
    }

    const cJSON* amt_json = cJSON_GetObjectItemCaseSensitive(params, "amount");
    int amount = (amt_json && cJSON_IsNumber(amt_json)) ? (int)amt_json->valuedouble : 1;

    char lua_buf[1024];
    snprintf(lua_buf, sizeof(lua_buf),
        "local t = _G._quest_tracker\n"
        "if not t then return '{\"error\":\"no quest tracker\"}' end\n"
        "local ok = t:update_objective('%s', '%s', %d)\n"
        "if not ok then return '{\"ok\":false,\"error\":\"update failed\"}' end\n"
        "local completed = t:is_objective_complete('%s', '%s')\n"
        "local quest_done = t:is_quest_complete('%s')\n"
        "return string.format('{\"ok\":true,\"objective_completed\":%%s,\"quest_complete\":%%s}',\n"
        "  tostring(completed), tostring(quest_done))\n",
        qid->valuestring, oid->valuestring, amount,
        qid->valuestring, oid->valuestring,
        qid->valuestring);

    const char* result_str = s_lua_eval(lua_buf);
    if (result_str) {
        cJSON* result = cJSON_Parse(result_str);
        if (result) return result;
    }

    cJSON* fallback = cJSON_CreateObject();
    cJSON_AddBoolToObject(fallback, "ok", 0);
    return fallback;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_dialogue_tools(cd_mcp_server_t* server) {
    if (!server) return CD_ERR_NULL;

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "dialogue.start", handle_dialogue_start,
        "Start a dialogue by its registered ID.",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\",\"description\":\"Dialogue identifier\"}"
        "},\"required\":[\"id\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "dialogue.advance", handle_dialogue_advance,
        "Advance the active dialogue or select a choice by index.",
        "{\"type\":\"object\",\"properties\":{"
        "\"choice\":{\"type\":\"number\",\"description\":\"1-based choice index (omit to advance linearly)\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "dialogue.status", handle_dialogue_status,
        "Get the current dialogue state including speaker, text, and choices.",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "quest.status", handle_quest_status,
        "Get quest tracker state for all quests or a specific quest.",
        "{\"type\":\"object\",\"properties\":{"
        "\"quest_id\":{\"type\":\"string\",\"description\":\"Specific quest ID (omit for all)\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "quest.update", handle_quest_update,
        "Update a quest objective's progress for testing.",
        "{\"type\":\"object\",\"properties\":{"
        "\"quest_id\":{\"type\":\"string\",\"description\":\"Quest identifier\"},"
        "\"objective_id\":{\"type\":\"string\",\"description\":\"Objective identifier\"},"
        "\"amount\":{\"type\":\"number\",\"description\":\"Progress increment (default 1)\"}"
        "},\"required\":[\"quest_id\",\"objective_id\"]}");
    if (res != CD_OK) return res;

    return CD_OK;
}
