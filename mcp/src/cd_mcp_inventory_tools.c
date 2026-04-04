/* cd_mcp_inventory_tools.c - MCP tool handlers for inventory system
 *
 * Tools:
 *   inventory.list    - List all items in a named inventory
 *   inventory.add     - Add item(s) to an inventory (for testing)
 *   inventory.remove  - Remove item(s) from an inventory
 *   inventory.inspect - Get details about an item type from the database
 *
 * These tools invoke Lua functions on global _inventory_db and
 * _inventories table (created by user scripts).
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

void cd_mcp_inventory_tools_set_lua_eval(void* fn) {
    s_lua_eval = (cd_lua_eval_fn)fn;
}

/* ============================================================================
 * inventory.list - List all items in a named inventory
 *
 * Params:
 *   name: string - inventory name (key in _G._inventories table)
 *
 * Returns: { items: [{id, count, slot}], free_slots, weight, capacity }
 * ============================================================================ */

static cJSON* handle_inventory_list(cd_kernel_t* kernel, const cJSON* params,
                                     int* error_code, const char** error_msg) {
    (void)kernel;

    if (!s_lua_eval) {
        *error_code = -32603;
        *error_msg = "Lua eval not initialized";
        return NULL;
    }

    const cJSON* name_json = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (!name_json || !cJSON_IsString(name_json)) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: name (string)";
        return NULL;
    }

    char lua_buf[2048];
    snprintf(lua_buf, sizeof(lua_buf),
        "local invs = _G._inventories\n"
        "if not invs then return '{\"error\":\"no inventories table\"}' end\n"
        "local inv = invs['%s']\n"
        "if not inv then return '{\"error\":\"inventory not found\"}' end\n"
        "local items = inv:get_items()\n"
        "local parts = {}\n"
        "for _, it in ipairs(items) do\n"
        "  table.insert(parts, string.format(\n"
        "    '{\"id\":\"%%s\",\"count\":%%d,\"slot\":%%d}',\n"
        "    it.id, it.count, it.slot))\n"
        "end\n"
        "return string.format(\n"
        "  '{\"items\":[%%s],\"free_slots\":%%d,\"weight\":%%s,\"capacity\":%%d}',\n"
        "  table.concat(parts, ','), inv:get_free_slots(),\n"
        "  string.format('%%.2f', inv:get_weight()), inv:get_capacity())\n",
        name_json->valuestring);

    const char* result_str = s_lua_eval(lua_buf);
    if (result_str) {
        cJSON* result = cJSON_Parse(result_str);
        if (result) return result;
    }

    cJSON* fallback = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(fallback, "items");
    (void)arr;
    return fallback;
}

/* ============================================================================
 * inventory.add - Add item(s) to an inventory
 *
 * Params:
 *   name: string - inventory name
 *   item_id: string - item type id
 *   count: number (optional, default 1)
 *
 * Returns: { ok, added, total }
 * ============================================================================ */

static cJSON* handle_inventory_add(cd_kernel_t* kernel, const cJSON* params,
                                    int* error_code, const char** error_msg) {
    (void)kernel;

    if (!s_lua_eval) {
        *error_code = -32603;
        *error_msg = "Lua eval not initialized";
        return NULL;
    }

    const cJSON* name_json = cJSON_GetObjectItemCaseSensitive(params, "name");
    const cJSON* item_json = cJSON_GetObjectItemCaseSensitive(params, "item_id");
    if (!name_json || !cJSON_IsString(name_json) ||
        !item_json || !cJSON_IsString(item_json)) {
        *error_code = -32602;
        *error_msg = "Missing required parameters: name, item_id";
        return NULL;
    }

    const cJSON* count_json = cJSON_GetObjectItemCaseSensitive(params, "count");
    int count = (count_json && cJSON_IsNumber(count_json)) ? (int)count_json->valuedouble : 1;

    char lua_buf[1024];
    snprintf(lua_buf, sizeof(lua_buf),
        "local invs = _G._inventories\n"
        "if not invs then return '{\"error\":\"no inventories table\"}' end\n"
        "local inv = invs['%s']\n"
        "if not inv then return '{\"error\":\"inventory not found\"}' end\n"
        "local added = inv:add('%s', %d)\n"
        "local total = inv:count_item('%s')\n"
        "return string.format('{\"ok\":true,\"added\":%%d,\"total\":%%d}', added, total)\n",
        name_json->valuestring, item_json->valuestring, count,
        item_json->valuestring);

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
 * inventory.remove - Remove item(s) from an inventory
 *
 * Params:
 *   name: string - inventory name
 *   item_id: string - item type id
 *   count: number (optional, default 1)
 *
 * Returns: { ok, removed, remaining }
 * ============================================================================ */

static cJSON* handle_inventory_remove(cd_kernel_t* kernel, const cJSON* params,
                                       int* error_code, const char** error_msg) {
    (void)kernel;

    if (!s_lua_eval) {
        *error_code = -32603;
        *error_msg = "Lua eval not initialized";
        return NULL;
    }

    const cJSON* name_json = cJSON_GetObjectItemCaseSensitive(params, "name");
    const cJSON* item_json = cJSON_GetObjectItemCaseSensitive(params, "item_id");
    if (!name_json || !cJSON_IsString(name_json) ||
        !item_json || !cJSON_IsString(item_json)) {
        *error_code = -32602;
        *error_msg = "Missing required parameters: name, item_id";
        return NULL;
    }

    const cJSON* count_json = cJSON_GetObjectItemCaseSensitive(params, "count");
    int count = (count_json && cJSON_IsNumber(count_json)) ? (int)count_json->valuedouble : 1;

    char lua_buf[1024];
    snprintf(lua_buf, sizeof(lua_buf),
        "local invs = _G._inventories\n"
        "if not invs then return '{\"error\":\"no inventories table\"}' end\n"
        "local inv = invs['%s']\n"
        "if not inv then return '{\"error\":\"inventory not found\"}' end\n"
        "local removed = inv:remove('%s', %d)\n"
        "local remaining = inv:count_item('%s')\n"
        "return string.format('{\"ok\":true,\"removed\":%%d,\"remaining\":%%d}', removed, remaining)\n",
        name_json->valuestring, item_json->valuestring, count,
        item_json->valuestring);

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
 * inventory.inspect - Get details about an item type from the database
 *
 * Params:
 *   item_id: string - item type id
 *
 * Returns: { id, name, description, category, max_stack, weight, value }
 * ============================================================================ */

static cJSON* handle_inventory_inspect(cd_kernel_t* kernel, const cJSON* params,
                                        int* error_code, const char** error_msg) {
    (void)kernel;

    if (!s_lua_eval) {
        *error_code = -32603;
        *error_msg = "Lua eval not initialized";
        return NULL;
    }

    const cJSON* item_json = cJSON_GetObjectItemCaseSensitive(params, "item_id");
    if (!item_json || !cJSON_IsString(item_json)) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: item_id (string)";
        return NULL;
    }

    char lua_buf[1024];
    snprintf(lua_buf, sizeof(lua_buf),
        "local db = _G._inventory_db\n"
        "if not db then return '{\"error\":\"no item database\"}' end\n"
        "local item = db:get('%s')\n"
        "if not item then return '{\"error\":\"item not found\"}' end\n"
        "return string.format(\n"
        "  '{\"id\":\"%%s\",\"name\":\"%%s\",\"description\":\"%%s\",\"category\":\"%%s\","
        "\"max_stack\":%%d,\"weight\":%%s,\"value\":%%d}',\n"
        "  item.id, item.name:gsub('\"','\\\\\"'),\n"
        "  item.description:gsub('\"','\\\\\"'),\n"
        "  item.category, item.max_stack,\n"
        "  string.format('%%.2f', item.weight), item.value)\n",
        item_json->valuestring);

    const char* result_str = s_lua_eval(lua_buf);
    if (result_str) {
        cJSON* result = cJSON_Parse(result_str);
        if (result) return result;
    }

    cJSON* fallback = cJSON_CreateObject();
    cJSON_AddStringToObject(fallback, "error", "item not found");
    return fallback;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_inventory_tools(cd_mcp_server_t* server) {
    if (!server) return CD_ERR_NULL;

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "inventory.list",
        handle_inventory_list,
        "List all items in a named inventory",
        "{\"type\":\"object\",\"properties\":{"
        "\"name\":{\"type\":\"string\",\"description\":\"Inventory name\"}"
        "},\"required\":[\"name\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "inventory.add",
        handle_inventory_add,
        "Add items to an inventory",
        "{\"type\":\"object\",\"properties\":{"
        "\"name\":{\"type\":\"string\",\"description\":\"Inventory name\"},"
        "\"item_id\":{\"type\":\"string\",\"description\":\"Item type ID to add\"},"
        "\"count\":{\"type\":\"number\",\"description\":\"Number of items to add (default: 1)\"}"
        "},\"required\":[\"name\",\"item_id\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "inventory.remove",
        handle_inventory_remove,
        "Remove items from an inventory",
        "{\"type\":\"object\",\"properties\":{"
        "\"name\":{\"type\":\"string\",\"description\":\"Inventory name\"},"
        "\"item_id\":{\"type\":\"string\",\"description\":\"Item type ID to remove\"},"
        "\"count\":{\"type\":\"number\",\"description\":\"Number of items to remove (default: 1)\"}"
        "},\"required\":[\"name\",\"item_id\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "inventory.inspect",
        handle_inventory_inspect,
        "Get details about an item type from the item database",
        "{\"type\":\"object\",\"properties\":{"
        "\"item_id\":{\"type\":\"string\",\"description\":\"Item type ID to inspect\"}"
        "},\"required\":[\"item_id\"]}");
    if (res != CD_OK) return res;

    return CD_OK;
}
