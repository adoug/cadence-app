/* cd_mcp_error.c - Enhanced MCP error response formatting
 *
 * Implements structured error message formatting with message/details/suggestion.
 * Uses a static buffer (no malloc) for the combined format string.
 */

#include "cadence/cd_mcp_error.h"

#include <stdio.h>
#include <string.h>

/* ============================================================================
 * Static buffer for formatted error messages.
 *
 * Thread safety note: The MCP server is single-threaded (all dispatch on
 * the main thread), so a single static buffer is safe. If multi-threaded
 * dispatch is ever added, this must become thread-local.
 * ============================================================================ */

static char s_error_buf[CD_MCP_ERROR_BUF_SIZE];

const char* cd_mcp_error_fmt(const char* message,
                              const char* details,
                              const char* suggestion) {
    if (message == NULL) {
        message = "Unknown error";
    }

    /* If no details or suggestion, just return the message as-is */
    if ((details == NULL || details[0] == '\0') &&
        (suggestion == NULL || suggestion[0] == '\0')) {
        /* Copy to static buffer for safety */
        snprintf(s_error_buf, sizeof(s_error_buf), "%s", message);
        return s_error_buf;
    }

    /* Format: "message\ndetails\nsuggestion" */
    snprintf(s_error_buf, sizeof(s_error_buf), "%s\n%s\n%s",
             message,
             (details != NULL) ? details : "",
             (suggestion != NULL) ? suggestion : "");

    return s_error_buf;
}

void cd_mcp_error_parse(char* error_str,
                          const char** out_message,
                          const char** out_details,
                          const char** out_suggest) {
    *out_message = error_str;
    *out_details = NULL;
    *out_suggest = NULL;

    if (error_str == NULL) {
        *out_message = "Unknown error";
        return;
    }

    /* Find first newline -> details */
    char* nl1 = strchr(error_str, '\n');
    if (nl1 == NULL) {
        return; /* No structured data — whole string is the message */
    }

    *nl1 = '\0';
    char* details_start = nl1 + 1;

    if (details_start[0] != '\0') {
        /* Find second newline -> suggestion */
        char* nl2 = strchr(details_start, '\n');
        if (nl2 != NULL) {
            *nl2 = '\0';
            char* suggest_start = nl2 + 1;
            if (suggest_start[0] != '\0') {
                *out_suggest = suggest_start;
            }
        }
        if (details_start[0] != '\0') {
            *out_details = details_start;
        }
    }
}
