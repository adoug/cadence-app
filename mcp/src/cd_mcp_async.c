/* cd_mcp_async.c - Cadence Engine async MCP tool dispatch (P2-4)
 *
 * Background-thread task pool for long-running MCP tool handlers.
 */
#include "cadence/cd_mcp_async.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_agent.h"
#include "cadence/cd_memory.h"
#include "cadence/cd_error.h"
#include "cadence/cd_platform.h"

#include <string.h>

/* We need cJSON for deep-copying params */
#include "cJSON.h"

/* ============================================================================
 * Background thread entry point
 * ============================================================================ */

typedef struct {
    cd_mcp_async_task_t* task;
    struct cd_kernel_t*  kernel;
} cd_async_thread_ctx_t;

static void async_thread_entry(void* arg) {
    cd_async_thread_ctx_t* ctx = (cd_async_thread_ctx_t*)arg;
    cd_mcp_async_task_t* task = ctx->task;
    struct cd_kernel_t* kernel = ctx->kernel;

    /* Run the work function */
    task->result = task->work_fn(kernel, task->params,
                                 &task->error_code,
                                 &task->error_msg,
                                 task->user_data);

    /* Mark as done (volatile write, visible to main thread) */
    task->status = task->result ? CD_MCP_TASK_DONE : CD_MCP_TASK_FAILED;

    /* Free the thread context */
    cd_mem_free_tagged(ctx);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void cd_mcp_async_init(cd_mcp_async_pool_t* pool) {
    if (!pool) return;
    memset(pool, 0, sizeof(*pool));
}

cd_result_t cd_mcp_async_dispatch(cd_mcp_async_pool_t* pool,
                                  struct cd_kernel_t* kernel,
                                  cd_mcp_async_work_fn work_fn,
                                  const cJSON* params,
                                  uint64_t request_id,
                                  uint32_t agent_id,
                                  void* user_data) {
    if (!pool || !work_fn) {
        CD_SET_ERROR(CD_ERR_NULL, "NULL pool or work function");
        return CD_ERR_NULL;
    }

    /* Find a free slot */
    cd_mcp_async_task_t* slot = NULL;
    for (uint32_t i = 0; i < CD_MCP_ASYNC_MAX_TASKS; i++) {
        if (pool->tasks[i].status == CD_MCP_TASK_IDLE) {
            slot = &pool->tasks[i];
            break;
        }
    }

    if (!slot) {
        CD_SET_ERROR(CD_ERR_FULL, "Async task pool full");
        return CD_ERR_FULL;
    }

    /* Initialize the task */
    memset(slot, 0, sizeof(*slot));
    slot->request_id = request_id;
    slot->agent_id   = agent_id;
    slot->work_fn    = work_fn;
    slot->user_data  = user_data;
    slot->status     = CD_MCP_TASK_RUNNING;

    /* Deep-copy params so the caller can free theirs */
    slot->params = params ? cJSON_Duplicate(params, true) : NULL;

    /* Allocate thread context */
    cd_async_thread_ctx_t* ctx = (cd_async_thread_ctx_t*)cd_mem_alloc_tagged(
        sizeof(cd_async_thread_ctx_t), CD_MEM_KERNEL);
    if (!ctx) {
        if (slot->params) cJSON_Delete(slot->params);
        slot->status = CD_MCP_TASK_IDLE;
        CD_SET_ERROR(CD_ERR_ALLOC, "Failed to allocate async thread context");
        return CD_ERR_ALLOC;
    }
    ctx->task   = slot;
    ctx->kernel = kernel;

    /* Spawn background thread */
    slot->thread = cd_thread_create(async_thread_entry, ctx);
    if (!slot->thread) {
        cd_mem_free_tagged(ctx);
        if (slot->params) cJSON_Delete(slot->params);
        slot->status = CD_MCP_TASK_IDLE;
        CD_SET_ERROR(CD_ERR_IO, "Failed to create async thread");
        return CD_ERR_IO;
    }

    pool->active_count++;
    return CD_OK;
}

void cd_mcp_async_poll(cd_mcp_async_pool_t* pool,
                       struct cd_mcp_server_t* server) {
    if (!pool || !server) return;

    for (uint32_t i = 0; i < CD_MCP_ASYNC_MAX_TASKS; i++) {
        cd_mcp_async_task_t* task = &pool->tasks[i];

        if (task->status == CD_MCP_TASK_DONE ||
            task->status == CD_MCP_TASK_FAILED) {

            /* Join the thread to clean up */
            cd_thread_join(task->thread);
            task->thread = NULL;

            /* Send JSON-RPC response back to the requesting client.
             * Set the agent context so multi-client routing works. */
            cd_mcp_set_current_agent(task->agent_id);

            if (task->status == CD_MCP_TASK_DONE && task->result) {
                /* Wrap in tools/call content format (same as synchronous path) */
                cJSON* wrapped = cJSON_CreateObject();
                cJSON* content_arr = cJSON_CreateArray();
                if (wrapped && content_arr) {
                    cJSON* text_item = cJSON_CreateObject();
                    if (text_item) {
                        cJSON_AddStringToObject(text_item, "type", "text");
                        char* text = cJSON_PrintUnformatted(task->result);
                        cJSON_AddStringToObject(text_item, "text", text ? text : "{}");
                        cJSON_free(text);
                        cJSON_AddItemToArray(content_arr, text_item);
                    }
                    cJSON_AddItemToObject(wrapped, "content", content_arr);
                    cd_mcp_server_send_result(server, task->request_id, wrapped);
                    cJSON_Delete(wrapped);
                }
            } else {
                cd_mcp_server_send_error(server, task->request_id,
                    task->error_code ? task->error_code : -32603,
                    task->error_msg ? task->error_msg : "Async task failed");
            }

            /* Free params copy */
            if (task->params) {
                cJSON_Delete(task->params);
                task->params = NULL;
            }

            /* Free result */
            if (task->result) {
                cJSON_Delete(task->result);
                task->result = NULL;
            }

            /* Return slot to pool */
            task->status = CD_MCP_TASK_IDLE;
            if (pool->active_count > 0) pool->active_count--;
        }
    }
}

void cd_mcp_async_shutdown(cd_mcp_async_pool_t* pool) {
    if (!pool) return;

    /* Wait for all running tasks */
    for (uint32_t i = 0; i < CD_MCP_ASYNC_MAX_TASKS; i++) {
        if (pool->tasks[i].status == CD_MCP_TASK_RUNNING) {
            cd_thread_join(pool->tasks[i].thread);
        }
        if (pool->tasks[i].params) {
            cJSON_Delete(pool->tasks[i].params);
            pool->tasks[i].params = NULL;
        }
        if (pool->tasks[i].result) {
            cJSON_Delete(pool->tasks[i].result);
            pool->tasks[i].result = NULL;
        }
        pool->tasks[i].status = CD_MCP_TASK_IDLE;
    }
    pool->active_count = 0;
}
