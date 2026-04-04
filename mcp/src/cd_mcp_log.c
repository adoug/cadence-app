/* cd_mcp_log.c - Cadence Engine MCP log ring buffer implementation
 *
 * Task 19.3: Fixed-size circular buffer for engine log entries.
 * PB-3: State moved into cd_mcp_log_state_t (owned by cd_kernel_get_mcp_tool_state(kernel)).
 *       A module-level pointer is used for the convenience API; it defaults
 *       to a static fallback for standalone / test usage.
 */

#include "cadence/cd_mcp_log.h"
#include "cadence/cd_mcp_tool_state.h"
#include <string.h>

/* ============================================================================
 * Module-level log state pointer
 *
 * Defaults to a static fallback instance. When the kernel initializes MCP
 * tool state, call cd_mcp_log_set_state() to point at the kernel-owned
 * instance. This eliminates the separate static globals while keeping the
 * convenience API intact.
 * ============================================================================ */

static cd_mcp_log_state_t s_fallback_log_state;
static cd_mcp_log_state_t* s_log = &s_fallback_log_state;

void cd_mcp_log_set_state(cd_mcp_log_state_t* state) {
    s_log = state ? state : &s_fallback_log_state;
}

cd_mcp_log_state_t* cd_mcp_log_get_state(void) {
    return s_log;
}

/* ============================================================================
 * Level conversion
 * ============================================================================ */

const char* cd_log_level_to_str(cd_log_level_t level) {
    switch (level) {
        case CD_LOG_DEBUG: return "debug";
        case CD_LOG_INFO:  return "info";
        case CD_LOG_WARN:  return "warn";
        case CD_LOG_ERROR: return "error";
        default:           return "unknown";
    }
}

bool cd_log_level_from_str(const char* str, cd_log_level_t* out) {
    if (str == NULL || out == NULL) return false;

    if (strcmp(str, "debug") == 0) { *out = CD_LOG_DEBUG; return true; }
    if (strcmp(str, "info")  == 0) { *out = CD_LOG_INFO;  return true; }
    if (strcmp(str, "warn")  == 0) { *out = CD_LOG_WARN;  return true; }
    if (strcmp(str, "error") == 0) { *out = CD_LOG_ERROR; return true; }

    return false;
}

/* ============================================================================
 * Writing
 * ============================================================================ */

void cd_mcp_log_write(double timestamp, cd_log_level_t level,
                       const char* module, const char* message) {
    cd_mcp_log_entry_t* entry = &s_log->entries[s_log->head];

    entry->timestamp = timestamp;
    entry->level = level;
    s_log->seq++;
    entry->sequence = s_log->seq;

    /* Copy module name */
    if (module != NULL) {
        size_t len = strlen(module);
        if (len >= CD_MCP_LOG_MODULE_SIZE) {
            len = CD_MCP_LOG_MODULE_SIZE - 1;
        }
        memcpy(entry->module, module, len);
        entry->module[len] = '\0';
    } else {
        memcpy(entry->module, "engine", 7);
    }

    /* Copy message */
    if (message != NULL) {
        size_t len = strlen(message);
        if (len >= CD_MCP_LOG_MESSAGE_SIZE) {
            len = CD_MCP_LOG_MESSAGE_SIZE - 1;
        }
        memcpy(entry->message, message, len);
        entry->message[len] = '\0';
    } else {
        entry->message[0] = '\0';
    }

    /* Advance head (wraps around) */
    s_log->head = (s_log->head + 1) % CD_MCP_LOG_CAPACITY;
    if (s_log->count < CD_MCP_LOG_CAPACITY) {
        s_log->count++;
    }
}

/* ============================================================================
 * Internal: iterate entries in chronological order
 *
 * The ring buffer stores entries from oldest to newest. The oldest entry
 * is at (head - count) % CAPACITY, the newest at (head - 1) % CAPACITY.
 * ============================================================================ */

static uint32_t cd_log_oldest_index(void) {
    if (s_log->count < CD_MCP_LOG_CAPACITY) {
        return 0;
    }
    return s_log->head; /* head points to the slot that will be overwritten next */
}

/* ============================================================================
 * Query
 * ============================================================================ */

/** Internal: check if an entry matches the given filters. */
static bool cd_log_entry_matches(const cd_mcp_log_entry_t* e,
                                  double since_time, cd_log_level_t min_level,
                                  const char* module_filter,
                                  const char* text_filter) {
    /* Filter: timestamp */
    if (since_time > 0.0 && e->timestamp < since_time) {
        return false;
    }

    /* Filter: level */
    if (e->level < min_level) {
        return false;
    }

    /* Filter: module substring */
    if (module_filter != NULL && module_filter[0] != '\0') {
        if (strstr(e->module, module_filter) == NULL) {
            return false;
        }
    }

    /* Filter: message text substring */
    if (text_filter != NULL && text_filter[0] != '\0') {
        if (strstr(e->message, text_filter) == NULL) {
            return false;
        }
    }

    return true;
}

uint32_t cd_mcp_log_query(double since_time, cd_log_level_t min_level,
                           const char* module_filter, const char* text_filter,
                           uint32_t limit, uint32_t offset,
                           cd_mcp_log_entry_t* out, uint32_t out_capacity) {
    if (out == NULL || out_capacity == 0 || s_log->count == 0) {
        return 0;
    }

    uint32_t max_results = out_capacity;
    if (limit > 0 && limit < max_results) {
        max_results = limit;
    }

    uint32_t start = cd_log_oldest_index();
    uint32_t written = 0;
    uint32_t skipped = 0;

    for (uint32_t i = 0; i < s_log->count && written < max_results; i++) {
        uint32_t idx = (start + i) % CD_MCP_LOG_CAPACITY;
        const cd_mcp_log_entry_t* e = &s_log->entries[idx];

        if (!cd_log_entry_matches(e, since_time, min_level, module_filter, text_filter)) {
            continue;
        }

        /* Skip entries for offset (pagination) */
        if (skipped < offset) {
            skipped++;
            continue;
        }

        out[written++] = *e;
    }

    return written;
}

uint32_t cd_mcp_log_query_count(double since_time, cd_log_level_t min_level,
                                 const char* module_filter,
                                 const char* text_filter) {
    if (s_log->count == 0) {
        return 0;
    }

    uint32_t start = cd_log_oldest_index();
    uint32_t matched = 0;

    for (uint32_t i = 0; i < s_log->count; i++) {
        uint32_t idx = (start + i) % CD_MCP_LOG_CAPACITY;
        const cd_mcp_log_entry_t* e = &s_log->entries[idx];

        if (cd_log_entry_matches(e, since_time, min_level, module_filter, text_filter)) {
            matched++;
        }
    }

    return matched;
}

/* ============================================================================
 * Stream (poll since sequence)
 * ============================================================================ */

uint32_t cd_mcp_log_stream(uint64_t since_seq, cd_log_level_t min_level,
                            const char* module_filter,
                            cd_mcp_log_entry_t* out, uint32_t out_capacity) {
    if (out == NULL || out_capacity == 0 || s_log->count == 0) {
        return 0;
    }

    uint32_t start = cd_log_oldest_index();
    uint32_t written = 0;

    for (uint32_t i = 0; i < s_log->count && written < out_capacity; i++) {
        uint32_t idx = (start + i) % CD_MCP_LOG_CAPACITY;
        const cd_mcp_log_entry_t* e = &s_log->entries[idx];

        /* Only entries after since_seq */
        if (e->sequence <= since_seq) {
            continue;
        }

        /* Filter: level */
        if (e->level < min_level) {
            continue;
        }

        /* Filter: module substring */
        if (module_filter != NULL && module_filter[0] != '\0') {
            if (strstr(e->module, module_filter) == NULL) {
                continue;
            }
        }

        out[written++] = *e;
    }

    return written;
}

/* ============================================================================
 * Utilities
 * ============================================================================ */

uint64_t cd_mcp_log_latest_seq(void) {
    return s_log->seq;
}

uint32_t cd_mcp_log_count(void) {
    return s_log->count;
}

void cd_mcp_log_reset(void) {
    s_log->head = 0;
    s_log->count = 0;
    s_log->seq = 0;
    memset(s_log->entries, 0, sizeof(s_log->entries));
}
