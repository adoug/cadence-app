/* cd_mcp_server.c - Cadence Engine MCP server implementation
 *
 * JSON-RPC 2.0 stdio transport for the MCP tool surface.
 *
 * ============================================================================
 * THREADING MODEL (Task S3.5 audit)
 * ============================================================================
 *
 * The MCP server uses a SINGLE-THREADED POLLING model. There are no
 * background threads for request processing. All I/O and dispatch happens
 * on the main engine thread:
 *
 * Threads:
 *   - Main thread: runs cd_engine_tick() which calls pre_tick_fn (mcp_pre_tick)
 *     each frame. This polls all transports and dispatches tool handlers.
 *
 * Transports (all polled from the main thread):
 *   - stdio:     cd_mcp_server_poll() — non-blocking stdin read via
 *                PeekNamedPipe (Win32) or select() (POSIX)
 *   - TCP:       cd_mcp_tcp_poll() — select() with zero timeout
 *   - WebSocket: cd_mcp_ws_poll() — select() with zero timeout
 *
 * Dispatch path (all on main thread):
 *   1. Transport reads complete lines from network/stdin
 *   2. Transport sets current agent ID (cd_mcp_set_current_agent)
 *   3. Transport calls cd_mcp_process_request()
 *   4. Process_request looks up tool handler, calls it
 *   5. Tool handler may call cd_command_execute_sync() which directly
 *      mutates scene state (safe: we are on the main thread)
 *   6. Response is written back via the transport's output callback
 *
 * Thread safety of shared state:
 *   - cd_command_queue_t.lock (mutex): protects the pending queue and
 *     undo/redo stacks. Enqueue is safe from any thread. Flush runs on
 *     main thread only.
 *   - cd_command_execute_sync(): executes on the main thread, mutates
 *     scene directly (no mutex needed for scene access). Uses the queue
 *     mutex only for the undo stack push.
 *   - cd_mcp_set_current_agent(): writes a module-level variable.
 *     Safe because dispatch is single-threaded. NOT safe if transports
 *     were ever moved to background threads.
 *   - mcp->output_fn / output_user_data: temporarily swapped by TCP/WS
 *     transports per-client. Safe because dispatch is single-threaded.
 *     NOT safe if transports were ever moved to background threads.
 *   - cd_mcp_tool_state_t: per-agent state indexed by agent ID.
 *     Safe because dispatch is single-threaded and agent IDs are unique
 *     per connection.
 *
 * If multi-threaded dispatch is ever needed, the following must be
 * addressed:
 *   1. cd_mcp_set_current_agent must become thread-local storage
 *   2. mcp->output_fn swapping must be replaced with per-request context
 *   3. cd_command_execute_sync scene mutations must be serialized
 *   4. cd_mcp_tool_state_t access must be protected per-agent
 * ============================================================================
 *
 * Memory strategy: Uses platform calloc/realloc for the tool registry and
 * read buffer. This is a documented exception to the "no raw malloc" rule,
 * as these arrays need realloc growth semantics that arenas/pools do not
 * provide.
 */

#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_agent.h"
#include "cadence/cd_mcp_async.h"
#include "cadence/cd_mcp_error.h"
#include "cadence/cd_mcp_fuzzy_match.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"
#include "cadence/cd_asset_db.h"
#include "cadence/cd_platform.h"
#include "cadence/cd_memory.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * P2-4: Trampoline from async work_fn signature to tool handler signature
 *
 * cd_mcp_async_work_fn has an extra void* user_data parameter that
 * cd_mcp_tool_handler_t does not.  Rather than casting the function
 * pointer (which is technically UB), we heap-allocate a tiny trampoline
 * context and route through this wrapper.  The async thread frees the
 * trampoline via the user_data pointer after the call returns.
 * ============================================================================ */

typedef struct {
    cd_mcp_tool_handler_t handler;
} cd_async_trampoline_t;

static cJSON* cd_mcp_async_trampoline(struct cd_kernel_t* kernel,
                                       const cJSON* params,
                                       int* error_code,
                                       const char** error_msg,
                                       void* user_data) {
    cd_async_trampoline_t* t = (cd_async_trampoline_t*)user_data;
    return t->handler(kernel, params, error_code, error_msg);
}

/* ============================================================================
 * Platform-specific non-blocking stdin reading
 * ============================================================================ */

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>

/**
 * Read available bytes from stdin without blocking (Windows).
 * Uses PeekNamedPipe to check if data is available, then ReadFile.
 *
 * @param buf   Buffer to read into.
 * @param size  Maximum number of bytes to read.
 * @return Number of bytes read, or 0 if no data available, or -1 on error.
 */
static int cd_mcp_read_stdin_nonblocking(char* buf, int size) {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD avail = 0;

    if (h == INVALID_HANDLE_VALUE || h == NULL) {
        return -1;
    }

    /* Check if stdin is a pipe (typical for MCP stdio transport) */
    DWORD file_type = GetFileType(h);
    if (file_type == FILE_TYPE_PIPE) {
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
            return -1;
        }
        if (avail == 0) {
            return 0;
        }
    } else if (file_type == FILE_TYPE_CHAR) {
        /* Console input -- use WaitForSingleObject with 0 timeout */
        DWORD result = WaitForSingleObject(h, 0);
        if (result != WAIT_OBJECT_0) {
            return 0;
        }
    } else {
        /* FILE_TYPE_DISK or unknown -- cannot poll, skip */
        return 0;
    }

    DWORD bytes_read = 0;
    if (!ReadFile(h, buf, (DWORD)size, &bytes_read, NULL)) {
        return -1;
    }

    return (int)bytes_read;
}

#else /* POSIX */
#include <sys/select.h>
#include <unistd.h>

/**
 * Read available bytes from stdin without blocking (POSIX).
 * Uses select() with 0 timeout.
 *
 * @param buf   Buffer to read into.
 * @param size  Maximum number of bytes to read.
 * @return Number of bytes read, or 0 if no data available, or -1 on error.
 */
static int cd_mcp_read_stdin_nonblocking(char* buf, int size) {
    fd_set fds;
    struct timeval tv;

    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);

    /* 0 timeout = non-blocking poll */
    tv.tv_sec  = 0;
    tv.tv_usec = 0;

    int ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) {
        return 0; /* No data or error */
    }

    return (int)read(STDIN_FILENO, buf, (size_t)size);
}

#endif /* _WIN32 */

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

/**
 * Write a JSON-RPC response string using the server's output function.
 * Defaults to stdout if no output callback is set.
 */
static void cd_mcp_write_response(cd_mcp_server_t* server, const char* json_str) {
    if (server->output_fn != NULL) {
        server->output_fn(json_str, server->output_user_data);
    } else {
        fprintf(stdout, "%s\n", json_str);
        fflush(stdout);
    }
}

/**
 * Build and write a JSON-RPC 2.0 error response.
 *
 * @param server     The MCP server (for output routing).
 * @param id_item    The "id" from the request (may be number or string).
 *                   If NULL, this is a notification and no response is sent.
 * @param code       JSON-RPC error code.
 * @param message    Human-readable error message.
 */
static void cd_mcp_send_error(cd_mcp_server_t* server,
                               const cJSON* id_item,
                               int code,
                               const char* message) {
    if (id_item == NULL) {
        return; /* Notification -- no response */
    }

    cJSON* response = cJSON_CreateObject();
    if (response == NULL) {
        return;
    }

    cJSON_AddStringToObject(response, "jsonrpc", "2.0");

    /* Preserve the id type (number or string) */
    if (cJSON_IsString(id_item)) {
        cJSON_AddStringToObject(response, "id", id_item->valuestring);
    } else if (cJSON_IsNumber(id_item)) {
        cJSON_AddNumberToObject(response, "id", id_item->valuedouble);
    } else {
        cJSON_AddNullToObject(response, "id");
    }

    cJSON* error = cJSON_CreateObject();
    if (error != NULL) {
        cJSON_AddNumberToObject(error, "code", code);

        /* Parse structured error format: "message\ndetails\nsuggestion" */
        char msg_buf[CD_MCP_ERROR_BUF_SIZE];
        snprintf(msg_buf, sizeof(msg_buf), "%s", message ? message : "Unknown error");

        const char* msg_part = NULL;
        const char* details_part = NULL;
        const char* suggest_part = NULL;
        cd_mcp_error_parse(msg_buf, &msg_part, &details_part, &suggest_part);

        cJSON_AddStringToObject(error, "message", msg_part);

        /* Add data object if we have details or suggestion */
        if (details_part != NULL || suggest_part != NULL) {
            cJSON* data = cJSON_CreateObject();
            if (data != NULL) {
                if (details_part != NULL) {
                    cJSON_AddStringToObject(data, "details", details_part);
                }
                if (suggest_part != NULL) {
                    cJSON_AddStringToObject(data, "suggestion", suggest_part);
                }
                cJSON_AddItemToObject(error, "data", data);
            }
        }

        cJSON_AddItemToObject(response, "error", error);
    }

    char* output = cJSON_PrintUnformatted(response);
    if (output != NULL) {
        cd_mcp_write_response(server, output);
        cJSON_free(output);
    }

    cJSON_Delete(response);
}

/**
 * Build and write a JSON-RPC 2.0 success response.
 *
 * @param server     The MCP server (for output routing).
 * @param id_item    The "id" from the request. If NULL, no response is sent.
 * @param result     The result object (ownership is NOT transferred -- caller
 *                   still owns it and must free it).
 */
static void cd_mcp_send_result(cd_mcp_server_t* server,
                                const cJSON* id_item,
                                const cJSON* result) {
    if (id_item == NULL) {
        return; /* Notification -- no response */
    }

    cJSON* response = cJSON_CreateObject();
    if (response == NULL) {
        return;
    }

    cJSON_AddStringToObject(response, "jsonrpc", "2.0");

    /* Preserve the id type */
    if (cJSON_IsString(id_item)) {
        cJSON_AddStringToObject(response, "id", id_item->valuestring);
    } else if (cJSON_IsNumber(id_item)) {
        cJSON_AddNumberToObject(response, "id", id_item->valuedouble);
    } else {
        cJSON_AddNullToObject(response, "id");
    }

    /* Add a duplicate of the result so caller retains ownership */
    cJSON* result_copy = cJSON_Duplicate(result, 1);
    if (result_copy != NULL) {
        /* Inject agentId into result for multi-agent coordination (Task 19.2) */
        uint32_t agent = cd_mcp_get_current_agent();
        char agent_buf[16];
        cd_mcp_agent_id_format(agent, agent_buf, sizeof(agent_buf));
        cJSON_AddStringToObject(result_copy, "agentId", agent_buf);

        cJSON_AddItemToObject(response, "result", result_copy);
    }

    char* output = cJSON_PrintUnformatted(response);
    if (output != NULL) {
        cd_mcp_write_response(server, output);
        cJSON_free(output);
    }

    cJSON_Delete(response);
}

/**
 * Look up a tool handler by method name.
 *
 * @param server  The MCP server.
 * @param method  Method name to search for.
 * @return Pointer to the tool entry, or NULL if not found.
 */
static cd_mcp_tool_entry_t* cd_mcp_find_tool(cd_mcp_server_t* server,
                                               const char* method) {
    for (uint32_t i = 0; i < server->tool_count; i++) {
        if (strcmp(server->tools[i].method, method) == 0) {
            return &server->tools[i];
        }
    }
    return NULL;
}

/* ============================================================================
 * MCP name normalization — dots ↔ underscores
 *
 * The MCP spec requires tool names to match [a-zA-Z0-9_-]{1,64}.
 * Internal tools use dot separators (e.g. "system.ping").
 * For standard MCP clients we translate: dots → underscores in tools/list,
 * underscores → dots in tools/call lookup (with fallback to original name).
 * ============================================================================ */

/**
 * Convert internal dotted name to MCP-safe name.
 * Dots become double-underscores: "camera.set_mode" → "camera__set_mode"
 * Single underscores in the original are preserved.
 */
static void cd_mcp_name_to_mcp(const char* src, char* dst, size_t dst_size) {
    size_t di = 0;
    for (const char* p = src; *p && di + 2 < dst_size; p++) {
        if (*p == '.') {
            dst[di++] = '_';
            dst[di++] = '_';
        } else {
            dst[di++] = *p;
        }
    }
    dst[di] = '\0';
}

/**
 * Convert MCP-safe name back to internal dotted name.
 * Double-underscores become dots: "camera__set_mode" → "camera.set_mode"
 * Single underscores are preserved.
 */
static void cd_mcp_name_from_mcp(const char* src, char* dst, size_t dst_size) {
    size_t di = 0;
    for (const char* p = src; *p && di + 1 < dst_size; p++) {
        if (p[0] == '_' && p[1] == '_') {
            dst[di++] = '.';
            p++; /* skip second underscore */
        } else {
            dst[di++] = *p;
        }
    }
    dst[di] = '\0';
}

/* ============================================================================
 * P2-4: Mark known long-running tools for async dispatch
 * ============================================================================ */

void cd_mcp_mark_async_tools(cd_mcp_server_t* server) {
    static const char* async_tools[] = {
        "scene.bake_lightmaps",
        "build.export",
        "asset.cook_all",
        "asset.compress_texture",
        NULL
    };
    for (const char** name = async_tools; *name; name++) {
        cd_mcp_tool_entry_t* tool = cd_mcp_find_tool(server, *name);
        if (tool) tool->flags |= CD_MCP_TOOL_ASYNC;
    }
}

/* ============================================================================
 * Public response sending for async tasks (P2-4 / 1A)
 * ============================================================================ */

void cd_mcp_server_send_result(cd_mcp_server_t* server,
                                uint64_t request_id,
                                const cJSON* result) {
    if (!server) return;
    cJSON id_node;
    memset(&id_node, 0, sizeof(id_node));
    id_node.type = cJSON_Number;
    id_node.valuedouble = (double)request_id;
    cd_mcp_send_result(server, &id_node, result);
}

void cd_mcp_server_send_error(cd_mcp_server_t* server,
                               uint64_t request_id,
                               int code,
                               const char* message) {
    if (!server) return;
    cJSON id_node;
    memset(&id_node, 0, sizeof(id_node));
    id_node.type = cJSON_Number;
    id_node.valuedouble = (double)request_id;
    cd_mcp_send_error(server, &id_node, code, message);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

cd_result_t cd_mcp_server_init(cd_mcp_server_t* server) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    memset(server, 0, sizeof(cd_mcp_server_t));

    /* Allocate tool registry */
    server->tools = (cd_mcp_tool_entry_t*)calloc(
        CD_MCP_INITIAL_TOOL_CAPACITY, sizeof(cd_mcp_tool_entry_t));
    if (server->tools == NULL) {
        return CD_ERR_ALLOC;
    }
    server->tool_count    = 0;
    server->tool_capacity = CD_MCP_INITIAL_TOOL_CAPACITY;

    /* Allocate read buffer */
    server->read_buffer = (char*)calloc(CD_MCP_READ_BUFFER_SIZE, 1);
    if (server->read_buffer == NULL) {
        free(server->tools);
        server->tools = NULL;
        return CD_ERR_ALLOC;
    }
    server->read_pos      = 0;
    server->read_capacity = CD_MCP_READ_BUFFER_SIZE;

    /* Default output: stdout (output_fn = NULL means use fprintf) */
    server->output_fn        = NULL;
    server->output_user_data = NULL;

    /* P2-4: Initialize async task pool */
    cd_mcp_async_init(&server->async_pool);

    server->initialized = true;

    return CD_OK;
}

void cd_mcp_server_shutdown(cd_mcp_server_t* server) {
    if (server == NULL) {
        return;
    }

    /* P2-4: Shut down async pool (waits for running tasks) */
    cd_mcp_async_shutdown(&server->async_pool);

    if (server->tools != NULL) {
        free(server->tools);
        server->tools = NULL;
    }
    server->tool_count    = 0;
    server->tool_capacity = 0;

    if (server->read_buffer != NULL) {
        free(server->read_buffer);
        server->read_buffer = NULL;
    }
    server->read_pos      = 0;
    server->read_capacity = 0;

    server->output_fn        = NULL;
    server->output_user_data = NULL;

    server->initialized = false;
}

cd_result_t cd_mcp_register_tool_ex(cd_mcp_server_t* server,
                                     const char*       method,
                                     cd_mcp_tool_handler_t handler,
                                     const char*       description,
                                     const char*       input_schema) {
    if (server == NULL || method == NULL || handler == NULL) {
        return CD_ERR_NULL;
    }

    /* Grow registry if needed */
    if (server->tool_count >= server->tool_capacity) {
        uint32_t new_cap = server->tool_capacity * 2;
        cd_mcp_tool_entry_t* new_tools = (cd_mcp_tool_entry_t*)realloc(
            server->tools, new_cap * sizeof(cd_mcp_tool_entry_t));
        if (new_tools == NULL) {
            return CD_ERR_ALLOC;
        }
        server->tools         = new_tools;
        server->tool_capacity = new_cap;
    }

    /* Append new tool entry */
    cd_mcp_tool_entry_t* entry = &server->tools[server->tool_count];
    entry->method       = method;
    entry->handler      = handler;
    entry->description  = description;
    entry->input_schema = input_schema;
    server->tool_count++;

    return CD_OK;
}

cd_result_t cd_mcp_register_tool(cd_mcp_server_t* server,
                                  const char*       method,
                                  cd_mcp_tool_handler_t handler) {
    return cd_mcp_register_tool_ex(server, method, handler, NULL, NULL);
}

cd_result_t cd_mcp_process_request(cd_mcp_server_t* server,
                                    struct cd_kernel_t* kernel,
                                    const char* json_str) {
    if (server == NULL || json_str == NULL) {
        return CD_ERR_NULL;
    }

    /* Step 1: Parse JSON */
    cJSON* request = cJSON_Parse(json_str);
    if (request == NULL) {
        /* Parse error -- send error with null id since we cannot extract it */
        cJSON null_id;
        memset(&null_id, 0, sizeof(null_id));
        null_id.type = cJSON_NULL;
        cd_mcp_send_error(server, &null_id, CD_JSONRPC_PARSE_ERROR,
                          "Parse error");
        return CD_ERR_PARSE;
    }

    /* Step 2: Extract the "id" field.
     * May be a number, string, or absent (notification).
     * cJSON_GetObjectItemCaseSensitive returns NULL if not found. */
    cJSON* id_item = cJSON_GetObjectItemCaseSensitive(request, "id");

    /* Step 3: Validate JSON-RPC 2.0 structure */
    cJSON* jsonrpc = cJSON_GetObjectItemCaseSensitive(request, "jsonrpc");
    if (jsonrpc == NULL || !cJSON_IsString(jsonrpc) ||
        strcmp(jsonrpc->valuestring, "2.0") != 0) {
        cd_mcp_send_error(server, id_item, CD_JSONRPC_INVALID_REQUEST,
                          "Invalid request: missing or wrong jsonrpc version");
        cJSON_Delete(request);
        return CD_ERR_PARSE;
    }

    cJSON* method_item = cJSON_GetObjectItemCaseSensitive(request, "method");
    if (method_item == NULL || !cJSON_IsString(method_item)) {
        cd_mcp_send_error(server, id_item, CD_JSONRPC_INVALID_REQUEST,
                          "Invalid request: missing method field");
        cJSON_Delete(request);
        return CD_ERR_PARSE;
    }

    const char* method = method_item->valuestring;

    /* ================================================================
     * Standard MCP protocol methods
     * ================================================================ */

    /* initialize — MCP handshake */
    if (strcmp(method, "initialize") == 0) {
        cJSON* result = cJSON_CreateObject();
        if (result) {
            cJSON_AddStringToObject(result, "protocolVersion",
                                    CD_MCP_PROTOCOL_VERSION);
            cJSON* capabilities = cJSON_CreateObject();
            if (capabilities) {
                cJSON_AddItemToObject(capabilities, "tools",
                                      cJSON_CreateObject());
                cJSON_AddItemToObject(capabilities, "resources",
                                      cJSON_CreateObject());
                cJSON_AddItemToObject(capabilities, "prompts",
                                      cJSON_CreateObject());
                cJSON_AddItemToObject(result, "capabilities", capabilities);
            }
            cJSON* server_info = cJSON_CreateObject();
            if (server_info) {
                cJSON_AddStringToObject(server_info, "name", "cadence-engine");
                cJSON_AddStringToObject(server_info, "version", "0.1.0");
                cJSON_AddItemToObject(result, "serverInfo", server_info);
            }
            server->handshake_done = true;
            cd_mcp_send_result(server, id_item, result);
            cJSON_Delete(result);
        }
        cJSON_Delete(request);
        return CD_OK;
    }

    /* notifications/initialized (and bare "initialized") — client ack */
    if (strcmp(method, "notifications/initialized") == 0 ||
        strcmp(method, "initialized") == 0) {
        /* Notification: id_item is NULL so cd_mcp_send_result is a no-op */
        cJSON_Delete(request);
        return CD_OK;
    }

    /* ping — health check */
    if (strcmp(method, "ping") == 0) {
        cJSON* result = cJSON_CreateObject();
        if (result) {
            cd_mcp_send_result(server, id_item, result);
            cJSON_Delete(result);
        }
        cJSON_Delete(request);
        return CD_OK;
    }

    /* tools/call — standard MCP tool invocation */
    if (strcmp(method, "tools/call") == 0) {
        cJSON* tc_params = cJSON_GetObjectItemCaseSensitive(request, "params");
        cJSON* name_item = tc_params
            ? cJSON_GetObjectItemCaseSensitive(tc_params, "name") : NULL;

        if (!name_item || !cJSON_IsString(name_item)) {
            cd_mcp_send_error(server, id_item, CD_JSONRPC_INVALID_PARAMS,
                              "tools/call requires params.name (string)");
            cJSON_Delete(request);
            return CD_OK;
        }

        const char* tool_name = name_item->valuestring;
        cJSON* arguments = tc_params
            ? cJSON_GetObjectItemCaseSensitive(tc_params, "arguments") : NULL;

        /* Try exact match first, then convert double-underscores→dots
         * for standard MCP clients that received names from tools/list */
        cd_mcp_tool_entry_t* tool = cd_mcp_find_tool(server, tool_name);
        char dotted_name[128];
        if (!tool) {
            cd_mcp_name_from_mcp(tool_name, dotted_name, sizeof(dotted_name));
            tool = cd_mcp_find_tool(server, dotted_name);
        }
        if (!tool) {
            /* Build fuzzy-match suggestion */
            const char* method_names[256];
            uint32_t method_count = 0;
            for (uint32_t i = 0; i < server->tool_count && i < 256; i++) {
                method_names[method_count++] = server->tools[i].method;
            }
            const char* suggestion = cd_fuzzy_best_match(
                tool_name, method_names, method_count);

            char detail_buf[256];
            snprintf(detail_buf, sizeof(detail_buf),
                     "No tool registered with name '%s'", tool_name);
            char suggest_buf[256];
            if (suggestion) {
                snprintf(suggest_buf, sizeof(suggest_buf),
                         "Did you mean '%s'?", suggestion);
            } else {
                snprintf(suggest_buf, sizeof(suggest_buf),
                         "Use tools/list to list available tools.");
            }
            cd_mcp_send_error(server, id_item, CD_JSONRPC_METHOD_NOT_FOUND,
                              cd_mcp_error_fmt("Method not found",
                                                detail_buf, suggest_buf));
            cJSON_Delete(request);
            return CD_OK;
        }

        /* P2-4: Async dispatch for long-running tools */
        if (tool->flags & CD_MCP_TOOL_ASYNC) {
            uint64_t req_id = id_item ? (uint64_t)id_item->valuedouble : 0;
            uint32_t agent = cd_mcp_get_current_agent();
            /* Heap-allocate trampoline context (freed by async thread) */
            cd_async_trampoline_t* tramp = (cd_async_trampoline_t*)cd_mem_alloc_tagged(
                sizeof(cd_async_trampoline_t), CD_MEM_KERNEL);
            if (tramp) {
                tramp->handler = tool->handler;
                cd_result_t ar = cd_mcp_async_dispatch(&server->async_pool, kernel,
                    cd_mcp_async_trampoline, arguments, req_id, agent, tramp);
                if (ar == CD_OK) {
                    /* Response will be sent when task completes -- don't send now */
                    cJSON_Delete(request);
                    return CD_OK;
                }
                cd_mem_free_tagged(tramp);
            }
            /* Fallback to synchronous if pool is full or alloc failed */
        }

        int         tc_error_code = 0;
        const char* tc_error_msg  = NULL;
        cJSON*      raw_result    = tool->handler(kernel, arguments,
                                                   &tc_error_code,
                                                   &tc_error_msg);

        /* Wrap into standard tools/call response format */
        cJSON* wrapped = cJSON_CreateObject();
        cJSON* content_arr = cJSON_CreateArray();
        if (wrapped && content_arr) {
            cJSON* text_item = cJSON_CreateObject();
            if (text_item) {
                cJSON_AddStringToObject(text_item, "type", "text");
                if (raw_result) {
                    char* serialized = cJSON_PrintUnformatted(raw_result);
                    if (serialized) {
                        cJSON_AddStringToObject(text_item, "text", serialized);
                        cJSON_free(serialized);
                    }
                } else {
                    /* Handler error */
                    const char* emsg = tc_error_msg ? tc_error_msg
                                                    : "Internal error";
                    cJSON_AddStringToObject(text_item, "text", emsg);
                }
                cJSON_AddItemToArray(content_arr, text_item);
            }
            cJSON_AddItemToObject(wrapped, "content", content_arr);
            if (!raw_result) {
                cJSON_AddBoolToObject(wrapped, "isError", 1);
            }
            cd_mcp_send_result(server, id_item, wrapped);
        } else {
            if (content_arr) cJSON_Delete(content_arr);
        }
        if (wrapped) cJSON_Delete(wrapped);
        if (raw_result) cJSON_Delete(raw_result);
        cJSON_Delete(request);
        return CD_OK;
    }

    /* Handle MCP protocol methods (not regular tools) */
    if (strcmp(method, "tools/list") == 0) {
        cJSON* result = cJSON_CreateObject();
        cJSON* tools_array = cJSON_CreateArray();
        if (result && tools_array) {
            for (uint32_t i = 0; i < server->tool_count; i++) {
                cd_mcp_tool_entry_t* t = &server->tools[i];
                cJSON* tool_obj = cJSON_CreateObject();
                if (!tool_obj) continue;
                /* MCP spec: tool names must be [a-zA-Z0-9_-]{1,64} */
                char mcp_name[128];
                cd_mcp_name_to_mcp(t->method, mcp_name, sizeof(mcp_name));
                cJSON_AddStringToObject(tool_obj, "name", mcp_name);
                if (t->description) {
                    cJSON_AddStringToObject(tool_obj, "description", t->description);
                }
                if (t->input_schema) {
                    cJSON* schema = cJSON_Parse(t->input_schema);
                    if (schema) {
                        cJSON_AddItemToObject(tool_obj, "inputSchema", schema);
                    }
                }
                cJSON_AddItemToArray(tools_array, tool_obj);
            }
            cJSON_AddItemToObject(result, "tools", tools_array);
            cd_mcp_send_result(server, id_item, result);
            cJSON_Delete(result);
        } else {
            if (result) cJSON_Delete(result);
            if (tools_array) cJSON_Delete(tools_array);
            cd_mcp_send_error(server, id_item, CD_JSONRPC_INTERNAL_ERROR,
                              "Failed to build tools/list response");
        }
        cJSON_Delete(request);
        return CD_OK;
    }

    /* ================================================================
     * resources/list — enumerate project assets as MCP resources
     * ================================================================ */
    if (strcmp(method, "resources/list") == 0) {
        cJSON* result = cJSON_CreateObject();
        cJSON* resources_array = cJSON_CreateArray();
        if (result && resources_array) {
            if (kernel && cd_kernel_get_asset_db(kernel) && cd_kernel_get_asset_db(kernel)->count > 0) {
                for (uint32_t i = 0; i < cd_kernel_get_asset_db(kernel)->count; i++) {
                    const cd_asset_entry_t* entry = &cd_kernel_get_asset_db(kernel)->entries[i];
                    cJSON* res_obj = cJSON_CreateObject();
                    if (!res_obj) continue;

                    cJSON_AddStringToObject(res_obj, "uri", entry->uri);

                    /* Extract filename from URI for the name field */
                    const char* name = entry->uri;
                    const char* slash = strrchr(entry->uri, '/');
                    if (slash) name = slash + 1;
                    cJSON_AddStringToObject(res_obj, "name", name);

                    /* Map asset kind to MIME type */
                    const char* mime = "application/octet-stream";
                    switch (entry->kind) {
                        case CD_ASSET_SCRIPT:  mime = "text/x-lua";        break;
                        case CD_ASSET_SCENE:   mime = "application/toml";  break;
                        case CD_ASSET_MESH:    mime = "model/gltf-binary"; break;
                        case CD_ASSET_TEXTURE: mime = "image/png";         break;
                        case CD_ASSET_AUDIO:   mime = "audio/wav";         break;
                        default:               break;
                    }
                    cJSON_AddStringToObject(res_obj, "mimeType", mime);

                    cJSON_AddItemToArray(resources_array, res_obj);
                }
            }
            cJSON_AddItemToObject(result, "resources", resources_array);
            cd_mcp_send_result(server, id_item, result);
            cJSON_Delete(result);
        } else {
            if (result) cJSON_Delete(result);
            if (resources_array) cJSON_Delete(resources_array);
            cd_mcp_send_error(server, id_item, CD_JSONRPC_INTERNAL_ERROR,
                              "Failed to build resources/list response");
        }
        cJSON_Delete(request);
        return CD_OK;
    }

    /* ================================================================
     * resources/read — return file contents for a resource URI
     * ================================================================ */
    if (strcmp(method, "resources/read") == 0) {
        cJSON* rr_params = cJSON_GetObjectItemCaseSensitive(request, "params");
        cJSON* uri_item = rr_params
            ? cJSON_GetObjectItemCaseSensitive(rr_params, "uri") : NULL;

        if (!uri_item || !cJSON_IsString(uri_item)) {
            cd_mcp_send_error(server, id_item, CD_JSONRPC_INVALID_PARAMS,
                              "resources/read requires params.uri (string)");
            cJSON_Delete(request);
            return CD_OK;
        }

        const char* uri = uri_item->valuestring;

        /* Look up the asset in the database */
        if (!kernel || !cd_kernel_get_asset_db(kernel)) {
            cd_mcp_send_error(server, id_item, CD_JSONRPC_INTERNAL_ERROR,
                              "No asset database available");
            cJSON_Delete(request);
            return CD_OK;
        }

        const cd_asset_entry_t* asset = cd_asset_db_lookup(cd_kernel_get_asset_db(kernel), uri);
        if (!asset) {
            cd_mcp_send_error(server, id_item, CD_JSONRPC_INVALID_PARAMS,
                              cd_mcp_error_fmt("Resource not found",
                                                uri,
                                                "Use resources/list to see available resources."));
            cJSON_Delete(request);
            return CD_OK;
        }

        /* Read the file contents */
        void* file_data = NULL;
        uint32_t file_size = 0;
        cd_result_t read_res = cd_fs_read_file(asset->abs_path,
                                                &file_data, &file_size);
        if (read_res != CD_OK || !file_data) {
            cd_mcp_send_error(server, id_item, CD_JSONRPC_INTERNAL_ERROR,
                              cd_mcp_error_fmt("Failed to read resource",
                                                asset->abs_path, NULL));
            cJSON_Delete(request);
            return CD_OK;
        }

        /* Build the response */
        cJSON* result = cJSON_CreateObject();
        cJSON* contents_array = cJSON_CreateArray();
        if (result && contents_array) {
            cJSON* content_obj = cJSON_CreateObject();
            if (content_obj) {
                cJSON_AddStringToObject(content_obj, "uri", uri);

                /* Text assets (scripts, scenes) get text; binary get base64 */
                bool is_text = (asset->kind == CD_ASSET_SCRIPT ||
                                asset->kind == CD_ASSET_SCENE);
                if (is_text) {
                    /* Null-terminate the text data for cJSON */
                    char* text = (char*)calloc(1, file_size + 1);
                    if (text) {
                        memcpy(text, file_data, file_size);
                        text[file_size] = '\0';
                        cJSON_AddStringToObject(content_obj, "text", text);
                        free(text);
                    }
                } else {
                    /* Base64 encode binary data */
                    static const char b64[] =
                        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                    uint32_t b64_len = ((file_size + 2) / 3) * 4;
                    char* b64_buf = (char*)calloc(1, b64_len + 1);
                    if (b64_buf) {
                        const unsigned char* src = (const unsigned char*)file_data;
                        uint32_t j = 0;
                        for (uint32_t i = 0; i < file_size; i += 3) {
                            uint32_t a = src[i];
                            uint32_t b_val = (i + 1 < file_size) ? src[i + 1] : 0;
                            uint32_t c = (i + 2 < file_size) ? src[i + 2] : 0;
                            uint32_t triple = (a << 16) | (b_val << 8) | c;
                            b64_buf[j++] = b64[(triple >> 18) & 0x3F];
                            b64_buf[j++] = b64[(triple >> 12) & 0x3F];
                            b64_buf[j++] = (i + 1 < file_size)
                                ? b64[(triple >> 6) & 0x3F] : '=';
                            b64_buf[j++] = (i + 2 < file_size)
                                ? b64[triple & 0x3F] : '=';
                        }
                        b64_buf[j] = '\0';
                        cJSON_AddStringToObject(content_obj, "blob", b64_buf);
                        free(b64_buf);
                    }
                }

                /* Map asset kind to MIME type */
                const char* mime = "application/octet-stream";
                switch (asset->kind) {
                    case CD_ASSET_SCRIPT:  mime = "text/x-lua";        break;
                    case CD_ASSET_SCENE:   mime = "application/toml";  break;
                    case CD_ASSET_MESH:    mime = "model/gltf-binary"; break;
                    case CD_ASSET_TEXTURE: mime = "image/png";         break;
                    case CD_ASSET_AUDIO:   mime = "audio/wav";         break;
                    default:               break;
                }
                cJSON_AddStringToObject(content_obj, "mimeType", mime);

                cJSON_AddItemToArray(contents_array, content_obj);
            }
            cJSON_AddItemToObject(result, "contents", contents_array);
            cd_mcp_send_result(server, id_item, result);
            cJSON_Delete(result);
        } else {
            if (result) cJSON_Delete(result);
            if (contents_array) cJSON_Delete(contents_array);
            cd_mcp_send_error(server, id_item, CD_JSONRPC_INTERNAL_ERROR,
                              "Failed to build resources/read response");
        }

        cd_mem_free_tagged(file_data);
        cJSON_Delete(request);
        return CD_OK;
    }

    /* ================================================================
     * prompts/list — return available workflow prompts
     * ================================================================ */
    if (strcmp(method, "prompts/list") == 0) {
        cJSON* result = cJSON_CreateObject();
        cJSON* prompts_array = cJSON_CreateArray();
        if (result && prompts_array) {
            /* create_platformer prompt */
            cJSON* p1 = cJSON_CreateObject();
            if (p1) {
                cJSON_AddStringToObject(p1, "name", "create_platformer");
                cJSON_AddStringToObject(p1, "description",
                    "Create a platformer game from scratch");
                cJSON_AddItemToArray(prompts_array, p1);
            }

            /* create_fps prompt */
            cJSON* p2 = cJSON_CreateObject();
            if (p2) {
                cJSON_AddStringToObject(p2, "name", "create_fps");
                cJSON_AddStringToObject(p2, "description",
                    "Create a first-person shooter game with WASD controls");
                cJSON_AddItemToArray(prompts_array, p2);
            }

            /* debug_scene prompt */
            cJSON* p3 = cJSON_CreateObject();
            if (p3) {
                cJSON_AddStringToObject(p3, "name", "debug_scene");
                cJSON_AddStringToObject(p3, "description",
                    "Inspect and debug the current scene graph");
                cJSON_AddItemToArray(prompts_array, p3);
            }

            cJSON_AddItemToObject(result, "prompts", prompts_array);
            cd_mcp_send_result(server, id_item, result);
            cJSON_Delete(result);
        } else {
            if (result) cJSON_Delete(result);
            if (prompts_array) cJSON_Delete(prompts_array);
            cd_mcp_send_error(server, id_item, CD_JSONRPC_INTERNAL_ERROR,
                              "Failed to build prompts/list response");
        }
        cJSON_Delete(request);
        return CD_OK;
    }

    /* Step 4: Extract params (optional) */
    cJSON* params = cJSON_GetObjectItemCaseSensitive(request, "params");

    /* Step 5: Look up handler in tool registry */
    cd_mcp_tool_entry_t* tool = cd_mcp_find_tool(server, method);
    if (tool == NULL) {
        /* Build an actionable error with fuzzy match suggestion */
        const char* method_names[256];
        uint32_t method_count = 0;
        for (uint32_t i = 0; i < server->tool_count && i < 256; i++) {
            method_names[method_count++] = server->tools[i].method;
        }
        const char* suggestion = cd_fuzzy_best_match(method, method_names, method_count);

        char detail_buf[256];
        snprintf(detail_buf, sizeof(detail_buf),
                 "No tool registered with method '%s'", method);

        char suggest_buf[256];
        if (suggestion != NULL) {
            snprintf(suggest_buf, sizeof(suggest_buf),
                     "Did you mean '%s'?", suggestion);
        } else {
            snprintf(suggest_buf, sizeof(suggest_buf),
                     "Use system.capabilities to list available tools.");
        }

        cd_mcp_send_error(server, id_item, CD_JSONRPC_METHOD_NOT_FOUND,
                          cd_mcp_error_fmt("Method not found",
                                            detail_buf, suggest_buf));
        cJSON_Delete(request);
        return CD_OK; /* Not a fatal error for the server */
    }

    /* P2-4: Async dispatch for long-running tools (legacy method path) */
    if (tool->flags & CD_MCP_TOOL_ASYNC) {
        uint64_t req_id = id_item ? (uint64_t)id_item->valuedouble : 0;
        uint32_t agent = cd_mcp_get_current_agent();
        cd_async_trampoline_t* tramp = (cd_async_trampoline_t*)cd_mem_alloc_tagged(
            sizeof(cd_async_trampoline_t), CD_MEM_KERNEL);
        if (tramp) {
            tramp->handler = tool->handler;
            cd_result_t ar = cd_mcp_async_dispatch(&server->async_pool, kernel,
                cd_mcp_async_trampoline, params, req_id, agent, tramp);
            if (ar == CD_OK) {
                cJSON_Delete(request);
                return CD_OK;
            }
            cd_mem_free_tagged(tramp);
        }
        /* Fallback to synchronous if pool is full or alloc failed */
    }

    /* Step 6: Call the handler */
    int         error_code = 0;
    const char* error_msg  = NULL;
    cJSON*      result     = tool->handler(kernel, params, &error_code,
                                           &error_msg);

    /* Step 7: Send response */
    if (result != NULL) {
        cd_mcp_send_result(server, id_item, result);
        cJSON_Delete(result);
    } else {
        /* Handler signaled an error */
        if (error_msg == NULL) {
            error_msg = "Internal error";
        }
        if (error_code == 0) {
            error_code = CD_JSONRPC_INTERNAL_ERROR;
        }
        cd_mcp_send_error(server, id_item, error_code, error_msg);
    }

    cJSON_Delete(request);
    return CD_OK;
}

cd_result_t cd_mcp_server_poll(cd_mcp_server_t* server,
                                struct cd_kernel_t* kernel) {
    if (server == NULL) {
        return CD_ERR_NULL;
    }

    if (!server->initialized) {
        return CD_ERR_INVALID;
    }

    /* Read available bytes from stdin (non-blocking) */
    uint32_t space = server->read_capacity - server->read_pos - 1;
    if (space == 0) {
        /* Buffer full but no newline found -- grow the buffer */
        uint32_t new_cap = server->read_capacity * 2;
        char* new_buf = (char*)realloc(server->read_buffer, new_cap);
        if (new_buf == NULL) {
            return CD_ERR_ALLOC;
        }
        server->read_buffer   = new_buf;
        server->read_capacity = new_cap;
        space = new_cap - server->read_pos - 1;
    }

    int bytes_read = cd_mcp_read_stdin_nonblocking(
        server->read_buffer + server->read_pos,
        (int)space);

    if (bytes_read <= 0) {
        return CD_OK; /* No data available or error -- not fatal */
    }

    server->read_pos += (uint32_t)bytes_read;
    server->read_buffer[server->read_pos] = '\0';

    /* Process all complete lines (newline-delimited) */
    char* line_start = server->read_buffer;
    char* newline;

    while ((newline = strchr(line_start, '\n')) != NULL) {
        /* Null-terminate the line (replacing the newline) */
        *newline = '\0';

        /* Handle optional \r before \n (Windows line endings) */
        if (newline > line_start && *(newline - 1) == '\r') {
            *(newline - 1) = '\0';
        }

        /* Skip empty lines */
        if (line_start[0] != '\0') {
            cd_mcp_process_request(server, kernel, line_start);
        }

        line_start = newline + 1;
    }

    /* Move any remaining partial data to the beginning of the buffer */
    uint32_t remaining = (uint32_t)(server->read_buffer + server->read_pos
                                    - line_start);
    if (remaining > 0 && line_start != server->read_buffer) {
        memmove(server->read_buffer, line_start, remaining);
    }
    server->read_pos = remaining;
    server->read_buffer[server->read_pos] = '\0';

    return CD_OK;
}
