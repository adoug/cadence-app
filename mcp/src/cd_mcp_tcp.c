/* cd_mcp_tcp.c - Cadence Engine MCP TCP transport implementation
 *
 * Task 9.1: Non-blocking TCP server for remote MCP agents.
 *
 * Uses select() for portable non-blocking I/O.  Platform differences
 * (Winsock vs POSIX) are abstracted with small #ifdef blocks.
 *
 * Threading (Task S3.5): cd_mcp_tcp_poll() is called from the main
 * thread only (inside mcp_pre_tick). It accepts connections, reads data,
 * and dispatches requests synchronously — no background threads. The
 * output_fn/output_user_data swap on the MCP server is safe because all
 * dispatch is single-threaded. See cd_mcp_server.c header for details.
 *
 * Memory strategy: cd_mcp_tcp_client_t uses a fixed inline buffer
 * (CD_MCP_TCP_CLIENT_BUF_SIZE) — no dynamic allocation per client.
 * The only allocations are WSAStartup on Windows.
 */

#include "cadence/cd_mcp_tcp.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_agent.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"

#include <string.h>
#include <stdio.h>

#include "cJSON.h"

/* ============================================================================
 * Platform includes and helpers
 * ============================================================================ */

#ifdef _WIN32

#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

static bool g_wsa_initialized = false;

static bool cd_tcp_platform_init(void) {
    if (!g_wsa_initialized) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return false;
        }
        g_wsa_initialized = true;
    }
    return true;
}

static void cd_tcp_close_socket(cd_socket_t sock) {
    if (sock != CD_INVALID_SOCKET) {
        closesocket(sock);
    }
}

static bool cd_tcp_set_nonblocking(cd_socket_t sock) {
    u_long mode = 1;
    return (ioctlsocket(sock, FIONBIO, &mode) == 0);
}

static int cd_tcp_get_error(void) {
    return WSAGetLastError();
}

#define CD_TCP_WOULD_BLOCK(e) ((e) == WSAEWOULDBLOCK)

#else /* POSIX */

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static bool cd_tcp_platform_init(void) {
    return true; /* No-op on POSIX */
}

static void cd_tcp_close_socket(cd_socket_t sock) {
    if (sock != CD_INVALID_SOCKET) {
        close(sock);
    }
}

static bool cd_tcp_set_nonblocking(cd_socket_t sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return false;
    return (fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0);
}

static int cd_tcp_get_error(void) {
    return errno;
}

#define CD_TCP_WOULD_BLOCK(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)

#endif /* _WIN32 */

/* ============================================================================
 * Per-client output callback
 *
 * When dispatching a request from a TCP client, we temporarily redirect
 * the MCP server's output to the client's socket.
 * ============================================================================ */

typedef struct {
    cd_socket_t sock;
} cd_tcp_output_ctx_t;

static void cd_tcp_output_fn(const char* json_line, void* user_data) {
    cd_tcp_output_ctx_t* ctx = (cd_tcp_output_ctx_t*)user_data;
    if (ctx == NULL || ctx->sock == CD_INVALID_SOCKET) {
        return;
    }

    size_t len = strlen(json_line);

    /* Send the JSON line followed by a newline */
    send(ctx->sock, json_line, (int)len, 0);
    send(ctx->sock, "\n", 1, 0);
}

/* ============================================================================
 * Internal: constant-time token comparison (prevents timing attacks)
 * ============================================================================ */

static bool cd_tcp_token_compare(const char* a, const char* b) {
    if (a == NULL || b == NULL) {
        return false;
    }

    size_t a_len = strlen(a);
    size_t b_len = strlen(b);

    /* Always compare full length of expected token to avoid length leak */
    volatile uint8_t result = (uint8_t)(a_len ^ b_len);
    size_t cmp_len = (a_len < b_len) ? a_len : b_len;

    for (size_t i = 0; i < cmp_len; i++) {
        result |= (uint8_t)((unsigned char)a[i] ^ (unsigned char)b[i]);
    }

    return (result == 0);
}

/* ============================================================================
 * Internal: send a raw string to a client socket
 * ============================================================================ */

static void cd_tcp_send_line(cd_socket_t sock, const char* line) {
    send(sock, line, (int)strlen(line), 0);
    send(sock, "\n", 1, 0);
}

/* ============================================================================
 * Internal: handle authentication for a single line from a client
 *
 * Returns true if the line was consumed by auth logic (caller should skip it).
 * Returns false if auth is not required or client is already authenticated
 * (caller should process normally).
 *
 * Sets *disconnect to true if the client should be dropped.
 * ============================================================================ */

static bool cd_tcp_handle_auth(cd_mcp_tcp_client_t* client,
                                const char* auth_token,
                                const char* line,
                                bool* disconnect) {
    *disconnect = false;

    /* No auth required — mark as authenticated and pass through */
    if (auth_token == NULL) {
        client->authenticated = true;
        return false;
    }

    /* Already authenticated — pass through */
    if (client->authenticated) {
        return false;
    }

    /* Client has not authenticated yet — this line MUST be the auth message.
     * Expected format: {"auth":"<token>"}
     */
    cJSON* json = cJSON_Parse(line);
    if (json == NULL) {
        cd_tcp_send_line(client->sock, "{\"auth\":\"denied\"}");
        *disconnect = true;
        return true;
    }

    cJSON* auth_field = cJSON_GetObjectItemCaseSensitive(json, "auth");
    if (auth_field == NULL || !cJSON_IsString(auth_field)) {
        cJSON_Delete(json);
        cd_tcp_send_line(client->sock, "{\"auth\":\"denied\"}");
        *disconnect = true;
        return true;
    }

    if (cd_tcp_token_compare(auth_field->valuestring, auth_token)) {
        client->authenticated = true;
        cJSON_Delete(json);
        cd_tcp_send_line(client->sock, "{\"auth\":\"ok\"}");
        return true;
    }

    /* Wrong token */
    cJSON_Delete(json);
    cd_tcp_send_line(client->sock, "{\"auth\":\"denied\"}");
    *disconnect = true;
    return true;
}

/* ============================================================================
 * Internal: process complete lines from a client's buffer
 * ============================================================================ */

static bool cd_tcp_process_client_buffer(cd_mcp_tcp_client_t* client,
                                          cd_mcp_server_t* mcp,
                                          struct cd_kernel_t* kernel,
                                          const char* auth_token,
                                          int client_index) {
    /* Save the MCP server's current output callback */
    cd_mcp_output_fn_t saved_fn   = mcp->output_fn;
    void*              saved_data = mcp->output_user_data;

    /* Set the current agent for per-connection txn isolation */
    uint32_t agent_id = (uint32_t)(CD_MCP_AGENT_TCP_BASE + client_index);
    cd_mcp_set_current_agent(agent_id);

    /* Redirect output to this client's socket */
    cd_tcp_output_ctx_t ctx;
    ctx.sock = client->sock;

    mcp->output_fn        = cd_tcp_output_fn;
    mcp->output_user_data = &ctx;

    bool should_disconnect = false;

    /* Scan for complete newline-delimited lines */
    char* line_start = client->read_buf;
    char* newline;

    while ((newline = memchr(line_start, '\n',
            (size_t)(client->read_buf + client->read_pos - line_start))) != NULL) {
        /* Null-terminate the line */
        *newline = '\0';

        /* Strip optional \r */
        if (newline > line_start && *(newline - 1) == '\r') {
            *(newline - 1) = '\0';
        }

        /* Dispatch non-empty lines */
        if (line_start[0] != '\0') {
            bool disconnect = false;
            bool consumed = cd_tcp_handle_auth(client, auth_token,
                                                line_start, &disconnect);
            if (disconnect) {
                should_disconnect = true;
                line_start = newline + 1;
                break;
            }
            if (!consumed) {
                cd_mcp_process_request(mcp, kernel, line_start);
            }
        }

        line_start = newline + 1;
    }

    /* Move remaining partial data to the front */
    uint32_t remaining = (uint32_t)(client->read_buf + client->read_pos
                                     - line_start);
    if (remaining > 0 && line_start != client->read_buf) {
        memmove(client->read_buf, line_start, remaining);
    }
    client->read_pos = remaining;

    /* Restore original output callback */
    mcp->output_fn        = saved_fn;
    mcp->output_user_data = saved_data;

    return should_disconnect;
}

/* ============================================================================
 * Internal: disconnect a client
 * ============================================================================ */

static void cd_tcp_disconnect_client(cd_mcp_tcp_client_t* client) {
    cd_tcp_close_socket(client->sock);
    client->sock          = CD_INVALID_SOCKET;
    client->read_pos      = 0;
    client->active        = false;
    client->authenticated = false;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

cd_result_t cd_mcp_tcp_init(cd_mcp_tcp_server_t* tcp, uint16_t port) {
    if (tcp == NULL) {
        return CD_ERR_NULL;
    }

    memset(tcp, 0, sizeof(cd_mcp_tcp_server_t));
    tcp->listen_sock = CD_INVALID_SOCKET;

    /* Mark all client slots inactive and unauthenticated */
    for (int i = 0; i < CD_MCP_TCP_MAX_CLIENTS; i++) {
        tcp->clients[i].sock          = CD_INVALID_SOCKET;
        tcp->clients[i].active        = false;
        tcp->clients[i].authenticated = false;
    }

    /* Platform socket init (Winsock on Windows) */
    if (!cd_tcp_platform_init()) {
        return CD_ERR_IO;
    }

    /* Create listening socket */
    cd_socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == CD_INVALID_SOCKET) {
        return CD_ERR_IO;
    }

    /* Allow port reuse */
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    /* Set non-blocking */
    if (!cd_tcp_set_nonblocking(sock)) {
        cd_tcp_close_socket(sock);
        return CD_ERR_IO;
    }

    /* Bind to 0.0.0.0:port */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        cd_tcp_close_socket(sock);
        return CD_ERR_IO;
    }

    /* Retrieve the actual bound port (important when port == 0) */
    struct sockaddr_in bound_addr;
    int addr_len = (int)sizeof(bound_addr);
    if (getsockname(sock, (struct sockaddr*)&bound_addr, &addr_len) == 0) {
        tcp->port = ntohs(bound_addr.sin_port);
    } else {
        tcp->port = port;
    }

    /* Start listening */
    if (listen(sock, CD_MCP_TCP_MAX_CLIENTS) != 0) {
        cd_tcp_close_socket(sock);
        return CD_ERR_IO;
    }

    tcp->listen_sock = sock;
    tcp->initialized = true;

    return CD_OK;
}

cd_result_t cd_mcp_tcp_poll(cd_mcp_tcp_server_t* tcp,
                             cd_mcp_server_t* mcp,
                             struct cd_kernel_t* kernel) {
    if (tcp == NULL || mcp == NULL) {
        return CD_ERR_NULL;
    }

    if (!tcp->initialized) {
        return CD_ERR_INVALID;
    }

    /* Build fd_set for select() */
    fd_set read_fds;
    FD_ZERO(&read_fds);

    FD_SET(tcp->listen_sock, &read_fds);
    cd_socket_t max_fd = tcp->listen_sock;

    for (int i = 0; i < CD_MCP_TCP_MAX_CLIENTS; i++) {
        if (tcp->clients[i].active) {
            FD_SET(tcp->clients[i].sock, &read_fds);
            if (tcp->clients[i].sock > max_fd) {
                max_fd = tcp->clients[i].sock;
            }
        }
    }

    /* Non-blocking select (0 timeout) */
    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 0;

    int ready = select((int)(max_fd + 1), &read_fds, NULL, NULL, &tv);
    if (ready <= 0) {
        return CD_OK; /* Nothing ready or error */
    }

    /* Accept all pending new connections */
    if (FD_ISSET(tcp->listen_sock, &read_fds)) {
        for (;;) {
            struct sockaddr_in client_addr;
            int client_addr_len = (int)sizeof(client_addr);
            cd_socket_t client_sock = accept(tcp->listen_sock,
                                              (struct sockaddr*)&client_addr,
                                              &client_addr_len);

            if (client_sock == CD_INVALID_SOCKET) {
                break; /* No more pending connections */
            }

            /* Find a free slot */
            bool placed = false;
            for (int i = 0; i < CD_MCP_TCP_MAX_CLIENTS; i++) {
                if (!tcp->clients[i].active) {
                    cd_tcp_set_nonblocking(client_sock);
                    tcp->clients[i].sock          = client_sock;
                    tcp->clients[i].read_pos      = 0;
                    tcp->clients[i].active        = true;
                    tcp->clients[i].authenticated = false;
                    memset(tcp->clients[i].read_buf, 0,
                           CD_MCP_TCP_CLIENT_BUF_SIZE);
                    placed = true;
                    break;
                }
            }

            if (!placed) {
                /* No free slots — reject */
                cd_tcp_close_socket(client_sock);
            }
        }
    }

    /* Read from active clients */
    for (int i = 0; i < CD_MCP_TCP_MAX_CLIENTS; i++) {
        if (!tcp->clients[i].active) {
            continue;
        }

        if (!FD_ISSET(tcp->clients[i].sock, &read_fds)) {
            continue;
        }

        cd_mcp_tcp_client_t* client = &tcp->clients[i];
        uint32_t space = CD_MCP_TCP_CLIENT_BUF_SIZE - client->read_pos - 1;

        if (space == 0) {
            /* Buffer full with no complete line — drop the client */
            cd_tcp_disconnect_client(client);
            continue;
        }

        int n = recv(client->sock,
                     client->read_buf + client->read_pos,
                     (int)space, 0);

        if (n <= 0) {
            /* n == 0: graceful disconnect. n < 0: check if would-block */
            if (n < 0 && CD_TCP_WOULD_BLOCK(cd_tcp_get_error())) {
                continue; /* Spurious wakeup */
            }
            cd_tcp_disconnect_client(client);
            continue;
        }

        client->read_pos += (uint32_t)n;
        client->read_buf[client->read_pos] = '\0';

        /* Process any complete lines */
        bool auth_rejected = cd_tcp_process_client_buffer(
            client, mcp, kernel, tcp->auth_token, i);
        if (auth_rejected) {
            cd_tcp_disconnect_client(client);
        }
    }

    return CD_OK;
}

void cd_mcp_tcp_shutdown(cd_mcp_tcp_server_t* tcp) {
    if (tcp == NULL) {
        return;
    }

    /* Close all client connections */
    for (int i = 0; i < CD_MCP_TCP_MAX_CLIENTS; i++) {
        if (tcp->clients[i].active) {
            cd_tcp_disconnect_client(&tcp->clients[i]);
        }
    }

    /* Close listening socket */
    cd_tcp_close_socket(tcp->listen_sock);
    tcp->listen_sock = CD_INVALID_SOCKET;

    tcp->initialized = false;
}

void cd_mcp_tcp_set_auth_token(cd_mcp_tcp_server_t* tcp, const char* token) {
    if (tcp != NULL) {
        tcp->auth_token = token;
    }
}

uint32_t cd_mcp_tcp_client_count(const cd_mcp_tcp_server_t* tcp) {
    if (tcp == NULL) {
        return 0;
    }

    uint32_t count = 0;
    for (int i = 0; i < CD_MCP_TCP_MAX_CLIENTS; i++) {
        if (tcp->clients[i].active) {
            count++;
        }
    }
    return count;
}
