/* cd_mcp_socket.h - Cadence Engine MCP local IPC transport
 *
 * Task S2.3: Named Pipe (Windows) / Unix domain socket (POSIX) transport
 * for the MCP server.
 *
 * Allows local agents to connect via fast IPC and send JSON-RPC 2.0
 * requests. Non-blocking I/O -- cd_mcp_socket_poll() is called each
 * frame from the main loop and never blocks.
 *
 * Protocol: line-delimited JSON-RPC (same as stdio and TCP transports).
 *
 * On Windows: uses Named Pipes (\\.\pipe\<name>).
 * On POSIX:   uses AF_UNIX domain sockets.
 *
 * CLI: --mcp-socket <path>
 *   Windows: path is the pipe name (e.g., "cadence-mcp" -> \\.\pipe\cadence-mcp)
 *   POSIX:   path is the socket file path (e.g., /tmp/cadence-mcp.sock)
 */
#ifndef CD_MCP_SOCKET_H
#define CD_MCP_SOCKET_H

#include <stdint.h>
#include <stdbool.h>
#include "cadence/cd_core_types.h"

/* Forward declarations */
struct cd_kernel_t;

#include "cadence/cd_mcp.h"

/* ============================================================================
 * Constants
 * ============================================================================ */

/** Maximum simultaneous local IPC clients. */
#define CD_MCP_SOCKET_MAX_CLIENTS 16

/** Per-client read buffer size in bytes. */
#define CD_MCP_SOCKET_BUF_SIZE 8192

/* ============================================================================
 * Platform-specific client connection
 * ============================================================================ */

#ifdef _WIN32

#include <windows.h>

typedef struct {
    HANDLE   pipe;                              /* Named pipe handle */
    OVERLAPPED overlap;                         /* Overlapped I/O state */
    char     read_buf[CD_MCP_SOCKET_BUF_SIZE];  /* Accumulation buffer */
    uint32_t read_pos;                          /* Bytes in read_buf */
    bool     active;                            /* Slot in use */
    bool     authenticated;                     /* Auth handshake complete */
    bool     connecting;                        /* Waiting for ConnectNamedPipe */
    bool     reading;                           /* Pending overlapped read */
} cd_mcp_socket_client_t;

typedef struct {
    cd_mcp_socket_client_t   clients[CD_MCP_SOCKET_MAX_CLIENTS];
    char                     pipe_name[256];     /* Full \\.\pipe\<name> path */
    bool                     initialized;
    const char*              auth_token;
} cd_mcp_socket_server_t;

#else /* POSIX */

typedef struct {
    int      sock;                              /* Client file descriptor */
    char     read_buf[CD_MCP_SOCKET_BUF_SIZE];  /* Accumulation buffer */
    uint32_t read_pos;                          /* Bytes in read_buf */
    bool     active;                            /* Slot in use */
    bool     authenticated;                     /* Auth handshake complete */
} cd_mcp_socket_client_t;

typedef struct {
    int                      listen_sock;                          /* Listening fd */
    cd_mcp_socket_client_t   clients[CD_MCP_SOCKET_MAX_CLIENTS];  /* Client slots */
    char                     socket_path[108];                     /* sun_path max */
    bool                     initialized;                         /* Server ready */
    const char*              auth_token;                           /* Required token */
} cd_mcp_socket_server_t;

#endif /* _WIN32 */

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Initialize and start listening on the given path.
 *
 * On Windows: path is a pipe name (e.g., "cadence-mcp") which becomes
 *             \\.\pipe\cadence-mcp. If path already starts with \\.\pipe\,
 *             it is used as-is.
 * On POSIX:   path is a filesystem path for the Unix domain socket.
 *             Unlinks any existing file at path before binding.
 *
 * @param srv   Pointer to uninitialized socket server structure.
 * @param path  Pipe name (Windows) or socket file path (POSIX).
 * @return CD_OK on success, CD_ERR_NULL if srv or path is NULL,
 *         CD_ERR_IO on socket/pipe creation failure.
 */
cd_result_t cd_mcp_socket_init(cd_mcp_socket_server_t* srv, const char* path);

/**
 * Poll for new connections and incoming data (non-blocking).
 *
 * Call once per frame. Accepts pending connections, reads available data
 * from each client, and dispatches complete JSON-RPC lines via
 * cd_mcp_process_request(). Responses are sent back to the originating
 * client.
 *
 * @param srv     Pointer to initialized socket server.
 * @param mcp     Pointer to the MCP server (tool registry + dispatch).
 * @param kernel  Pointer to the engine kernel.
 * @return CD_OK on success, CD_ERR_NULL if any argument is NULL.
 */
cd_result_t cd_mcp_socket_poll(cd_mcp_socket_server_t* srv,
                                cd_mcp_server_t* mcp,
                                struct cd_kernel_t* kernel);

/**
 * Shut down the local IPC server. Closes all client connections and
 * cleans up resources. Safe to call on an uninitialized or
 * already-shut-down server.
 *
 * @param srv  Pointer to socket server structure.
 */
void cd_mcp_socket_shutdown(cd_mcp_socket_server_t* srv);

/**
 * Set the authentication token required for socket/pipe clients.
 *
 * When a non-NULL token is set, each client must send {"auth":"<token>"}
 * as its first line. The server responds with {"auth":"ok"} on success
 * or {"auth":"denied"} followed by disconnect on failure.
 *
 * @param srv    Pointer to socket server structure.
 * @param token  Auth token string, or NULL to disable authentication.
 */
void cd_mcp_socket_set_auth_token(cd_mcp_socket_server_t* srv,
                                   const char* token);

/**
 * Return the number of currently connected clients.
 *
 * @param srv  Pointer to socket server structure.
 * @return Number of active client connections, or 0 if srv is NULL.
 */
uint32_t cd_mcp_socket_client_count(const cd_mcp_socket_server_t* srv);

#endif /* CD_MCP_SOCKET_H */
