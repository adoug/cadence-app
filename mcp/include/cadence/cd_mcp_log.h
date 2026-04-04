/* cd_mcp_log.h - Cadence Engine MCP log ring buffer
 *
 * Task 19.3: log.stream + log.query MCP tools.
 *
 * Provides a fixed-size ring buffer of log entries that engine subsystems
 * can write to via cd_mcp_log_write(). MCP tools (log.stream, log.query)
 * read from this buffer.
 *
 * The buffer is module-level static — no heap allocation needed.
 */
#ifndef CD_MCP_LOG_H
#define CD_MCP_LOG_H

#include <stdint.h>
#include <stdbool.h>
#include "cadence/cd_core_types.h"

/* ============================================================================
 * Constants
 * ============================================================================ */

/** Maximum number of log entries in the ring buffer. */
#define CD_MCP_LOG_CAPACITY     1024

/** Maximum length of a log module name (including NUL). */
#define CD_MCP_LOG_MODULE_SIZE  64

/** Maximum length of a log message (including NUL). */
#define CD_MCP_LOG_MESSAGE_SIZE 512

/* ============================================================================
 * Log level
 * ============================================================================ */

typedef enum {
    CD_LOG_DEBUG = 0,
    CD_LOG_INFO  = 1,
    CD_LOG_WARN  = 2,
    CD_LOG_ERROR = 3,
} cd_log_level_t;

/* ============================================================================
 * Log entry
 * ============================================================================ */

typedef struct {
    double          timestamp;                       /* Engine time (seconds) */
    cd_log_level_t  level;
    char            module[CD_MCP_LOG_MODULE_SIZE];  /* e.g. "renderer", "physics" */
    char            message[CD_MCP_LOG_MESSAGE_SIZE];
    uint64_t        sequence;                        /* Monotonic sequence number */
} cd_mcp_log_entry_t;

/* ============================================================================
 * Public API — Writing
 * ============================================================================ */

/**
 * Write a log entry to the ring buffer.
 *
 * Thread-safe for single-writer (main thread). The entry is copied into
 * the ring buffer; oldest entries are overwritten when full.
 *
 * @param timestamp  Engine time in seconds.
 * @param level      Log severity level.
 * @param module     Module name (e.g. "renderer"). NULL treated as "engine".
 * @param message    Log message text. NULL treated as empty string.
 */
void cd_mcp_log_write(double timestamp, cd_log_level_t level,
                       const char* module, const char* message);

/* ============================================================================
 * Public API — Reading / Querying
 * ============================================================================ */

/**
 * Query log entries matching the given filters.
 *
 * Scans the ring buffer for entries matching the criteria. Results are
 * written to the provided output array in chronological order.
 *
 * @param since_time    Minimum timestamp (0.0 = no minimum).
 * @param min_level     Minimum severity (CD_LOG_DEBUG = all levels).
 * @param module_filter Module substring filter (NULL = all modules).
 * @param text_filter   Message substring filter (NULL = all messages).
 * @param limit         Maximum entries to return (0 = unlimited up to out_capacity).
 * @param offset        Number of matching entries to skip (pagination).
 * @param out           Output array for matching entries.
 * @param out_capacity  Size of the output array.
 * @return Number of entries written to out.
 */
uint32_t cd_mcp_log_query(double since_time, cd_log_level_t min_level,
                           const char* module_filter, const char* text_filter,
                           uint32_t limit, uint32_t offset,
                           cd_mcp_log_entry_t* out, uint32_t out_capacity);

/**
 * Count total matching log entries (for pagination metadata).
 *
 * Same filtering as cd_mcp_log_query but only counts, does not copy.
 *
 * @param since_time    Minimum timestamp (0.0 = no minimum).
 * @param min_level     Minimum severity (CD_LOG_DEBUG = all levels).
 * @param module_filter Module substring filter (NULL = all modules).
 * @param text_filter   Message substring filter (NULL = all messages).
 * @return Number of matching entries in the buffer.
 */
uint32_t cd_mcp_log_query_count(double since_time, cd_log_level_t min_level,
                                 const char* module_filter,
                                 const char* text_filter);

/**
 * Stream (poll) log entries since the given sequence number.
 *
 * Returns entries with sequence > since_seq. Use the returned entries'
 * sequence numbers to track the read cursor.
 *
 * @param since_seq     Return entries after this sequence (0 = all).
 * @param min_level     Minimum severity (CD_LOG_DEBUG = all levels).
 * @param module_filter Module substring filter (NULL = all modules).
 * @param out           Output array for matching entries.
 * @param out_capacity  Size of the output array.
 * @return Number of entries written to out.
 */
uint32_t cd_mcp_log_stream(uint64_t since_seq, cd_log_level_t min_level,
                            const char* module_filter,
                            cd_mcp_log_entry_t* out, uint32_t out_capacity);

/**
 * Get the current highest sequence number in the buffer.
 *
 * @return Latest sequence number, or 0 if buffer is empty.
 */
uint64_t cd_mcp_log_latest_seq(void);

/**
 * Clear all log entries and reset the buffer.
 * Intended for testing.
 */
void cd_mcp_log_reset(void);

/**
 * Get the total number of entries currently in the buffer.
 *
 * @return Entry count (0 to CD_MCP_LOG_CAPACITY).
 */
uint32_t cd_mcp_log_count(void);

/* ============================================================================
 * Utility — level string conversion
 * ============================================================================ */

/**
 * Convert a log level enum to its string name.
 *
 * @param level  Log level.
 * @return Static string: "debug", "info", "warn", or "error".
 */
const char* cd_log_level_to_str(cd_log_level_t level);

/**
 * Parse a log level from a string.
 *
 * @param str    String to parse ("debug", "info", "warn", "error").
 * @param out    Output level.
 * @return true if parsed successfully, false if unrecognized.
 */
bool cd_log_level_from_str(const char* str, cd_log_level_t* out);

/* ============================================================================
 * State management (PB-3)
 *
 * The log ring buffer state is normally owned by cd_kernel_get_mcp_tool_state(kernel).
 * Call cd_mcp_log_set_state() to point the module at a specific state
 * instance; pass NULL to revert to the internal fallback.
 * ============================================================================ */

/* Forward declaration (full definition in cd_mcp_tool_state.h) */
struct cd_mcp_log_state_t;

/** Set the active log state. NULL reverts to the internal fallback. */
void cd_mcp_log_set_state(struct cd_mcp_log_state_t* state);

/** Get the active log state pointer. */
struct cd_mcp_log_state_t* cd_mcp_log_get_state(void);

#endif /* CD_MCP_LOG_H */
