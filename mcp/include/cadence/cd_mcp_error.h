/* cd_mcp_error.h - Enhanced MCP error response formatting
 *
 * Provides helpers for building actionable error messages with context,
 * details, and suggestions. Used by MCP tool handlers to produce
 * error responses that help AI agents self-correct.
 *
 * Error response format (JSON-RPC 2.0 compatible):
 * {
 *   "code": -32602,
 *   "message": "property not found",
 *   "data": {
 *     "details": "component 'MeshRenderer' on node 'Cube' (id=5:1) has no property 'positon'",
 *     "suggestion": "Did you mean 'position'? Available properties: position, rotation, scale"
 *   }
 * }
 *
 * The tool handler signature uses const char* error_msg. To pass structured
 * errors, we use thread-local static buffers with a combined format:
 *   "message\ndetails\nsuggestion"
 *
 * The cd_mcp_send_error function parses this format and builds the structured
 * JSON response.
 */
#ifndef CD_MCP_ERROR_H
#define CD_MCP_ERROR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Maximum length for a formatted error message (message + details + suggestion).
 */
#define CD_MCP_ERROR_BUF_SIZE 1024

/**
 * Format a structured error message string.
 *
 * The returned pointer is to a static thread-local buffer.
 * The format is: "message\ndetails\nsuggestion"
 * where details and suggestion may be empty.
 *
 * The cd_mcp_send_error function recognizes this format and builds
 * structured JSON with data.details and data.suggestion fields.
 *
 * @param message     Short error summary (required).
 * @param details     Longer explanation with context (may be NULL).
 * @param suggestion  What to do next (may be NULL).
 * @return            Pointer to static buffer with combined string.
 */
const char* cd_mcp_error_fmt(const char* message,
                              const char* details,
                              const char* suggestion);

/**
 * Parse a structured error string into its components.
 *
 * If the string contains newlines, splits into message/details/suggestion.
 * Otherwise, the entire string is treated as the message.
 *
 * Output pointers are set to point within the provided mutable buffer.
 * The buffer is modified (newlines replaced with NUL).
 *
 * @param error_str    Mutable copy of the error string.
 * @param out_message  Receives pointer to message portion.
 * @param out_details  Receives pointer to details (or NULL if absent).
 * @param out_suggest  Receives pointer to suggestion (or NULL if absent).
 */
void cd_mcp_error_parse(char* error_str,
                          const char** out_message,
                          const char** out_details,
                          const char** out_suggest);

#ifdef __cplusplus
}
#endif

#endif /* CD_MCP_ERROR_H */
