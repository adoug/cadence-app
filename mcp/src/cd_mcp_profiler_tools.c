/* cd_mcp_profiler_tools.c - Cadence Engine MCP profiler tool handlers (Sprint 8.3)
 *
 * Implements:
 *   - perf.profile_frame    : Returns hierarchical timing data for the last frame
 *   - perf.memory_report    : Returns memory usage across all subsystems
 *   - perf.profiler_enable  : Enable/disable the CPU profiler
 *
 * All handlers are read-only (no scene mutation), so calling from the MCP
 * thread is safe.
 */

#include "cadence/cd_mcp_tools.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_profiler.h"
#include "cadence/cd_memory.h"
#include "cJSON.h"

#include <string.h>

/* ============================================================================
 * perf.profile_frame handler
 *
 * Returns the profiler zones from the last completed frame.
 * If the profiler is disabled, returns an empty zones array.
 * ============================================================================ */

static cJSON* cd_mcp_handle_profile_frame(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel;
    (void)params;
    (void)error_code;
    (void)error_msg;

    cJSON* result = cJSON_CreateObject();
    if (!result) {
        *error_code = -32603; /* internal error */
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cd_profiler_t* p = &g_profiler;

    double frame_ms = p->frame_elapsed_us / 1000.0;
    cJSON_AddNumberToObject(result, "frame_ms", frame_ms);
    cJSON_AddBoolToObject(result, "enabled", p->enabled);

    cJSON* zones_arr = cJSON_AddArrayToObject(result, "zones");
    if (zones_arr && p->enabled) {
        for (uint32_t i = 0; i < p->zone_count; i++) {
            cd_profile_zone_t* z = &p->zones[i];
            cJSON* zone_obj = cJSON_CreateObject();
            if (!zone_obj) continue;

            cJSON_AddStringToObject(zone_obj, "name", z->name ? z->name : "unknown");
            cJSON_AddNumberToObject(zone_obj, "ms", z->elapsed_us / 1000.0);
            cJSON_AddNumberToObject(zone_obj, "depth", (double)z->depth);
            cJSON_AddNumberToObject(zone_obj, "call_count", (double)z->call_count);

            cJSON_AddItemToArray(zones_arr, zone_obj);
        }
    }

    return result;
}

/* ============================================================================
 * perf.memory_report handler
 *
 * Returns per-subsystem memory usage.
 * ============================================================================ */

static cJSON* cd_mcp_handle_memory_report(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)error_code;
    (void)error_msg;

    cJSON* result = cJSON_CreateObject();
    if (!result) {
        *error_code = -32603;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    cd_memory_profile_t profile;
    cd_memory_get_profile(kernel, &profile);

    double total_mb = (double)profile.total_heap_used / (1024.0 * 1024.0);
    double peak_mb  = (double)profile.total_heap_peak / (1024.0 * 1024.0);
    cJSON_AddNumberToObject(result, "total_heap_mb", total_mb);
    cJSON_AddNumberToObject(result, "peak_heap_mb", peak_mb);

    /* Global tracked allocation counters from kernel memory context */
    if (kernel) {
        cd_memory_t* mem = cd_kernel_get_memory(kernel);
        cJSON_AddNumberToObject(result, "total_allocated", (double)mem->total_allocated);
        cJSON_AddNumberToObject(result, "total_freed", (double)mem->total_freed);
        cJSON_AddNumberToObject(result, "active_allocations", (double)mem->active_allocations);

        /* Frame arena usage */
        if (mem->frame_arena.capacity > 0) {
            cJSON* fa = cJSON_AddObjectToObject(result, "frame_arena");
            if (fa) {
                cJSON_AddNumberToObject(fa, "capacity", (double)mem->frame_arena.capacity);
                cJSON_AddNumberToObject(fa, "used", (double)mem->frame_arena.offset);
                double usage_pct = (mem->frame_arena.capacity > 0)
                    ? ((double)mem->frame_arena.offset / (double)mem->frame_arena.capacity) * 100.0
                    : 0.0;
                cJSON_AddNumberToObject(fa, "usage_pct", usage_pct);
            }
        }

        /* Persistent arena usage */
        if (mem->persistent_arena.capacity > 0) {
            cJSON* pa = cJSON_AddObjectToObject(result, "persistent_arena");
            if (pa) {
                cJSON_AddNumberToObject(pa, "capacity", (double)mem->persistent_arena.capacity);
                cJSON_AddNumberToObject(pa, "used", (double)mem->persistent_arena.offset);
                double usage_pct = (mem->persistent_arena.capacity > 0)
                    ? ((double)mem->persistent_arena.offset / (double)mem->persistent_arena.capacity) * 100.0
                    : 0.0;
                cJSON_AddNumberToObject(pa, "usage_pct", usage_pct);
            }
        }
    }

    /* Per-subsystem breakdown (arenas, pools) */
    cJSON* subs_arr = cJSON_AddArrayToObject(result, "subsystems");
    if (subs_arr) {
        for (uint32_t i = 0; i < profile.subsystem_count; i++) {
            cd_memory_subsystem_stats_t* s = &profile.subsystems[i];
            cJSON* sub = cJSON_CreateObject();
            if (!sub) continue;

            cJSON_AddStringToObject(sub, "name", s->name ? s->name : "unknown");

            /* Use MB for large subsystems (>= 1MB capacity), KB otherwise */
            if (s->capacity >= 1024 * 1024) {
                cJSON_AddNumberToObject(sub, "capacity_mb",
                    (double)s->capacity / (1024.0 * 1024.0));
                cJSON_AddNumberToObject(sub, "used_mb",
                    (double)s->used / (1024.0 * 1024.0));
                cJSON_AddNumberToObject(sub, "peak_mb",
                    (double)s->peak / (1024.0 * 1024.0));
            } else {
                cJSON_AddNumberToObject(sub, "capacity_kb",
                    (double)s->capacity / 1024.0);
                cJSON_AddNumberToObject(sub, "used_kb",
                    (double)s->used / 1024.0);
                cJSON_AddNumberToObject(sub, "peak_kb",
                    (double)s->peak / 1024.0);
            }

            if (s->alloc_count > 0) {
                cJSON_AddNumberToObject(sub, "count", (double)s->alloc_count);
            }

            cJSON_AddItemToArray(subs_arr, sub);
        }
    }

    /* Per-tag allocation breakdown */
    cJSON* tag_arr = cJSON_AddArrayToObject(result, "per_tag");
    if (tag_arr) {
        for (int t = 0; t < (int)CD_MEM_TAG_COUNT; t++) {
            cd_mem_tag_stats_t tag_stats;
            if (cd_mem_get_tag_stats((cd_mem_tag_t)t, &tag_stats) != CD_OK) {
                continue;
            }
            /* Skip tags with no activity */
            if (tag_stats.current_bytes == 0 && tag_stats.peak_bytes == 0 &&
                tag_stats.allocation_count == 0) {
                continue;
            }

            cJSON* tag_obj = cJSON_CreateObject();
            if (!tag_obj) continue;

            cJSON_AddStringToObject(tag_obj, "tag", cd_mem_tag_name((cd_mem_tag_t)t));
            cJSON_AddNumberToObject(tag_obj, "current_bytes", (double)tag_stats.current_bytes);
            cJSON_AddNumberToObject(tag_obj, "peak_bytes", (double)tag_stats.peak_bytes);
            cJSON_AddNumberToObject(tag_obj, "allocations", (double)tag_stats.allocation_count);

            cJSON_AddItemToArray(tag_arr, tag_obj);
        }
    }

    /* Optional: reset peak stats after collecting the report */
    if (params) {
        const cJSON* reset_item = cJSON_GetObjectItemCaseSensitive(params, "reset_peaks");
        if (cJSON_IsTrue(reset_item)) {
            cd_mem_reset_peak_stats();
        }
    }

    return result;
}

/* ============================================================================
 * perf.profiler_enable handler
 *
 * Enable or disable the CPU profiler.
 * Input: { "enable": true/false }
 * ============================================================================ */

static cJSON* cd_mcp_handle_profiler_enable(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg)
{
    (void)kernel;

    cJSON* result = cJSON_CreateObject();
    if (!result) {
        *error_code = -32603;
        *error_msg  = "Failed to allocate JSON response";
        return NULL;
    }

    bool enable = true; /* default to enable if param not specified */
    if (params) {
        const cJSON* enable_item = cJSON_GetObjectItemCaseSensitive(params, "enable");
        if (cJSON_IsBool(enable_item)) {
            enable = cJSON_IsTrue(enable_item) ? true : false;
        }
    }

    cd_profiler_t* p = &g_profiler;

    if (enable && !p->enabled) {
        cd_profiler_init(p);
        p->enabled = true;
    } else if (!enable) {
        p->enabled = false;
    }

    cJSON_AddBoolToObject(result, "enabled", p->enabled);

    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

cd_result_t cd_mcp_register_profiler_tools(cd_mcp_server_t* server) {
    if (!server) return CD_ERR_NULL;

    cd_result_t res;

    res = cd_mcp_register_tool_ex(server, "perf.profile_frame", cd_mcp_handle_profile_frame,
        "Return hierarchical timing data for the last completed frame.",
        "{\"type\":\"object\",\"properties\":{}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "perf.memory_report", cd_mcp_handle_memory_report,
        "Return memory usage across all subsystems with per-tag breakdown.",
        "{\"type\":\"object\",\"properties\":{"
        "\"reset_peaks\":{\"type\":\"boolean\",\"description\":\"If true, reset peak stats after collecting the report\"}"
        "}}");
    if (res != CD_OK) return res;

    res = cd_mcp_register_tool_ex(server, "perf.profiler_enable", cd_mcp_handle_profiler_enable,
        "Enable or disable the CPU profiler.",
        "{\"type\":\"object\",\"properties\":{"
        "\"enable\":{\"type\":\"boolean\",\"description\":\"True to enable, false to disable\"}"
        "}}");
    if (res != CD_OK) return res;

    return CD_OK;
}
