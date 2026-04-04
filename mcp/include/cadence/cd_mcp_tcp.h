/* cd_mcp_tcp.h - Cadence Engine MCP TCP transport
 *
 * Task 9.1: TCP transport for the MCP server.
 *
 * Allows remote agents to connect via TCP and send JSON-RPC 2.0 requests.
 * Non-blocking I/O using select() — cd_mcp_tcp_poll() is called each frame
 * from the main loop and never blocks.
 *
 * Protocol: line-delimited JSON-RPC (same as stdio transport).
 */
#ifndef CD_MCP_TCP_H
#define CD_MCP_TCP_H

#include <stdint.h>
#include <stdbool.h>
#include "cadence/cd_core_types.h"

/* Forward declarations */
struct cd_kernel_t;

#include "cadence/cd_mcp.h"

/* ============================================================================
 * Constants
 * ============================================================================ */

/** Maximum simultaneous TCP clients. */
#define CD_MCP_TCP_MAX_CLIENTS 16

/** Per-client read buffer size in bytes. */
#define CD_MCP_TCP_CLIENT_BUF_SIZE 8192

/* ============================================================================
 * Platform socket type abstraction
 * ============================================================================ */

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET cd_socket_t;
#define CD_INVALID_SOCKET INVALID_SOCKET
#else
typedef int cd_socket_t;
#define CD_INVALID_SOCKET (-1)
#endif

/* ============================================================================
 * Client connection
 * ============================================================================ */

typedef struct {
    cd_socket_t sock;                             /* Client socket */
    char        read_buf[CD_MCP_TCP_CLIENT_BUF_SIZE]; /* Accumulation buffer */
    uint32_t    read_pos;                         /* Bytes in read_buf */
    bool        active;                           /* Slot in use */
    bool        authenticated;                    /* Auth handshake complete */
} cd_mcp_tcp_client_t;

/* ============================================================================
 * TCP server
 * ============================================================================ */

typedef struct cd_mcp_tcp_server_t {
    cd_socket_t          listen_sock;                      /* Listening socket */
    cd_mcp_tcp_client_t  clients[CD_MCP_TCP_MAX_CLIENTS];  /* Client slots */
    uint16_t             port;                             /* Bound port */
    bool                 initialized;                      /* Server ready */
    const char*          auth_token;                       /* Required token, NULL = no auth */
} cd_mcp_tcp_server_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Initialize and start listening on the given port.
 *
 * Binds to 0.0.0.0:port. Pass port 0 for OS-assigned port (useful for tests).
 * The actual bound port is stored in tcp->port after success.
 *
 * @param tcp   Pointer to uninitialized TCP server structure.
 * @param port  TCP port to listen on (0 = OS-assigned).
 * @return CD_OK on success, CD_ERR_NULL if tcp is NULL,
 *         CD_ERR_IO on socket/bind/listen failure.
 */
cd_result_t cd_mcp_tcp_init(cd_mcp_tcp_server_t* tcp, uint16_t port);

/**
 * Poll for new connections and incoming data (non-blocking).
 *
 * Call once per frame. Accepts pending connections, reads available data
 * from each client, and dispatches complete JSON-RPC lines via
 * cd_mcp_process_request(). Responses are sent back to the originating
 * client socket.
 *
 * @param tcp     Pointer to initialized TCP server.
 * @param mcp     Pointer to the MCP server (tool registry + dispatch).
 * @param kernel  Pointer to the engine kernel.
 * @return CD_OK on success, CD_ERR_NULL if any argument is NULL.
 */
cd_result_t cd_mcp_tcp_poll(cd_mcp_tcp_server_t* tcp,
                             cd_mcp_server_t* mcp,
                             struct cd_kernel_t* kernel);

/**
 * Shut down the TCP server. Closes all client connections and the
 * listening socket. Safe to call on an uninitialized or already-shut-down
 * server.
 *
 * @param tcp  Pointer to TCP server structure.
 */
void cd_mcp_tcp_shutdown(cd_mcp_tcp_server_t* tcp);

/**
 * Set the authentication token required for TCP clients.
 *
 * When a non-NULL token is set, each client must send {"auth":"<token>"}
 * as its first line. The server responds with {"auth":"ok"} on success
 * or {"auth":"denied"} followed by disconnect on failure.
 *
 * If token is NULL, authentication is disabled (all clients are accepted).
 * The token string must outlive the server.
 *
 * @param tcp    Pointer to TCP server structure.
 * @param token  Auth token string, or NULL to disable authentication.
 */
void cd_mcp_tcp_set_auth_token(cd_mcp_tcp_server_t* tcp, const char* token);

/**
 * Return the number of currently connected clients.
 *
 * @param tcp  Pointer to TCP server structure.
 * @return Number of active client connections, or 0 if tcp is NULL.
 */
uint32_t cd_mcp_tcp_client_count(const cd_mcp_tcp_server_t* tcp);

#endif /* CD_MCP_TCP_H */
