/* cd_mcp_ai_tools.c - MCP tool handlers for AI agent inspection/manipulation
 *
 * Tools:
 *   ai.listAgents          - List all AI agents with summary
 *   ai.getAgentState       - Get detailed agent state (goals, BB, memory)
 *   ai.setAgentState       - Force agent state for testing
 *   ai.getBlackboard       - Get all BB key-value pairs
 *   ai.setBlackboard       - Set a BB entry
 *   ai.getSensoryMemory    - Get agent's sensory memory records
 *   ai.emitSound           - Inject a sound event
 */
#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_error.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

/* Forward declarations for types we access via void* to avoid DLL dependency */
/* These match the structs in cd_lua_ai_agent.h and cd_lua_blackboard.h */

#define CD_AI_MAX_AGENTS         128
#define CD_AI_MAX_MEMORY_RECORDS  16
#define CD_BB_MAX_BOARDS   128
#define CD_BB_MAX_ENTRIES   32
#define CD_BB_MAX_KEY_LEN   32
#define CD_BB_MAX_STR_LEN   64

/* Minimal type replicas for reading agent data without linking scripting_lua */
typedef enum { CD_AI_LOD_FULL=0, CD_AI_LOD_REDUCED=1, CD_AI_LOD_POLLING=2, CD_AI_LOD_DORMANT=3 } cd_ai_lod_t;

typedef struct {
    uint64_t  entity_id;
    float     last_sensed_pos[3];
    double    time_became_visible;
    double    time_last_visible;
    double    time_last_sensed;
    bool      within_fov;
    bool      shootable;
} cd_ai_memory_record_mcp_t;

typedef struct {
    bool          active;
    uint64_t      node_id;
    uint32_t      bb_handle;
    uint32_t      shared_bb;
    float         sight_range;
    float         sight_fov;
    float         hear_range;
    float         memory_span;
    cd_ai_memory_record_mcp_t memory[CD_AI_MAX_MEMORY_RECORDS];
    uint32_t      memory_count;
    uint32_t      nav_handle;
    char          group[32];
    uint32_t      brain_handle;
    uint32_t      bt_handle;
    cd_ai_lod_t   lod;
    uint32_t      perception_frame;
    float         vision_interval;
    float         targeting_interval;
    float         arbitration_interval;
    float         vision_timer;
    float         targeting_timer;
    float         arbitration_timer;
    uint64_t      current_target;
    uint32_t      origin_id;
} cd_ai_agent_mcp_t;

typedef enum {
    CD_BB_TYPE_NONE=0, CD_BB_TYPE_FLOAT=1, CD_BB_TYPE_INT=2,
    CD_BB_TYPE_BOOL=3, CD_BB_TYPE_ID=4, CD_BB_TYPE_STRING=5,
} cd_bb_value_type_mcp_t;

typedef struct {
    char key[CD_BB_MAX_KEY_LEN];
    cd_bb_value_type_mcp_t type;
    union {
        float f;
        int32_t i;
        bool b;
        uint64_t id;
        char s[CD_BB_MAX_STR_LEN];
    } value;
} cd_bb_entry_mcp_t;

typedef struct {
    bool active;
    cd_bb_entry_mcp_t entries[CD_BB_MAX_ENTRIES];
    uint32_t entry_count;
} cd_bb_mcp_t;

/* Function pointers set by scripting_lua plugin at init */
typedef uint32_t (*cd_ai_get_count_fn)(void);
typedef void* (*cd_ai_get_agent_fn)(uint32_t handle);
typedef void* (*cd_ai_get_all_fn)(void);

static cd_ai_get_count_fn s_get_count = NULL;
static cd_ai_get_agent_fn s_get_agent = NULL;
static cd_ai_get_all_fn   s_get_all   = NULL;

/* Blackboard access function pointers */
typedef int32_t  (*cd_bb_create_fn)(void);
typedef void     (*cd_bb_destroy_fn)(int32_t);
typedef int32_t  (*cd_bb_set_float_fn)(int32_t, const char*, float);
typedef int32_t  (*cd_bb_set_int_fn)(int32_t, const char*, int32_t);
typedef int32_t  (*cd_bb_set_bool_fn)(int32_t, const char*, bool);
typedef int32_t  (*cd_bb_set_string_fn)(int32_t, const char*, const char*);
typedef bool     (*cd_bb_get_float_fn)(int32_t, const char*, float*);
typedef void     (*cd_bb_clear_fn)(int32_t);

/* For simplicity, use a direct pointer to the blackboard pool array */
static void* s_bb_pool = NULL;   /* points to array of cd_bb_mcp_t[CD_BB_MAX_BOARDS] */

/* Sound emitter fn pointer */
typedef void (*cd_ai_emit_sound_fn)(float, float, float, float, const char*);
static cd_ai_emit_sound_fn s_emit_sound = NULL;

/* ============================================================================
 * Setter functions (called by scripting_lua during init)
 * ============================================================================ */

void cd_mcp_ai_tools_set_agent_fns(void* get_count, void* get_agent, void* get_all) {
    s_get_count = (cd_ai_get_count_fn)get_count;
    s_get_agent = (cd_ai_get_agent_fn)get_agent;
    s_get_all   = (cd_ai_get_all_fn)get_all;
}

void cd_mcp_ai_tools_set_bb_pool(void* pool) {
    s_bb_pool = pool;
}

void cd_mcp_ai_tools_set_sound_fn(void* fn) {
    s_emit_sound = (cd_ai_emit_sound_fn)fn;
}

/* ============================================================================
 * Helper: LOD to string
 * ============================================================================ */

static const char* lod_to_string(cd_ai_lod_t lod) {
    switch (lod) {
        case CD_AI_LOD_FULL:    return "full";
        case CD_AI_LOD_REDUCED: return "reduced";
        case CD_AI_LOD_POLLING: return "polling";
        case CD_AI_LOD_DORMANT: return "dormant";
        default: return "unknown";
    }
}

/* ============================================================================
 * ai.listAgents
 * ============================================================================ */

static cJSON* handle_ai_list_agents(cd_kernel_t* kernel, const cJSON* params,
                                      int* error_code, const char** error_msg) {
    (void)kernel; (void)params;

    if (!s_get_all) {
        *error_code = -32603;
        *error_msg = cd_mcp_error_fmt("AI agent system not initialized",
            "The scripting_lua plugin must be loaded with AI agents enabled.",
            "Ensure your scene has scripts that use the AI agent API.");
        return NULL;
    }

    cd_ai_agent_mcp_t* agents = (cd_ai_agent_mcp_t*)s_get_all();
    if (!agents) {
        *error_code = -32603;
        *error_msg = cd_mcp_error_fmt("AI agent pool unavailable",
            "The agent pool pointer is NULL.",
            "Ensure AI scripts are loaded that create agents.");
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(result, "agents");

    for (int i = 0; i < CD_AI_MAX_AGENTS; i++) {
        if (!agents[i].active) continue;
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "handle", i + 1);
        cJSON_AddNumberToObject(obj, "node_id", (double)agents[i].node_id);
        cJSON_AddStringToObject(obj, "group", agents[i].group);
        cJSON_AddStringToObject(obj, "lod", lod_to_string(agents[i].lod));
        cJSON_AddNumberToObject(obj, "brain", agents[i].brain_handle);
        cJSON_AddNumberToObject(obj, "bt", agents[i].bt_handle);
        cJSON_AddNumberToObject(obj, "bb", agents[i].bb_handle);
        cJSON_AddNumberToObject(obj, "memory_count", agents[i].memory_count);
        if (agents[i].current_target != UINT64_MAX) {
            cJSON_AddNumberToObject(obj, "target", (double)agents[i].current_target);
        }
        cJSON_AddItemToArray(arr, obj);
    }

    return result;
}

/* ============================================================================
 * ai.getAgentState
 * ============================================================================ */

static cJSON* handle_ai_get_agent_state(cd_kernel_t* kernel, const cJSON* params,
                                          int* error_code, const char** error_msg) {
    (void)kernel;

    if (!s_get_agent) {
        *error_code = -32603;
        *error_msg = cd_mcp_error_fmt("AI agent system not initialized",
            "The scripting_lua plugin must be loaded with AI agents enabled.",
            "Ensure your scene has scripts that use the AI agent API.");
        return NULL;
    }

    const cJSON* id_json = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (!id_json || !cJSON_IsNumber(id_json)) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: id";
        return NULL;
    }

    uint32_t handle = (uint32_t)id_json->valuedouble;
    cd_ai_agent_mcp_t* agent = (cd_ai_agent_mcp_t*)s_get_agent(handle);
    if (!agent || !agent->active) {
        *error_code = -32602;
        *error_msg = cd_mcp_error_fmt("Agent not found",
            "No AI agent exists with the given id.",
            "Use ai.listAgents to see available agent IDs.");
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "handle", handle);
    cJSON_AddNumberToObject(result, "node_id", (double)agent->node_id);
    cJSON_AddStringToObject(result, "group", agent->group);
    cJSON_AddStringToObject(result, "lod", lod_to_string(agent->lod));

    /* Perception config */
    cJSON* perception = cJSON_AddObjectToObject(result, "perception");
    cJSON_AddNumberToObject(perception, "sight_range", agent->sight_range);
    cJSON_AddNumberToObject(perception, "sight_fov", agent->sight_fov);
    cJSON_AddNumberToObject(perception, "hear_range", agent->hear_range);
    cJSON_AddNumberToObject(perception, "memory_span", agent->memory_span);

    /* Regulators */
    cJSON* regulators = cJSON_AddObjectToObject(result, "regulators");
    cJSON_AddNumberToObject(regulators, "vision_interval", agent->vision_interval);
    cJSON_AddNumberToObject(regulators, "targeting_interval", agent->targeting_interval);
    cJSON_AddNumberToObject(regulators, "arbitration_interval", agent->arbitration_interval);

    /* Decision system */
    cJSON_AddNumberToObject(result, "brain", agent->brain_handle);
    cJSON_AddNumberToObject(result, "bt", agent->bt_handle);
    cJSON_AddNumberToObject(result, "bb", agent->bb_handle);
    cJSON_AddNumberToObject(result, "shared_bb", agent->shared_bb);

    /* Current target */
    if (agent->current_target != UINT64_MAX) {
        cJSON_AddNumberToObject(result, "target_id", (double)agent->current_target);
    } else {
        cJSON_AddNullToObject(result, "target_id");
    }

    /* Sensory memory */
    cJSON* mem_arr = cJSON_AddArrayToObject(result, "sensory_memory");
    for (uint32_t i = 0; i < agent->memory_count && i < CD_AI_MAX_MEMORY_RECORDS; i++) {
        cd_ai_memory_record_mcp_t* rec = &agent->memory[i];
        if (rec->entity_id == UINT64_MAX) continue;
        cJSON* mem = cJSON_CreateObject();
        cJSON_AddNumberToObject(mem, "entity_id", (double)rec->entity_id);
        cJSON* pos = cJSON_AddArrayToObject(mem, "last_pos");
        cJSON_AddItemToArray(pos, cJSON_CreateNumber(rec->last_sensed_pos[0]));
        cJSON_AddItemToArray(pos, cJSON_CreateNumber(rec->last_sensed_pos[1]));
        cJSON_AddItemToArray(pos, cJSON_CreateNumber(rec->last_sensed_pos[2]));
        cJSON_AddNumberToObject(mem, "time_became_visible", rec->time_became_visible);
        cJSON_AddNumberToObject(mem, "time_last_visible", rec->time_last_visible);
        cJSON_AddNumberToObject(mem, "time_last_sensed", rec->time_last_sensed);
        cJSON_AddBoolToObject(mem, "within_fov", rec->within_fov);
        cJSON_AddBoolToObject(mem, "shootable", rec->shootable);
        cJSON_AddItemToArray(mem_arr, mem);
    }

    return result;
}

/* ============================================================================
 * ai.setAgentState -- force LOD or target for testing
 * ============================================================================ */

static cJSON* handle_ai_set_agent_state(cd_kernel_t* kernel, const cJSON* params,
                                          int* error_code, const char** error_msg) {
    (void)kernel;

    if (!s_get_agent) {
        *error_code = -32603;
        *error_msg = cd_mcp_error_fmt("AI agent system not initialized",
            "The scripting_lua plugin must be loaded with AI agents enabled.",
            "Ensure your scene has scripts that use the AI agent API.");
        return NULL;
    }

    const cJSON* id_json = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (!id_json || !cJSON_IsNumber(id_json)) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: id";
        return NULL;
    }

    uint32_t handle = (uint32_t)id_json->valuedouble;
    cd_ai_agent_mcp_t* agent = (cd_ai_agent_mcp_t*)s_get_agent(handle);
    if (!agent || !agent->active) {
        *error_code = -32602;
        *error_msg = cd_mcp_error_fmt("Agent not found",
            "No AI agent exists with the given id.",
            "Use ai.listAgents to see available agent IDs.");
        return NULL;
    }

    /* Optional: set LOD */
    const cJSON* lod_json = cJSON_GetObjectItemCaseSensitive(params, "lod");
    if (lod_json && cJSON_IsNumber(lod_json)) {
        int lod_val = (int)lod_json->valuedouble;
        if (lod_val >= 0 && lod_val <= 3) {
            agent->lod = (cd_ai_lod_t)lod_val;
        }
    }

    /* Optional: set group */
    const cJSON* group_json = cJSON_GetObjectItemCaseSensitive(params, "group");
    if (group_json && cJSON_IsString(group_json)) {
        snprintf(agent->group, 32, "%s", group_json->valuestring);
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", 1);
    return result;
}

/* ============================================================================
 * ai.getBlackboard
 * ============================================================================ */

static cJSON* handle_ai_get_blackboard(cd_kernel_t* kernel, const cJSON* params,
                                         int* error_code, const char** error_msg) {
    (void)kernel;

    if (!s_bb_pool) {
        *error_code = -32603;
        *error_msg = cd_mcp_error_fmt("Blackboard system not initialized",
            "The blackboard pool is not available.",
            "Ensure AI scripts are loaded that create blackboards.");
        return NULL;
    }

    const cJSON* handle_json = cJSON_GetObjectItemCaseSensitive(params, "handle");
    if (!handle_json || !cJSON_IsNumber(handle_json)) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: handle";
        return NULL;
    }

    int handle = (int)handle_json->valuedouble;
    int idx = handle - 1;  /* 1-based to 0-based */
    if (idx < 0 || idx >= CD_BB_MAX_BOARDS) {
        *error_code = -32602;
        *error_msg = cd_mcp_error_fmt("Invalid blackboard handle",
            "Handle must be between 0 and 127.",
            "Use ai.getAgentState to find the agent's blackboard handle.");
        return NULL;
    }

    cd_bb_mcp_t* boards = (cd_bb_mcp_t*)s_bb_pool;
    cd_bb_mcp_t* bb = &boards[idx];
    if (!bb->active) {
        *error_code = -32602;
        *error_msg = cd_mcp_error_fmt("Blackboard not active",
            "The blackboard at this handle has been freed or was never allocated.",
            "Use ai.getAgentState to check the agent's current blackboard.");
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "handle", handle);
    cJSON* entries = cJSON_AddObjectToObject(result, "entries");

    for (uint32_t i = 0; i < bb->entry_count; i++) {
        cd_bb_entry_mcp_t* e = &bb->entries[i];
        if (e->type == CD_BB_TYPE_NONE) continue;
        switch (e->type) {
            case CD_BB_TYPE_FLOAT:
                cJSON_AddNumberToObject(entries, e->key, e->value.f);
                break;
            case CD_BB_TYPE_INT:
                cJSON_AddNumberToObject(entries, e->key, e->value.i);
                break;
            case CD_BB_TYPE_BOOL:
                cJSON_AddBoolToObject(entries, e->key, e->value.b);
                break;
            case CD_BB_TYPE_ID:
                cJSON_AddNumberToObject(entries, e->key, (double)e->value.id);
                break;
            case CD_BB_TYPE_STRING:
                cJSON_AddStringToObject(entries, e->key, e->value.s);
                break;
            default:
                break;
        }
    }

    return result;
}

/* ============================================================================
 * ai.setBlackboard
 * ============================================================================ */

static cJSON* handle_ai_set_blackboard(cd_kernel_t* kernel, const cJSON* params,
                                         int* error_code, const char** error_msg) {
    (void)kernel;

    if (!s_bb_pool) {
        *error_code = -32603;
        *error_msg = cd_mcp_error_fmt("Blackboard system not initialized",
            "The blackboard pool is not available.",
            "Ensure AI scripts are loaded that create blackboards.");
        return NULL;
    }

    const cJSON* handle_json = cJSON_GetObjectItemCaseSensitive(params, "handle");
    const cJSON* key_json = cJSON_GetObjectItemCaseSensitive(params, "key");
    const cJSON* value_json = cJSON_GetObjectItemCaseSensitive(params, "value");

    if (!handle_json || !key_json || !value_json) {
        *error_code = -32602;
        *error_msg = "Missing required parameters: handle, key, value";
        return NULL;
    }

    int handle = (int)handle_json->valuedouble;
    int idx = handle - 1;
    if (idx < 0 || idx >= CD_BB_MAX_BOARDS) {
        *error_code = -32602;
        *error_msg = cd_mcp_error_fmt("Invalid blackboard handle",
            "Handle must be between 0 and 127.",
            "Use ai.getAgentState to find the agent's blackboard handle.");
        return NULL;
    }

    cd_bb_mcp_t* boards = (cd_bb_mcp_t*)s_bb_pool;
    cd_bb_mcp_t* bb = &boards[idx];
    if (!bb->active) {
        *error_code = -32602;
        *error_msg = cd_mcp_error_fmt("Blackboard not active",
            "The blackboard at this handle has been freed or was never allocated.",
            "Use ai.getAgentState to check the agent's current blackboard.");
        return NULL;
    }

    const char* key = key_json->valuestring;
    if (!key || strlen(key) >= CD_BB_MAX_KEY_LEN) {
        *error_code = -32602;
        *error_msg = cd_mcp_error_fmt("Invalid blackboard key",
            "Key must be a non-empty string up to 31 characters.",
            "Example: {\"handle\": 0, \"key\": \"health\", \"value\": 100}");
        return NULL;
    }

    /* Find or create entry */
    cd_bb_entry_mcp_t* target_entry = NULL;
    cd_bb_entry_mcp_t* empty_entry = NULL;
    for (uint32_t i = 0; i < bb->entry_count; i++) {
        if (bb->entries[i].type != CD_BB_TYPE_NONE && strcmp(bb->entries[i].key, key) == 0) {
            target_entry = &bb->entries[i];
            break;
        }
        if (!empty_entry && bb->entries[i].type == CD_BB_TYPE_NONE) {
            empty_entry = &bb->entries[i];
        }
    }
    if (!target_entry) {
        if (empty_entry) {
            target_entry = empty_entry;
        } else if (bb->entry_count < CD_BB_MAX_ENTRIES) {
            target_entry = &bb->entries[bb->entry_count++];
        } else {
            *error_code = -32603;
            *error_msg = cd_mcp_error_fmt("Blackboard full",
                "Maximum 32 entries per blackboard.",
                "Remove unused entries or use a different blackboard.");
            return NULL;
        }
    }

    snprintf(target_entry->key, CD_BB_MAX_KEY_LEN, "%s", key);

    if (cJSON_IsBool(value_json)) {
        target_entry->type = CD_BB_TYPE_BOOL;
        target_entry->value.b = cJSON_IsTrue(value_json);
    } else if (cJSON_IsNumber(value_json)) {
        double v = value_json->valuedouble;
        if (v == (int32_t)v && v >= INT32_MIN && v <= INT32_MAX) {
            target_entry->type = CD_BB_TYPE_INT;
            target_entry->value.i = (int32_t)v;
        } else {
            target_entry->type = CD_BB_TYPE_FLOAT;
            target_entry->value.f = (float)v;
        }
    } else if (cJSON_IsString(value_json)) {
        target_entry->type = CD_BB_TYPE_STRING;
        snprintf(target_entry->value.s, CD_BB_MAX_STR_LEN, "%s", value_json->valuestring);
    } else {
        *error_code = -32602;
        *error_msg = cd_mcp_error_fmt("Unsupported value type",
            "Blackboard values must be number, bool, or string.",
            "Example: {\"handle\": 0, \"key\": \"state\", \"value\": \"patrol\"}");
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", 1);
    return result;
}

/* ============================================================================
 * ai.getSensoryMemory
 * ============================================================================ */

static cJSON* handle_ai_get_sensory_memory(cd_kernel_t* kernel, const cJSON* params,
                                             int* error_code, const char** error_msg) {
    (void)kernel;

    if (!s_get_agent) {
        *error_code = -32603;
        *error_msg = cd_mcp_error_fmt("AI agent system not initialized",
            "The scripting_lua plugin must be loaded with AI agents enabled.",
            "Ensure your scene has scripts that use the AI agent API.");
        return NULL;
    }

    const cJSON* id_json = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (!id_json || !cJSON_IsNumber(id_json)) {
        *error_code = -32602;
        *error_msg = "Missing required parameter: id";
        return NULL;
    }

    uint32_t handle = (uint32_t)id_json->valuedouble;
    cd_ai_agent_mcp_t* agent = (cd_ai_agent_mcp_t*)s_get_agent(handle);
    if (!agent || !agent->active) {
        *error_code = -32602;
        *error_msg = cd_mcp_error_fmt("Agent not found",
            "No AI agent exists with the given id.",
            "Use ai.listAgents to see available agent IDs.");
        return NULL;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON* records = cJSON_AddArrayToObject(result, "records");

    for (uint32_t i = 0; i < agent->memory_count && i < CD_AI_MAX_MEMORY_RECORDS; i++) {
        cd_ai_memory_record_mcp_t* rec = &agent->memory[i];
        if (rec->entity_id == UINT64_MAX) continue;
        cJSON* r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "entity_id", (double)rec->entity_id);
        cJSON* pos = cJSON_AddArrayToObject(r, "position");
        cJSON_AddItemToArray(pos, cJSON_CreateNumber(rec->last_sensed_pos[0]));
        cJSON_AddItemToArray(pos, cJSON_CreateNumber(rec->last_sensed_pos[1]));
        cJSON_AddItemToArray(pos, cJSON_CreateNumber(rec->last_sensed_pos[2]));
        cJSON_AddNumberToObject(r, "time_became_visible", rec->time_became_visible);
        cJSON_AddNumberToObject(r, "time_last_visible", rec->time_last_visible);
        cJSON_AddNumberToObject(r, "time_last_sensed", rec->time_last_sensed);
        cJSON_AddBoolToObject(r, "within_fov", rec->within_fov);
        cJSON_AddBoolToObject(r, "shootable", rec->shootable);
        cJSON_AddItemToArray(records, r);
    }

    return result;
}

/* ============================================================================
 * ai.emitSound
 * ============================================================================ */

static cJSON* handle_ai_emit_sound(cd_kernel_t* kernel, const cJSON* params,
                                     int* error_code, const char** error_msg) {
    (void)kernel;

    if (!s_emit_sound) {
        *error_code = -32603;
        *error_msg = cd_mcp_error_fmt("AI sound system not initialized",
            "The sensory memory system must be enabled for sound events.",
            "Ensure AI agents are loaded with sensory memory support.");
        return NULL;
    }

    const cJSON* x_json = cJSON_GetObjectItemCaseSensitive(params, "x");
    const cJSON* y_json = cJSON_GetObjectItemCaseSensitive(params, "y");
    const cJSON* z_json = cJSON_GetObjectItemCaseSensitive(params, "z");
    const cJSON* r_json = cJSON_GetObjectItemCaseSensitive(params, "radius");
    const cJSON* t_json = cJSON_GetObjectItemCaseSensitive(params, "type");

    if (!x_json || !y_json || !z_json || !r_json) {
        *error_code = -32602;
        *error_msg = "Missing required parameters: x, y, z, radius";
        return NULL;
    }

    float x = (float)x_json->valuedouble;
    float y = (float)y_json->valuedouble;
    float z = (float)z_json->valuedouble;
    float radius = (float)r_json->valuedouble;
    const char* type = (t_json && cJSON_IsString(t_json)) ? t_json->valuestring : "generic";

    s_emit_sound(x, y, z, radius, type);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", 1);
    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_ai_tools(cd_mcp_server_t* server) {
    if (!server) return CD_ERR_NULL;

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "ai.listAgents", handle_ai_list_agents,
        "List all active AI agents with summary info.",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "ai.getAgentState", handle_ai_get_agent_state,
        "Get detailed state of an AI agent including perception, regulators, and memory.",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"number\",\"description\":\"Agent handle\"}"
        "},\"required\":[\"id\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "ai.setAgentState", handle_ai_set_agent_state,
        "Force an AI agent's LOD level or group for testing.",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"number\",\"description\":\"Agent handle\"},"
        "\"lod\":{\"type\":\"number\",\"description\":\"LOD level 0-3\"},"
        "\"group\":{\"type\":\"string\",\"description\":\"Group name\"}"
        "},\"required\":[\"id\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "ai.getBlackboard", handle_ai_get_blackboard,
        "Get all key-value entries from a blackboard.",
        "{\"type\":\"object\",\"properties\":{"
        "\"handle\":{\"type\":\"number\",\"description\":\"Blackboard handle\"}"
        "},\"required\":[\"handle\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "ai.setBlackboard", handle_ai_set_blackboard,
        "Set a key-value entry on a blackboard.",
        "{\"type\":\"object\",\"properties\":{"
        "\"handle\":{\"type\":\"number\",\"description\":\"Blackboard handle\"},"
        "\"key\":{\"type\":\"string\",\"description\":\"Entry key\"},"
        "\"value\":{\"description\":\"Value (number, bool, or string)\"}"
        "},\"required\":[\"handle\",\"key\",\"value\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "ai.getSensoryMemory",
        handle_ai_get_sensory_memory,
        "Get an agent's sensory memory records.",
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"number\",\"description\":\"Agent handle\"}"
        "},\"required\":[\"id\"]}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "ai.emitSound", handle_ai_emit_sound,
        "Inject a sound event at a world position for AI perception.",
        "{\"type\":\"object\",\"properties\":{"
        "\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},"
        "\"radius\":{\"type\":\"number\",\"description\":\"Audible radius\"},"
        "\"type\":{\"type\":\"string\",\"description\":\"Sound type label\"}"
        "},\"required\":[\"x\",\"y\",\"z\",\"radius\"]}");
    if (res != CD_OK) return res;

    return CD_OK;
}
