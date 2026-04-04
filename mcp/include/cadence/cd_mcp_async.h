/* cd_mcp_async.h - Cadence Engine async MCP tool dispatch (P2-4)
 *
 * Provides a background-thread task pool for long-running MCP tool
 * handlers (e.g., asset cooking, lightmap baking, PAK export).
 *
 * Long-running handlers are tagged with CD_MCP_TOOL_ASYNC.  When the
 * server dispatches such a handler, it spawns a background thread and
 * returns immediately.  The response is sent when the task completes,
 * polled once per frame via cd_mcp_async_poll().
 */
#ifndef CD_MCP_ASYNC_H
#define CD_MCP_ASYNC_H

#include <stdint.h>
#include <stdbool.h>
#include "cadence/cd_core_types.h"
#include "cadence/cd_platform.h"

/* Forward declarations */
struct cd_kernel_t;
struct cd_mcp_server_t;
typedef struct cJSON cJSON;

/* ============================================================================
 * Constants
 * ============================================================================ */

/** Maximum concurrent async tasks. */
#define CD_MCP_ASYNC_MAX_TASKS 16

/** Tool flag indicating the handler may run asynchronously. */
#define CD_MCP_TOOL_ASYNC (1u << 0)

/* ============================================================================
 * Async task
 * ============================================================================ */

typedef enum {
    CD_MCP_TASK_IDLE    = 0,
    CD_MCP_TASK_RUNNING = 1,
    CD_MCP_TASK_DONE    = 2,
    CD_MCP_TASK_FAILED  = 3,
} cd_mcp_task_status_t;

/** Function signature for async tool work (runs on background thread). */
typedef cJSON* (*cd_mcp_async_work_fn)(struct cd_kernel_t* kernel,
                                       const cJSON* params,
                                       int* error_code,
                                       const char** error_msg,
                                       void* user_data);

typedef struct {
    /* Request identity */
    uint64_t              request_id;   /**< JSON-RPC request ID for response routing. */
    uint32_t              agent_id;     /**< MCP agent/connection that sent the request. */

    /* Work function and params */
    cd_mcp_async_work_fn  work_fn;
    cJSON*                params;       /**< Deep copy of request params (owned). */
    void*                 user_data;

    /* Result (set by background thread) */
    cJSON*                result;       /**< Result JSON (NULL on error). */
    int                   error_code;
    const char*           error_msg;

    /* Status (read by main thread, written by bg thread) */
    volatile cd_mcp_task_status_t status;

    /* Background thread handle */
    cd_thread_t           thread;
} cd_mcp_async_task_t;

/* ============================================================================
 * Async task pool
 * ============================================================================ */

typedef struct {
    cd_mcp_async_task_t tasks[CD_MCP_ASYNC_MAX_TASKS];
    uint32_t            active_count;
} cd_mcp_async_pool_t;

/**
 * Initialize the async task pool (zero all slots).
 */
void cd_mcp_async_init(cd_mcp_async_pool_t* pool);

/**
 * Dispatch an async task on a background thread.
 *
 * @param pool       The task pool.
 * @param kernel     Engine kernel (passed to work_fn).
 * @param work_fn    The function to run on the background thread.
 * @param params     JSON params (deep-copied; caller retains ownership).
 * @param request_id JSON-RPC request ID for response routing.
 * @param agent_id   MCP agent/connection ID.
 * @param user_data  Opaque pointer passed to work_fn.
 * @return CD_OK on success, CD_ERR_FULL if all slots are occupied.
 */
cd_result_t cd_mcp_async_dispatch(cd_mcp_async_pool_t* pool,
                                  struct cd_kernel_t* kernel,
                                  cd_mcp_async_work_fn work_fn,
                                  const cJSON* params,
                                  uint64_t request_id,
                                  uint32_t agent_id,
                                  void* user_data);

/**
 * Poll for completed async tasks and send their responses.
 *
 * Call once per frame from the main thread (in pre_tick).
 * For each completed task, sends the JSON-RPC response via the server
 * and frees the task slot.
 *
 * @param pool   The task pool.
 * @param server The MCP server (for sending responses).
 */
void cd_mcp_async_poll(cd_mcp_async_pool_t* pool,
                       struct cd_mcp_server_t* server);

/**
 * Shut down the pool: wait for all running tasks to finish.
 */
void cd_mcp_async_shutdown(cd_mcp_async_pool_t* pool);

#endif /* CD_MCP_ASYNC_H */
