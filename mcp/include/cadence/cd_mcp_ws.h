/* cd_mcp_ws.h - Cadence Engine MCP WebSocket transport
 *
 * Task 19.1: WebSocket transport for the MCP server.
 *
 * Allows browser-based agents to connect via WebSocket (RFC 6455) and send
 * JSON-RPC 2.0 requests. Non-blocking I/O using select() — cd_mcp_ws_poll()
 * is called each frame from the main loop and never blocks.
 *
 * Protocol: each WebSocket text message is one JSON-RPC request/response.
 * The server performs the HTTP upgrade handshake automatically.
 */
#ifndef CD_MCP_WS_H
#define CD_MCP_WS_H

#include <stdint.h>
#include <stdbool.h>
#include "cadence/cd_core_types.h"

/* Forward declarations */
struct cd_kernel_t;

#include "cadence/cd_mcp.h"

/* ============================================================================
 * Constants
 * ============================================================================ */

/** Maximum simultaneous WebSocket clients. */
#define CD_MCP_WS_MAX_CLIENTS 16

/** Per-client read buffer size in bytes. */
#define CD_MCP_WS_CLIENT_BUF_SIZE 16384

/** Maximum single WebSocket frame payload (64 KB). */
#define CD_MCP_WS_MAX_FRAME_SIZE 65536

/* ============================================================================
 * Platform socket type abstraction (same as TCP)
 * ============================================================================ */

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET cd_ws_socket_t;
#define CD_WS_INVALID_SOCKET INVALID_SOCKET
#else
typedef int cd_ws_socket_t;
#define CD_WS_INVALID_SOCKET (-1)
#endif

/* ============================================================================
 * WebSocket client state
 * ============================================================================ */

typedef enum {
    CD_WS_STATE_HTTP_HANDSHAKE, /* Waiting for HTTP upgrade request */
    CD_WS_STATE_OPEN,           /* WebSocket connection established */
    CD_WS_STATE_CLOSING         /* Close frame sent, waiting for response */
} cd_ws_client_state_t;

typedef struct {
    cd_ws_socket_t       sock;                              /* Client socket */
    char                 read_buf[CD_MCP_WS_CLIENT_BUF_SIZE]; /* Accumulation buffer */
    uint32_t             read_pos;                          /* Bytes in read_buf */
    bool                 active;                            /* Slot in use */
    cd_ws_client_state_t state;                             /* Connection state */
} cd_mcp_ws_client_t;

/* ============================================================================
 * WebSocket server
 * ============================================================================ */

typedef struct cd_mcp_ws_server_t {
    cd_ws_socket_t       listen_sock;                       /* Listening socket */
    cd_mcp_ws_client_t   clients[CD_MCP_WS_MAX_CLIENTS];   /* Client slots */
    uint16_t             port;                              /* Bound port */
    bool                 initialized;                       /* Server ready */
    const char*          allowed_origin;                    /* CORS origin, NULL = allow all */
} cd_mcp_ws_server_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Initialize and start listening on the given port.
 *
 * Binds to 0.0.0.0:port. Pass port 0 for OS-assigned port.
 * The actual bound port is stored in ws->port after success.
 *
 * @param ws    Pointer to uninitialized WebSocket server structure.
 * @param port  TCP port to listen on (0 = OS-assigned).
 * @return CD_OK on success, CD_ERR_NULL if ws is NULL,
 *         CD_ERR_IO on socket/bind/listen failure.
 */
cd_result_t cd_mcp_ws_init(cd_mcp_ws_server_t* ws, uint16_t port);

/**
 * Poll for new connections and incoming data (non-blocking).
 *
 * Call once per frame. Accepts pending connections, performs WebSocket
 * handshakes, reads and decodes WebSocket frames, and dispatches
 * complete JSON-RPC messages via cd_mcp_process_request().
 * Responses are sent back as WebSocket text frames.
 *
 * @param ws      Pointer to initialized WebSocket server.
 * @param mcp     Pointer to the MCP server (tool registry + dispatch).
 * @param kernel  Pointer to the engine kernel.
 * @return CD_OK on success, CD_ERR_NULL if any argument is NULL.
 */
cd_result_t cd_mcp_ws_poll(cd_mcp_ws_server_t* ws,
                             cd_mcp_server_t* mcp,
                             struct cd_kernel_t* kernel);

/**
 * Shut down the WebSocket server. Sends close frames to all connected
 * clients, closes all sockets. Safe to call on uninitialized servers.
 *
 * @param ws  Pointer to WebSocket server structure.
 */
void cd_mcp_ws_shutdown(cd_mcp_ws_server_t* ws);

/**
 * Set the allowed Origin header for CORS validation.
 *
 * When a non-NULL origin is set, the server validates the Origin header
 * during the WebSocket handshake and rejects connections from other origins.
 * If origin is NULL, all origins are accepted.
 *
 * The string must outlive the server.
 *
 * @param ws      Pointer to WebSocket server structure.
 * @param origin  Allowed origin string, or NULL to allow all.
 */
void cd_mcp_ws_set_allowed_origin(cd_mcp_ws_server_t* ws, const char* origin);

/**
 * Return the number of currently connected WebSocket clients.
 *
 * @param ws  Pointer to WebSocket server structure.
 * @return Number of active client connections, or 0 if ws is NULL.
 */
uint32_t cd_mcp_ws_client_count(const cd_mcp_ws_server_t* ws);

#endif /* CD_MCP_WS_H */
