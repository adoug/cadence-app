/* cd_mcp_socket.c - Cadence Engine MCP local IPC transport
 *
 * Task S2.3: Named Pipe (Windows) / Unix domain socket (POSIX) transport.
 *
 * Windows: Uses Named Pipes with polling-based non-blocking I/O.
 *          Each client slot has its own pipe instance created with
 *          FILE_FLAG_OVERLAPPED. We use GetOverlappedResult with
 *          bWait=FALSE for non-blocking checks.
 *
 * POSIX:   Uses AF_UNIX domain sockets with select() for non-blocking I/O.
 *
 * Memory strategy: cd_mcp_socket_client_t uses a fixed inline buffer
 * (CD_MCP_SOCKET_BUF_SIZE) -- no dynamic allocation per client.
 */

#include "cadence/cd_mcp_socket.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_agent.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"

#include <string.h>
#include <stdio.h>

#include "cJSON.h"

/* ============================================================================
 * Shared: constant-time token comparison (prevents timing attacks)
 * ============================================================================ */

static bool cd_pipe_token_compare(const char* a, const char* b) {
    if (a == NULL || b == NULL) {
        return false;
    }

    size_t a_len = strlen(a);
    size_t b_len = strlen(b);

    volatile uint8_t result = (uint8_t)(a_len ^ b_len);
    size_t cmp_len = (a_len < b_len) ? a_len : b_len;

    for (size_t i = 0; i < cmp_len; i++) {
        result |= (uint8_t)((unsigned char)a[i] ^ (unsigned char)b[i]);
    }

    return (result == 0);
}

/* ============================================================================
 *
 * WINDOWS NAMED PIPE IMPLEMENTATION
 *
 * ============================================================================ */

#ifdef _WIN32

/* ============================================================================
 * Internal: send a string to a pipe handle
 * ============================================================================ */

static void cd_pipe_send_line(HANDLE pipe, const char* line) {
    DWORD written;
    DWORD len = (DWORD)strlen(line);
    WriteFile(pipe, line, len, &written, NULL);
    WriteFile(pipe, "\n", 1, &written, NULL);
}

/* ============================================================================
 * Per-client output callback
 * ============================================================================ */

typedef struct {
    HANDLE pipe;
} cd_pipe_output_ctx_t;

static void cd_pipe_output_fn(const char* json_line, void* user_data) {
    cd_pipe_output_ctx_t* ctx = (cd_pipe_output_ctx_t*)user_data;
    if (ctx == NULL || ctx->pipe == INVALID_HANDLE_VALUE) {
        return;
    }

    DWORD written;
    DWORD len = (DWORD)strlen(json_line);
    WriteFile(ctx->pipe, json_line, len, &written, NULL);
    WriteFile(ctx->pipe, "\n", 1, &written, NULL);
}

/* ============================================================================
 * Internal: handle authentication for a single line from a client
 * ============================================================================ */

static bool cd_pipe_handle_auth(cd_mcp_socket_client_t* client,
                                 const char* auth_token,
                                 const char* line,
                                 bool* disconnect) {
    *disconnect = false;

    if (auth_token == NULL) {
        client->authenticated = true;
        return false;
    }

    if (client->authenticated) {
        return false;
    }

    cJSON* json = cJSON_Parse(line);
    if (json == NULL) {
        cd_pipe_send_line(client->pipe, "{\"auth\":\"denied\"}");
        *disconnect = true;
        return true;
    }

    cJSON* auth_field = cJSON_GetObjectItemCaseSensitive(json, "auth");
    if (auth_field == NULL || !cJSON_IsString(auth_field)) {
        cJSON_Delete(json);
        cd_pipe_send_line(client->pipe, "{\"auth\":\"denied\"}");
        *disconnect = true;
        return true;
    }

    if (cd_pipe_token_compare(auth_field->valuestring, auth_token)) {
        client->authenticated = true;
        cJSON_Delete(json);
        cd_pipe_send_line(client->pipe, "{\"auth\":\"ok\"}");
        return true;
    }

    cJSON_Delete(json);
    cd_pipe_send_line(client->pipe, "{\"auth\":\"denied\"}");
    *disconnect = true;
    return true;
}

/* ============================================================================
 * Internal: process complete lines from a client's buffer
 * ============================================================================ */

static bool cd_pipe_process_client_buffer(cd_mcp_socket_client_t* client,
                                           cd_mcp_server_t* mcp,
                                           struct cd_kernel_t* kernel,
                                           const char* auth_token,
                                           int client_index) {
    cd_mcp_output_fn_t saved_fn   = mcp->output_fn;
    void*              saved_data = mcp->output_user_data;

    uint32_t agent_id = (uint32_t)(CD_MCP_AGENT_SOCKET_BASE + client_index);
    cd_mcp_set_current_agent(agent_id);

    cd_pipe_output_ctx_t ctx;
    ctx.pipe = client->pipe;

    mcp->output_fn        = cd_pipe_output_fn;
    mcp->output_user_data = &ctx;

    bool should_disconnect = false;

    char* line_start = client->read_buf;
    char* newline;

    while ((newline = memchr(line_start, '\n',
            (size_t)(client->read_buf + client->read_pos - line_start))) != NULL) {
        *newline = '\0';

        if (newline > line_start && *(newline - 1) == '\r') {
            *(newline - 1) = '\0';
        }

        if (line_start[0] != '\0') {
            bool disconnect = false;
            bool consumed = cd_pipe_handle_auth(client, auth_token,
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

    uint32_t remaining = (uint32_t)(client->read_buf + client->read_pos
                                     - line_start);
    if (remaining > 0 && line_start != client->read_buf) {
        memmove(client->read_buf, line_start, remaining);
    }
    client->read_pos = remaining;

    mcp->output_fn        = saved_fn;
    mcp->output_user_data = saved_data;

    return should_disconnect;
}

/* ============================================================================
 * Internal: disconnect a pipe client
 * ============================================================================ */

static void cd_pipe_disconnect_client(cd_mcp_socket_client_t* client) {
    if (client->pipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(client->pipe);
        CloseHandle(client->pipe);
    }
    if (client->overlap.hEvent != NULL) {
        CloseHandle(client->overlap.hEvent);
    }
    memset(client, 0, sizeof(*client));
    client->pipe       = INVALID_HANDLE_VALUE;
    client->active     = false;
    client->connecting = false;
    client->reading    = false;
}

/* ============================================================================
 * Internal: create a new pipe instance and start listening for a connection
 * ============================================================================ */

static bool cd_pipe_create_instance(cd_mcp_socket_client_t* client,
                                     const char* pipe_name) {
    memset(client, 0, sizeof(*client));
    client->pipe       = INVALID_HANDLE_VALUE;
    client->active     = false;
    client->connecting = false;
    client->reading    = false;

    client->overlap.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (client->overlap.hEvent == NULL) {
        return false;
    }

    client->pipe = CreateNamedPipeA(
        pipe_name,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        CD_MCP_SOCKET_MAX_CLIENTS,
        CD_MCP_SOCKET_BUF_SIZE,
        CD_MCP_SOCKET_BUF_SIZE,
        0,
        NULL
    );

    if (client->pipe == INVALID_HANDLE_VALUE) {
        CloseHandle(client->overlap.hEvent);
        client->overlap.hEvent = NULL;
        return false;
    }

    /* Start listening for a connection (overlapped) */
    BOOL connected = ConnectNamedPipe(client->pipe, &client->overlap);
    if (connected) {
        /* Already connected (rare but possible) */
        client->active     = true;
        client->connecting = false;
        return true;
    }

    DWORD err = GetLastError();
    if (err == ERROR_IO_PENDING) {
        /* Connection pending -- will complete later */
        client->connecting = true;
        return true;
    } else if (err == ERROR_PIPE_CONNECTED) {
        /* Client connected between CreateNamedPipe and ConnectNamedPipe */
        client->active     = true;
        client->connecting = false;
        return true;
    }

    /* Unexpected error */
    CloseHandle(client->pipe);
    CloseHandle(client->overlap.hEvent);
    client->pipe = INVALID_HANDLE_VALUE;
    client->overlap.hEvent = NULL;
    return false;
}

/* ============================================================================
 * Public API - Windows
 * ============================================================================ */

cd_result_t cd_mcp_socket_init(cd_mcp_socket_server_t* srv, const char* path) {
    if (srv == NULL || path == NULL) {
        return CD_ERR_NULL;
    }

    memset(srv, 0, sizeof(cd_mcp_socket_server_t));

    /* Build full pipe name */
    if (strncmp(path, "\\\\.\\pipe\\", 9) == 0) {
        /* Already a full pipe path */
        snprintf(srv->pipe_name, sizeof(srv->pipe_name), "%s", path);
    } else {
        snprintf(srv->pipe_name, sizeof(srv->pipe_name),
                 "\\\\.\\pipe\\%s", path);
    }

    /* Initialize all client slots */
    for (int i = 0; i < CD_MCP_SOCKET_MAX_CLIENTS; i++) {
        memset(&srv->clients[i], 0, sizeof(cd_mcp_socket_client_t));
        srv->clients[i].pipe   = INVALID_HANDLE_VALUE;
        srv->clients[i].active = false;
    }

    /* Create first pipe instance to start accepting connections */
    if (!cd_pipe_create_instance(&srv->clients[0], srv->pipe_name)) {
        return CD_ERR_IO;
    }

    srv->initialized = true;

    return CD_OK;
}

cd_result_t cd_mcp_socket_poll(cd_mcp_socket_server_t* srv,
                                cd_mcp_server_t* mcp,
                                struct cd_kernel_t* kernel) {
    if (srv == NULL || mcp == NULL) {
        return CD_ERR_NULL;
    }

    if (!srv->initialized) {
        return CD_ERR_INVALID;
    }

    for (int i = 0; i < CD_MCP_SOCKET_MAX_CLIENTS; i++) {
        cd_mcp_socket_client_t* client = &srv->clients[i];

        /* Check if a pending connection has completed */
        if (client->connecting) {
            DWORD bytes_transferred = 0;
            BOOL result = GetOverlappedResult(client->pipe, &client->overlap,
                                               &bytes_transferred, FALSE);
            if (result) {
                /* Client connected */
                client->connecting = false;
                client->active     = true;
                client->reading    = false;
                ResetEvent(client->overlap.hEvent);

                /* Create a new pipe instance in the next free slot
                 * so we can accept more connections */
                for (int j = 0; j < CD_MCP_SOCKET_MAX_CLIENTS; j++) {
                    if (!srv->clients[j].active && !srv->clients[j].connecting
                        && srv->clients[j].pipe == INVALID_HANDLE_VALUE) {
                        cd_pipe_create_instance(&srv->clients[j], srv->pipe_name);
                        break;
                    }
                }
            } else {
                DWORD err = GetLastError();
                if (err != ERROR_IO_INCOMPLETE) {
                    /* Connection failed -- reset this slot and recreate */
                    cd_pipe_disconnect_client(client);
                    cd_pipe_create_instance(client, srv->pipe_name);
                }
                /* ERROR_IO_INCOMPLETE means still waiting -- continue */
            }
            continue;
        }

        /* Read data from active clients */
        if (!client->active) {
            continue;
        }

        /* Start a new overlapped read if one is not pending */
        if (!client->reading) {
            uint32_t space = CD_MCP_SOCKET_BUF_SIZE - client->read_pos - 1;
            if (space == 0) {
                /* Buffer full with no complete line -- drop the client */
                cd_pipe_disconnect_client(client);
                cd_pipe_create_instance(client, srv->pipe_name);
                continue;
            }

            ResetEvent(client->overlap.hEvent);
            DWORD bytes_read = 0;
            BOOL ok = ReadFile(
                client->pipe,
                client->read_buf + client->read_pos,
                space,
                &bytes_read,
                &client->overlap
            );

            if (ok) {
                /* Read completed immediately */
                if (bytes_read > 0) {
                    client->read_pos += bytes_read;
                    client->read_buf[client->read_pos] = '\0';

                    bool auth_rejected = cd_pipe_process_client_buffer(
                        client, mcp, kernel, srv->auth_token, i);
                    if (auth_rejected) {
                        cd_pipe_disconnect_client(client);
                        cd_pipe_create_instance(client, srv->pipe_name);
                    }
                }
                /* Don't set reading=true; start another read next poll */
            } else {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    client->reading = true;
                } else if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) {
                    /* Client disconnected */
                    cd_pipe_disconnect_client(client);
                    cd_pipe_create_instance(client, srv->pipe_name);
                } else {
                    /* Other error -- drop client */
                    cd_pipe_disconnect_client(client);
                    cd_pipe_create_instance(client, srv->pipe_name);
                }
            }
        } else {
            /* Check if a pending read has completed */
            DWORD bytes_read = 0;
            BOOL result = GetOverlappedResult(client->pipe, &client->overlap,
                                               &bytes_read, FALSE);
            if (result) {
                client->reading = false;
                if (bytes_read > 0) {
                    client->read_pos += bytes_read;
                    client->read_buf[client->read_pos] = '\0';

                    bool auth_rejected = cd_pipe_process_client_buffer(
                        client, mcp, kernel, srv->auth_token, i);
                    if (auth_rejected) {
                        cd_pipe_disconnect_client(client);
                        cd_pipe_create_instance(client, srv->pipe_name);
                    }
                }
            } else {
                DWORD err = GetLastError();
                if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) {
                    client->reading = false;
                    cd_pipe_disconnect_client(client);
                    cd_pipe_create_instance(client, srv->pipe_name);
                } else if (err != ERROR_IO_INCOMPLETE) {
                    /* Unexpected error */
                    client->reading = false;
                    cd_pipe_disconnect_client(client);
                    cd_pipe_create_instance(client, srv->pipe_name);
                }
                /* ERROR_IO_INCOMPLETE means read still pending -- keep waiting */
            }
        }
    }

    return CD_OK;
}

void cd_mcp_socket_shutdown(cd_mcp_socket_server_t* srv) {
    if (srv == NULL) {
        return;
    }

    for (int i = 0; i < CD_MCP_SOCKET_MAX_CLIENTS; i++) {
        if (srv->clients[i].pipe != INVALID_HANDLE_VALUE) {
            if (srv->clients[i].reading || srv->clients[i].connecting) {
                CancelIo(srv->clients[i].pipe);
            }
            cd_pipe_disconnect_client(&srv->clients[i]);
        }
    }

    srv->initialized = false;
}

void cd_mcp_socket_set_auth_token(cd_mcp_socket_server_t* srv,
                                   const char* token) {
    if (srv != NULL) {
        srv->auth_token = token;
    }
}

uint32_t cd_mcp_socket_client_count(const cd_mcp_socket_server_t* srv) {
    if (srv == NULL) {
        return 0;
    }

    uint32_t count = 0;
    for (int i = 0; i < CD_MCP_SOCKET_MAX_CLIENTS; i++) {
        if (srv->clients[i].active) {
            count++;
        }
    }
    return count;
}

/* ============================================================================
 *
 * POSIX UNIX DOMAIN SOCKET IMPLEMENTATION
 *
 * ============================================================================ */

#else /* POSIX */

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* ============================================================================
 * Internal: set a file descriptor to non-blocking
 * ============================================================================ */

static bool cd_socket_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

/* ============================================================================
 * Per-client output callback
 * ============================================================================ */

typedef struct {
    int sock;
} cd_socket_output_ctx_t;

static void cd_socket_output_fn(const char* json_line, void* user_data) {
    cd_socket_output_ctx_t* ctx = (cd_socket_output_ctx_t*)user_data;
    if (ctx == NULL || ctx->sock < 0) {
        return;
    }

    size_t len = strlen(json_line);
    (void)write(ctx->sock, json_line, len);
    (void)write(ctx->sock, "\n", 1);
}

/* ============================================================================
 * Internal: send a raw string to a client socket
 * ============================================================================ */

static void cd_socket_send_line(int sock, const char* line) {
    (void)write(sock, line, strlen(line));
    (void)write(sock, "\n", 1);
}

/* ============================================================================
 * Internal: handle authentication for a single line from a client
 * ============================================================================ */

static bool cd_socket_handle_auth(cd_mcp_socket_client_t* client,
                                   const char* auth_token,
                                   const char* line,
                                   bool* disconnect) {
    *disconnect = false;

    if (auth_token == NULL) {
        client->authenticated = true;
        return false;
    }

    if (client->authenticated) {
        return false;
    }

    cJSON* json = cJSON_Parse(line);
    if (json == NULL) {
        cd_socket_send_line(client->sock, "{\"auth\":\"denied\"}");
        *disconnect = true;
        return true;
    }

    cJSON* auth_field = cJSON_GetObjectItemCaseSensitive(json, "auth");
    if (auth_field == NULL || !cJSON_IsString(auth_field)) {
        cJSON_Delete(json);
        cd_socket_send_line(client->sock, "{\"auth\":\"denied\"}");
        *disconnect = true;
        return true;
    }

    if (cd_pipe_token_compare(auth_field->valuestring, auth_token)) {
        client->authenticated = true;
        cJSON_Delete(json);
        cd_socket_send_line(client->sock, "{\"auth\":\"ok\"}");
        return true;
    }

    cJSON_Delete(json);
    cd_socket_send_line(client->sock, "{\"auth\":\"denied\"}");
    *disconnect = true;
    return true;
}

/* ============================================================================
 * Internal: process complete lines from a client's buffer
 * ============================================================================ */

static bool cd_socket_process_client_buffer(cd_mcp_socket_client_t* client,
                                             cd_mcp_server_t* mcp,
                                             struct cd_kernel_t* kernel,
                                             const char* auth_token,
                                             int client_index) {
    cd_mcp_output_fn_t saved_fn   = mcp->output_fn;
    void*              saved_data = mcp->output_user_data;

    uint32_t agent_id = (uint32_t)(CD_MCP_AGENT_SOCKET_BASE + client_index);
    cd_mcp_set_current_agent(agent_id);

    cd_socket_output_ctx_t ctx;
    ctx.sock = client->sock;

    mcp->output_fn        = cd_socket_output_fn;
    mcp->output_user_data = &ctx;

    bool should_disconnect = false;

    char* line_start = client->read_buf;
    char* newline;

    while ((newline = memchr(line_start, '\n',
            (size_t)(client->read_buf + client->read_pos - line_start))) != NULL) {
        *newline = '\0';

        if (newline > line_start && *(newline - 1) == '\r') {
            *(newline - 1) = '\0';
        }

        if (line_start[0] != '\0') {
            bool disconnect = false;
            bool consumed = cd_socket_handle_auth(client, auth_token,
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

    uint32_t remaining = (uint32_t)(client->read_buf + client->read_pos
                                     - line_start);
    if (remaining > 0 && line_start != client->read_buf) {
        memmove(client->read_buf, line_start, remaining);
    }
    client->read_pos = remaining;

    mcp->output_fn        = saved_fn;
    mcp->output_user_data = saved_data;

    return should_disconnect;
}

/* ============================================================================
 * Internal: disconnect a client
 * ============================================================================ */

static void cd_socket_disconnect_client(cd_mcp_socket_client_t* client) {
    if (client->sock >= 0) {
        close(client->sock);
    }
    client->sock          = -1;
    client->read_pos      = 0;
    client->active        = false;
    client->authenticated = false;
}

/* ============================================================================
 * Public API - POSIX
 * ============================================================================ */

cd_result_t cd_mcp_socket_init(cd_mcp_socket_server_t* srv, const char* path) {
    if (srv == NULL || path == NULL) {
        return CD_ERR_NULL;
    }

    memset(srv, 0, sizeof(cd_mcp_socket_server_t));
    srv->listen_sock = -1;

    for (int i = 0; i < CD_MCP_SOCKET_MAX_CLIENTS; i++) {
        srv->clients[i].sock          = -1;
        srv->clients[i].active        = false;
        srv->clients[i].authenticated = false;
    }

    size_t path_len = strlen(path);
    if (path_len == 0 || path_len >= sizeof(srv->socket_path)) {
        return CD_ERR_INVALID;
    }

    strncpy(srv->socket_path, path, sizeof(srv->socket_path) - 1);
    srv->socket_path[sizeof(srv->socket_path) - 1] = '\0';

    unlink(srv->socket_path);

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        return CD_ERR_IO;
    }

    if (!cd_socket_set_nonblocking(sock)) {
        close(sock);
        return CD_ERR_IO;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, srv->socket_path, sizeof(addr.sun_path) - 1);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(sock);
        return CD_ERR_IO;
    }

    if (listen(sock, CD_MCP_SOCKET_MAX_CLIENTS) != 0) {
        close(sock);
        unlink(srv->socket_path);
        return CD_ERR_IO;
    }

    srv->listen_sock = sock;
    srv->initialized = true;

    return CD_OK;
}

cd_result_t cd_mcp_socket_poll(cd_mcp_socket_server_t* srv,
                                cd_mcp_server_t* mcp,
                                struct cd_kernel_t* kernel) {
    if (srv == NULL || mcp == NULL) {
        return CD_ERR_NULL;
    }

    if (!srv->initialized) {
        return CD_ERR_INVALID;
    }

    fd_set read_fds;
    FD_ZERO(&read_fds);

    FD_SET(srv->listen_sock, &read_fds);
    int max_fd = srv->listen_sock;

    for (int i = 0; i < CD_MCP_SOCKET_MAX_CLIENTS; i++) {
        if (srv->clients[i].active) {
            FD_SET(srv->clients[i].sock, &read_fds);
            if (srv->clients[i].sock > max_fd) {
                max_fd = srv->clients[i].sock;
            }
        }
    }

    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 0;

    int ready = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
    if (ready <= 0) {
        return CD_OK;
    }

    if (FD_ISSET(srv->listen_sock, &read_fds)) {
        for (;;) {
            int client_sock = accept(srv->listen_sock, NULL, NULL);

            if (client_sock < 0) {
                break;
            }

            bool placed = false;
            for (int i = 0; i < CD_MCP_SOCKET_MAX_CLIENTS; i++) {
                if (!srv->clients[i].active) {
                    cd_socket_set_nonblocking(client_sock);
                    srv->clients[i].sock          = client_sock;
                    srv->clients[i].read_pos      = 0;
                    srv->clients[i].active        = true;
                    srv->clients[i].authenticated = false;
                    memset(srv->clients[i].read_buf, 0,
                           CD_MCP_SOCKET_BUF_SIZE);
                    placed = true;
                    break;
                }
            }

            if (!placed) {
                close(client_sock);
            }
        }
    }

    for (int i = 0; i < CD_MCP_SOCKET_MAX_CLIENTS; i++) {
        if (!srv->clients[i].active) {
            continue;
        }

        if (!FD_ISSET(srv->clients[i].sock, &read_fds)) {
            continue;
        }

        cd_mcp_socket_client_t* client = &srv->clients[i];
        uint32_t space = CD_MCP_SOCKET_BUF_SIZE - client->read_pos - 1;

        if (space == 0) {
            cd_socket_disconnect_client(client);
            continue;
        }

        ssize_t n = read(client->sock,
                         client->read_buf + client->read_pos,
                         space);

        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }
            cd_socket_disconnect_client(client);
            continue;
        }

        client->read_pos += (uint32_t)n;
        client->read_buf[client->read_pos] = '\0';

        bool auth_rejected = cd_socket_process_client_buffer(
            client, mcp, kernel, srv->auth_token, i);
        if (auth_rejected) {
            cd_socket_disconnect_client(client);
        }
    }

    return CD_OK;
}

void cd_mcp_socket_shutdown(cd_mcp_socket_server_t* srv) {
    if (srv == NULL) {
        return;
    }

    for (int i = 0; i < CD_MCP_SOCKET_MAX_CLIENTS; i++) {
        if (srv->clients[i].active) {
            cd_socket_disconnect_client(&srv->clients[i]);
        }
    }

    if (srv->listen_sock >= 0) {
        close(srv->listen_sock);
        srv->listen_sock = -1;
    }

    if (srv->socket_path[0] != '\0') {
        unlink(srv->socket_path);
    }

    srv->initialized = false;
}

void cd_mcp_socket_set_auth_token(cd_mcp_socket_server_t* srv,
                                   const char* token) {
    if (srv != NULL) {
        srv->auth_token = token;
    }
}

uint32_t cd_mcp_socket_client_count(const cd_mcp_socket_server_t* srv) {
    if (srv == NULL) {
        return 0;
    }

    uint32_t count = 0;
    for (int i = 0; i < CD_MCP_SOCKET_MAX_CLIENTS; i++) {
        if (srv->clients[i].active) {
            count++;
        }
    }
    return count;
}

#endif /* _WIN32 / POSIX */
