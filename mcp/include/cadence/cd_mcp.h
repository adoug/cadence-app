/* cd_mcp.h - Cadence Engine MCP server types and API
 *
 * The MCP (Model Context Protocol) server is the primary external API for
 * Cadence. It reads JSON-RPC 2.0 requests from stdin, dispatches them to
 * registered tool handlers, and writes JSON-RPC 2.0 responses to stdout.
 *
 * Key design principles:
 * - Non-blocking I/O: cd_mcp_server_poll() never blocks the main loop
 * - Line-delimited JSON: each message is one line terminated by '\n'
 * - Tool dispatch table: handlers registered by method name
 * - cJSON library: all JSON parsing/generation uses vendored cJSON
 */
#ifndef CD_MCP_H
#define CD_MCP_H

#include <stdint.h>
#include <stdbool.h>

#include "cadence/cd_core_types.h"
#include "cadence/cd_mcp_async.h"

/* Forward declarations */
struct cd_kernel_t;
typedef struct cJSON cJSON;

/* ============================================================================
 * JSON-RPC 2.0 error codes
 * ============================================================================ */

/* MCP protocol version (spec date) */
#define CD_MCP_PROTOCOL_VERSION "2024-11-05"

#define CD_JSONRPC_PARSE_ERROR      (-32700)
#define CD_JSONRPC_INVALID_REQUEST  (-32600)
#define CD_JSONRPC_METHOD_NOT_FOUND (-32601)
#define CD_JSONRPC_INVALID_PARAMS   (-32602)
#define CD_JSONRPC_INTERNAL_ERROR   (-32603)

/* ============================================================================
 * Tool handler function signature
 *
 * Returns a cJSON* result object (ownership transferred to caller).
 * On error, sets *error_code and *error_msg and returns NULL.
 * ============================================================================ */

typedef cJSON* (*cd_mcp_tool_handler_t)(
    struct cd_kernel_t* kernel,
    const cJSON*        params,
    int*                error_code,
    const char**        error_msg
);

/* ============================================================================
 * Tool registration entry
 * ============================================================================ */

typedef struct {
    const char*            method;       /* e.g., "system.ping" */
    cd_mcp_tool_handler_t  handler;      /* Function pointer */
    const char*            description;  /* Human-readable, may be NULL */
    const char*            input_schema; /* JSON Schema string, may be NULL */
    uint32_t               flags;        /* 0 = synchronous, CD_MCP_TOOL_ASYNC = background thread */
} cd_mcp_tool_entry_t;

/* ============================================================================
 * Output callback for response writing
 *
 * By default, responses are written to stdout. For testing or alternative
 * transports, set server.output_fn to redirect output.
 * ============================================================================ */

typedef void (*cd_mcp_output_fn_t)(const char* json_line, void* user_data);

/* ============================================================================
 * MCP Server structure
 * ============================================================================ */

/** Initial capacity for the tool registry. */
#define CD_MCP_INITIAL_TOOL_CAPACITY 16

/** Read buffer size for accumulating partial stdin reads. */
#define CD_MCP_READ_BUFFER_SIZE 4096

typedef struct cd_mcp_server_t {
    /* Tool registry */
    cd_mcp_tool_entry_t* tools;
    uint32_t             tool_count;
    uint32_t             tool_capacity;

    /* Input buffer for accumulating partial reads */
    char*    read_buffer;
    uint32_t read_pos;
    uint32_t read_capacity;

    /* Output callback (NULL = write to stdout) */
    cd_mcp_output_fn_t output_fn;
    void*              output_user_data;

    /* Running state */
    bool initialized;
    bool handshake_done;  /* True after MCP initialize handshake */

    /* P2-4: Async dispatch pool for long-running tools */
    cd_mcp_async_pool_t async_pool;
} cd_mcp_server_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Initialize the MCP server.
 * Allocates the tool registry and read buffer.
 *
 * @param server  Pointer to server structure to initialize.
 * @return CD_OK on success, CD_ERR_NULL if server is NULL,
 *         CD_ERR_ALLOC on allocation failure.
 */
cd_result_t cd_mcp_server_init(cd_mcp_server_t* server);

/**
 * Shut down the MCP server.
 * Frees the tool registry and read buffer.
 *
 * @param server  Pointer to server structure. Safe to call with NULL.
 */
void cd_mcp_server_shutdown(cd_mcp_server_t* server);

/**
 * Register a tool handler for a given method name.
 *
 * @param server   Pointer to initialized server.
 * @param method   Method name string (not copied -- must outlive server).
 * @param handler  Tool handler function pointer.
 * @return CD_OK on success, CD_ERR_NULL if any argument is NULL,
 *         CD_ERR_ALLOC on allocation failure.
 */
cd_result_t cd_mcp_register_tool(cd_mcp_server_t* server,
                                  const char*       method,
                                  cd_mcp_tool_handler_t handler);

/**
 * Register a tool handler with description and JSON Schema.
 *
 * Extended version of cd_mcp_register_tool that stores metadata for
 * the MCP tools/list protocol method.
 *
 * @param server       Pointer to initialized server.
 * @param method       Method name string (not copied -- must outlive server).
 * @param handler      Tool handler function pointer.
 * @param description  Human-readable description (not copied, may be NULL).
 * @param input_schema JSON Schema string for params (not copied, may be NULL).
 * @return CD_OK on success, CD_ERR_NULL if server/method/handler is NULL,
 *         CD_ERR_ALLOC on allocation failure.
 */
cd_result_t cd_mcp_register_tool_ex(cd_mcp_server_t* server,
                                     const char*       method,
                                     cd_mcp_tool_handler_t handler,
                                     const char*       description,
                                     const char*       input_schema);

/**
 * Poll for and handle incoming JSON-RPC requests (non-blocking).
 * Called once per frame from the main loop. Reads available bytes from
 * stdin without blocking, accumulates them in the read buffer, and
 * processes any complete lines as JSON-RPC requests.
 *
 * @param server  Pointer to initialized server.
 * @param kernel  Pointer to the engine kernel (passed to tool handlers).
 * @return CD_OK on success, CD_ERR_NULL if server is NULL.
 */
cd_result_t cd_mcp_server_poll(cd_mcp_server_t* server,
                                struct cd_kernel_t* kernel);

/**
 * Process a single JSON-RPC 2.0 request string.
 * Parses the JSON, validates the structure, dispatches to the appropriate
 * tool handler, and writes the response to stdout.
 *
 * For notifications (no "id" field), the handler is called but no response
 * is written.
 *
 * @param server    Pointer to initialized server.
 * @param kernel    Pointer to the engine kernel.
 * @param json_str  The raw JSON-RPC request string.
 * @return CD_OK on success, CD_ERR_NULL if any argument is NULL,
 *         CD_ERR_PARSE on JSON parse error.
 */
cd_result_t cd_mcp_process_request(cd_mcp_server_t* server,
                                    struct cd_kernel_t* kernel,
                                    const char* json_str);

/**
 * P2-4: Mark known long-running tools for async dispatch.
 *
 * Call after cd_mcp_register_all_tools() so all tools are in the registry.
 * Sets CD_MCP_TOOL_ASYNC on tools that benefit from background execution
 * (e.g. lightmap baking, asset cooking, build export).
 */
void cd_mcp_mark_async_tools(cd_mcp_server_t* server);

/**
 * Send a JSON-RPC success response for a given request ID.
 *
 * Used by cd_mcp_async_poll to deliver results from completed background
 * tasks.  The result is wrapped in the standard tools/call content format.
 *
 * @param server      Pointer to initialized server.
 * @param request_id  The JSON-RPC request ID (numeric).
 * @param result      The result cJSON object (not consumed; caller retains ownership).
 */
void cd_mcp_server_send_result(cd_mcp_server_t* server,
                                uint64_t request_id,
                                const cJSON* result);

/**
 * Send a JSON-RPC error response for a given request ID.
 */
void cd_mcp_server_send_error(cd_mcp_server_t* server,
                               uint64_t request_id,
                               int code,
                               const char* message);

#endif /* CD_MCP_H */
