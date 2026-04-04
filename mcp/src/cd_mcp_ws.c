/* cd_mcp_ws.c - Cadence Engine MCP WebSocket transport implementation
 *
 * Task 19.1: WebSocket (RFC 6455) server for browser-based MCP agents.
 *
 * Implements the WebSocket protocol on top of TCP sockets:
 *   - HTTP upgrade handshake with Sec-WebSocket-Accept
 *   - Text frame decoding (client-to-server, masked)
 *   - Text frame encoding (server-to-client, unmasked)
 *   - Close frame handling
 *   - Ping/pong for keep-alive
 *
 * Uses select() for portable non-blocking I/O, same pattern as cd_mcp_tcp.c.
 *
 * Threading (Task S3.5): cd_mcp_ws_poll() is called from the main thread
 * only. It accepts connections, reads data, performs handshakes, and
 * dispatches requests synchronously — no background threads. The
 * output_fn/output_user_data swap on the MCP server is safe because all
 * dispatch is single-threaded. See cd_mcp_server.c header for details.
 *
 * Memory: fixed inline buffers per client — no dynamic allocation.
 * SHA-1 and Base64 are implemented inline (only ~80 lines total, used
 * exclusively for the WebSocket handshake).
 */

#include "cadence/cd_mcp_ws.h"
#include "cadence/cd_mcp.h"
#include "cadence/cd_mcp_agent.h"
#include "cadence/cd_kernel.h"
#include "cadence/cd_kernel_api.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ============================================================================
 * Platform includes and helpers (same abstraction as cd_mcp_tcp.c)
 * ============================================================================ */

#ifdef _WIN32

#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

static bool g_ws_wsa_initialized = false;

static bool cd_ws_platform_init(void) {
    if (!g_ws_wsa_initialized) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return false;
        }
        g_ws_wsa_initialized = true;
    }
    return true;
}

static void cd_ws_close_socket(cd_ws_socket_t sock) {
    if (sock != CD_WS_INVALID_SOCKET) {
        closesocket(sock);
    }
}

static bool cd_ws_set_nonblocking(cd_ws_socket_t sock) {
    u_long mode = 1;
    return (ioctlsocket(sock, FIONBIO, &mode) == 0);
}

static int cd_ws_get_error(void) {
    return WSAGetLastError();
}

#define CD_WS_WOULD_BLOCK(e) ((e) == WSAEWOULDBLOCK)

#else /* POSIX */

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static bool cd_ws_platform_init(void) {
    return true;
}

static void cd_ws_close_socket(cd_ws_socket_t sock) {
    if (sock != CD_WS_INVALID_SOCKET) {
        close(sock);
    }
}

static bool cd_ws_set_nonblocking(cd_ws_socket_t sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return false;
    return (fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0);
}

static int cd_ws_get_error(void) {
    return errno;
}

#define CD_WS_WOULD_BLOCK(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)

#endif /* _WIN32 */

/* ============================================================================
 * Minimal SHA-1 (RFC 3174) — used only for WebSocket handshake
 * ============================================================================ */

typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buffer[64];
} cd_sha1_ctx_t;

static uint32_t cd_sha1_rotl(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

static void cd_sha1_transform(cd_sha1_ctx_t* ctx, const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = cd_sha1_rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2];
    uint32_t d = ctx->state[3], e = ctx->state[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else              { f = b ^ c ^ d;                   k = 0xCA62C1D6; }

        uint32_t temp = cd_sha1_rotl(a, 5) + f + e + k + w[i];
        e = d; d = c; c = cd_sha1_rotl(b, 30); b = a; a = temp;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
    ctx->state[3] += d; ctx->state[4] += e;
}

static void cd_sha1_init(cd_sha1_ctx_t* ctx) {
    ctx->state[0] = 0x67452301; ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE; ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count = 0;
    memset(ctx->buffer, 0, 64);
}

static void cd_sha1_update(cd_sha1_ctx_t* ctx, const uint8_t* data, size_t len) {
    size_t offset = (size_t)(ctx->count % 64);
    ctx->count += len;

    for (size_t i = 0; i < len; i++) {
        ctx->buffer[offset++] = data[i];
        if (offset == 64) {
            cd_sha1_transform(ctx, ctx->buffer);
            offset = 0;
        }
    }
}

static void cd_sha1_final(cd_sha1_ctx_t* ctx, uint8_t digest[20]) {
    uint64_t bits = ctx->count * 8;
    uint8_t pad = 0x80;
    cd_sha1_update(ctx, &pad, 1);

    pad = 0;
    while ((ctx->count % 64) != 56) {
        cd_sha1_update(ctx, &pad, 1);
    }

    uint8_t len_bytes[8];
    for (int i = 7; i >= 0; i--) {
        len_bytes[i] = (uint8_t)(bits & 0xFF);
        bits >>= 8;
    }
    cd_sha1_update(ctx, len_bytes, 8);

    for (int i = 0; i < 5; i++) {
        digest[i*4+0] = (uint8_t)(ctx->state[i] >> 24);
        digest[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i*4+2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i*4+3] = (uint8_t)(ctx->state[i]);
    }
}

/* ============================================================================
 * Minimal Base64 encode — used only for WebSocket handshake
 * ============================================================================ */

static const char cd_b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void cd_base64_encode(const uint8_t* in, size_t in_len,
                              char* out, size_t out_cap) {
    size_t j = 0;
    for (size_t i = 0; i < in_len && j + 4 < out_cap; ) {
        uint32_t a = (i < in_len) ? in[i++] : 0;
        uint32_t b = (i < in_len) ? in[i++] : 0;
        uint32_t c = (i < in_len) ? in[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;

        out[j++] = cd_b64_table[(triple >> 18) & 0x3F];
        out[j++] = cd_b64_table[(triple >> 12) & 0x3F];
        out[j++] = cd_b64_table[(triple >> 6) & 0x3F];
        out[j++] = cd_b64_table[triple & 0x3F];
    }

    /* Pad with '=' */
    size_t mod = in_len % 3;
    if (mod == 1 && j >= 2) { out[j-1] = '='; out[j-2] = '='; }
    else if (mod == 2 && j >= 1) { out[j-1] = '='; }

    if (j < out_cap) out[j] = '\0';
    else if (out_cap > 0) out[out_cap - 1] = '\0';
}

/* ============================================================================
 * WebSocket handshake
 * ============================================================================ */

/* RFC 6455 magic GUID for Sec-WebSocket-Accept */
static const char* CD_WS_GUID = "258EAFA5-E914-47DA-95CA-5AB5DC76B97E";

/* Extract a header value from HTTP request text.
 * Returns pointer into buf (not a copy) or NULL if not found. */
static const char* cd_ws_find_header(const char* buf, const char* header_name) {
    const char* p = buf;
    size_t hlen = strlen(header_name);

    while (*p) {
        /* Find start of line */
        if (strncmp(p, header_name, hlen) == 0 && p[hlen] == ':') {
            p += hlen + 1;
            while (*p == ' ') p++;
            return p;
        }
        /* Advance to next line */
        const char* nl = strstr(p, "\r\n");
        if (nl == NULL) break;
        p = nl + 2;
    }
    return NULL;
}

/* Copy a header value up to \r\n into dst. Returns false if not found. */
static bool cd_ws_get_header(const char* buf, const char* header_name,
                              char* dst, size_t dst_cap) {
    const char* val = cd_ws_find_header(buf, header_name);
    if (val == NULL) return false;

    size_t i = 0;
    while (val[i] && val[i] != '\r' && val[i] != '\n' && i < dst_cap - 1) {
        dst[i] = val[i];
        i++;
    }
    dst[i] = '\0';
    return true;
}

/* Perform the WebSocket handshake. Returns true on success. */
static bool cd_ws_do_handshake(cd_mcp_ws_client_t* client,
                                const char* allowed_origin) {
    /* Verify it's a GET request with the upgrade headers */
    if (strncmp(client->read_buf, "GET ", 4) != 0) {
        return false;
    }

    /* Check required headers */
    char ws_key[128] = {0};
    char upgrade[32] = {0};

    if (!cd_ws_get_header(client->read_buf, "Sec-WebSocket-Key", ws_key, sizeof(ws_key))) {
        return false;
    }
    if (!cd_ws_get_header(client->read_buf, "Upgrade", upgrade, sizeof(upgrade))) {
        return false;
    }

    /* Upgrade header must contain "websocket" (case-insensitive) */
    bool has_ws = false;
    for (size_t i = 0; upgrade[i]; i++) {
        char c = upgrade[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        upgrade[i] = c;
    }
    has_ws = (strstr(upgrade, "websocket") != NULL);
    if (!has_ws) return false;

    /* Check Origin if configured */
    if (allowed_origin != NULL) {
        char origin[256] = {0};
        if (cd_ws_get_header(client->read_buf, "Origin", origin, sizeof(origin))) {
            if (strcmp(origin, allowed_origin) != 0) {
                /* Origin mismatch — send 403 */
                const char* resp = "HTTP/1.1 403 Forbidden\r\n\r\n";
                send(client->sock, resp, (int)strlen(resp), 0);
                return false;
            }
        }
    }

    /* Compute Sec-WebSocket-Accept:
     * SHA1(Sec-WebSocket-Key + GUID) → base64 */
    char concat[256];
    snprintf(concat, sizeof(concat), "%s%s", ws_key, CD_WS_GUID);

    cd_sha1_ctx_t sha;
    cd_sha1_init(&sha);
    cd_sha1_update(&sha, (const uint8_t*)concat, strlen(concat));
    uint8_t digest[20];
    cd_sha1_final(&sha, digest);

    char accept_b64[64];
    cd_base64_encode(digest, 20, accept_b64, sizeof(accept_b64));

    /* Build HTTP 101 response */
    char response[512];
    snprintf(response, sizeof(response),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n"
             "\r\n",
             accept_b64);

    send(client->sock, response, (int)strlen(response), 0);
    return true;
}

/* ============================================================================
 * WebSocket frame constants
 * ============================================================================ */

#define CD_WS_OP_CONTINUATION 0x0
#define CD_WS_OP_TEXT         0x1
#define CD_WS_OP_BINARY       0x2
#define CD_WS_OP_CLOSE        0x8
#define CD_WS_OP_PING         0x9
#define CD_WS_OP_PONG         0xA

/* ============================================================================
 * WebSocket frame sending (server-to-client, unmasked)
 * ============================================================================ */

static void cd_ws_send_frame(cd_ws_socket_t sock, uint8_t opcode,
                              const uint8_t* payload, size_t len) {
    uint8_t header[10];
    size_t header_len = 2;

    header[0] = 0x80 | (opcode & 0x0F); /* FIN + opcode */

    if (len < 126) {
        header[1] = (uint8_t)len;
    } else if (len <= 0xFFFF) {
        header[1] = 126;
        header[2] = (uint8_t)(len >> 8);
        header[3] = (uint8_t)(len & 0xFF);
        header_len = 4;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; i++) {
            header[2 + i] = (uint8_t)((len >> (56 - i * 8)) & 0xFF);
        }
        header_len = 10;
    }

    send(sock, (const char*)header, (int)header_len, 0);
    if (len > 0 && payload != NULL) {
        send(sock, (const char*)payload, (int)len, 0);
    }
}

static void cd_ws_send_text(cd_ws_socket_t sock, const char* text) {
    cd_ws_send_frame(sock, CD_WS_OP_TEXT,
                      (const uint8_t*)text, strlen(text));
}

static void cd_ws_send_close(cd_ws_socket_t sock) {
    cd_ws_send_frame(sock, CD_WS_OP_CLOSE, NULL, 0);
}

/* ============================================================================
 * WebSocket frame parsing (client-to-server, masked)
 *
 * Returns the number of bytes consumed from buf, or 0 if incomplete frame.
 * On success, writes the unmasked payload to out_payload (null-terminated)
 * and the opcode to out_opcode.
 * ============================================================================ */

static size_t cd_ws_parse_frame(const uint8_t* buf, size_t buf_len,
                                 char* out_payload, size_t out_cap,
                                 uint8_t* out_opcode) {
    if (buf_len < 2) return 0;

    uint8_t opcode = buf[0] & 0x0F;
    bool masked = (buf[1] & 0x80) != 0;
    uint64_t payload_len = buf[1] & 0x7F;
    size_t offset = 2;

    if (payload_len == 126) {
        if (buf_len < 4) return 0;
        payload_len = ((uint64_t)buf[2] << 8) | buf[3];
        offset = 4;
    } else if (payload_len == 127) {
        if (buf_len < 10) return 0;
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | buf[2 + i];
        }
        offset = 10;
    }

    /* Reject oversized frames */
    if (payload_len > CD_MCP_WS_MAX_FRAME_SIZE) {
        *out_opcode = CD_WS_OP_CLOSE;
        if (out_cap > 0) out_payload[0] = '\0';
        return buf_len; /* consume everything to trigger disconnect */
    }

    uint8_t mask_key[4] = {0};
    if (masked) {
        if (buf_len < offset + 4) return 0;
        memcpy(mask_key, buf + offset, 4);
        offset += 4;
    }

    if (buf_len < offset + payload_len) return 0; /* incomplete */

    /* Unmask and copy payload */
    size_t copy_len = (payload_len < out_cap - 1) ? (size_t)payload_len : out_cap - 1;
    for (size_t i = 0; i < copy_len; i++) {
        out_payload[i] = (char)(buf[offset + i] ^ (masked ? mask_key[i % 4] : 0));
    }
    out_payload[copy_len] = '\0';

    *out_opcode = opcode;
    return (size_t)(offset + payload_len);
}

/* ============================================================================
 * Per-client output callback (same pattern as TCP transport)
 * ============================================================================ */

typedef struct {
    cd_ws_socket_t sock;
} cd_ws_output_ctx_t;

static void cd_ws_output_fn(const char* json_line, void* user_data) {
    cd_ws_output_ctx_t* ctx = (cd_ws_output_ctx_t*)user_data;
    if (ctx == NULL || ctx->sock == CD_WS_INVALID_SOCKET) {
        return;
    }
    cd_ws_send_text(ctx->sock, json_line);
}

/* ============================================================================
 * Internal: disconnect a client
 * ============================================================================ */

static void cd_ws_disconnect_client(cd_mcp_ws_client_t* client) {
    cd_ws_close_socket(client->sock);
    client->sock     = CD_WS_INVALID_SOCKET;
    client->read_pos = 0;
    client->active   = false;
    client->state    = CD_WS_STATE_HTTP_HANDSHAKE;
}

/* ============================================================================
 * Internal: process data from a client
 * ============================================================================ */

static bool cd_ws_process_client(cd_mcp_ws_client_t* client,
                                  cd_mcp_server_t* mcp,
                                  struct cd_kernel_t* kernel,
                                  const char* allowed_origin,
                                  int client_index) {
    /* HTTP handshake phase */
    if (client->state == CD_WS_STATE_HTTP_HANDSHAKE) {
        /* Check if we have the complete HTTP request (ends with \r\n\r\n) */
        client->read_buf[client->read_pos] = '\0';
        if (strstr(client->read_buf, "\r\n\r\n") == NULL) {
            return false; /* incomplete, wait for more data */
        }

        if (!cd_ws_do_handshake(client, allowed_origin)) {
            return true; /* disconnect */
        }

        client->state = CD_WS_STATE_OPEN;
        client->read_pos = 0;
        return false;
    }

    /* WebSocket frame processing */
    if (client->state != CD_WS_STATE_OPEN) {
        return false;
    }

    /* Set the current agent for per-connection txn isolation */
    uint32_t agent_id = (uint32_t)(CD_MCP_AGENT_WS_BASE + client_index);
    cd_mcp_set_current_agent(agent_id);

    /* Save and redirect MCP output */
    cd_mcp_output_fn_t saved_fn   = mcp->output_fn;
    void*              saved_data = mcp->output_user_data;

    cd_ws_output_ctx_t ctx;
    ctx.sock = client->sock;
    mcp->output_fn        = cd_ws_output_fn;
    mcp->output_user_data = &ctx;

    bool should_disconnect = false;

    /* Process all complete frames in the buffer */
    while (client->read_pos > 0) {
        char payload[CD_MCP_WS_CLIENT_BUF_SIZE];
        uint8_t opcode = 0;

        size_t consumed = cd_ws_parse_frame(
            (const uint8_t*)client->read_buf, client->read_pos,
            payload, sizeof(payload), &opcode);

        if (consumed == 0) break; /* incomplete frame */

        switch (opcode) {
            case CD_WS_OP_TEXT:
                /* Dispatch JSON-RPC request */
                if (payload[0] != '\0') {
                    cd_mcp_process_request(mcp, kernel, payload);
                }
                break;

            case CD_WS_OP_PING:
                /* Respond with pong (echo payload) */
                cd_ws_send_frame(client->sock, CD_WS_OP_PONG,
                                  (const uint8_t*)payload, strlen(payload));
                break;

            case CD_WS_OP_CLOSE:
                /* Send close frame back and disconnect */
                cd_ws_send_close(client->sock);
                should_disconnect = true;
                break;

            case CD_WS_OP_PONG:
                /* Ignore unsolicited pongs */
                break;

            default:
                break;
        }

        if (should_disconnect) break;

        /* Shift remaining data */
        uint32_t remaining = client->read_pos - (uint32_t)consumed;
        if (remaining > 0) {
            memmove(client->read_buf, client->read_buf + consumed, remaining);
        }
        client->read_pos = remaining;
    }

    /* Restore MCP output */
    mcp->output_fn        = saved_fn;
    mcp->output_user_data = saved_data;

    return should_disconnect;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

cd_result_t cd_mcp_ws_init(cd_mcp_ws_server_t* ws, uint16_t port) {
    if (ws == NULL) return CD_ERR_NULL;

    memset(ws, 0, sizeof(cd_mcp_ws_server_t));
    ws->listen_sock = CD_WS_INVALID_SOCKET;

    for (int i = 0; i < CD_MCP_WS_MAX_CLIENTS; i++) {
        ws->clients[i].sock   = CD_WS_INVALID_SOCKET;
        ws->clients[i].active = false;
        ws->clients[i].state  = CD_WS_STATE_HTTP_HANDSHAKE;
    }

    if (!cd_ws_platform_init()) return CD_ERR_IO;

    cd_ws_socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == CD_WS_INVALID_SOCKET) return CD_ERR_IO;

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    if (!cd_ws_set_nonblocking(sock)) {
        cd_ws_close_socket(sock);
        return CD_ERR_IO;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        cd_ws_close_socket(sock);
        return CD_ERR_IO;
    }

    struct sockaddr_in bound_addr;
    int addr_len = (int)sizeof(bound_addr);
    if (getsockname(sock, (struct sockaddr*)&bound_addr, &addr_len) == 0) {
        ws->port = ntohs(bound_addr.sin_port);
    } else {
        ws->port = port;
    }

    if (listen(sock, CD_MCP_WS_MAX_CLIENTS) != 0) {
        cd_ws_close_socket(sock);
        return CD_ERR_IO;
    }

    ws->listen_sock = sock;
    ws->initialized = true;
    return CD_OK;
}

cd_result_t cd_mcp_ws_poll(cd_mcp_ws_server_t* ws,
                             cd_mcp_server_t* mcp,
                             struct cd_kernel_t* kernel) {
    if (ws == NULL || mcp == NULL) return CD_ERR_NULL;
    if (!ws->initialized) return CD_ERR_INVALID;

    fd_set read_fds;
    FD_ZERO(&read_fds);

    FD_SET(ws->listen_sock, &read_fds);
    cd_ws_socket_t max_fd = ws->listen_sock;

    for (int i = 0; i < CD_MCP_WS_MAX_CLIENTS; i++) {
        if (ws->clients[i].active) {
            FD_SET(ws->clients[i].sock, &read_fds);
            if (ws->clients[i].sock > max_fd) {
                max_fd = ws->clients[i].sock;
            }
        }
    }

    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 0;

    int ready = select((int)(max_fd + 1), &read_fds, NULL, NULL, &tv);
    if (ready <= 0) return CD_OK;

    /* Accept pending connections */
    if (FD_ISSET(ws->listen_sock, &read_fds)) {
        for (;;) {
            struct sockaddr_in client_addr;
            int client_addr_len = (int)sizeof(client_addr);
            cd_ws_socket_t client_sock = accept(ws->listen_sock,
                                                  (struct sockaddr*)&client_addr,
                                                  &client_addr_len);
            if (client_sock == CD_WS_INVALID_SOCKET) break;

            bool placed = false;
            for (int i = 0; i < CD_MCP_WS_MAX_CLIENTS; i++) {
                if (!ws->clients[i].active) {
                    cd_ws_set_nonblocking(client_sock);
                    ws->clients[i].sock     = client_sock;
                    ws->clients[i].read_pos = 0;
                    ws->clients[i].active   = true;
                    ws->clients[i].state    = CD_WS_STATE_HTTP_HANDSHAKE;
                    memset(ws->clients[i].read_buf, 0,
                           CD_MCP_WS_CLIENT_BUF_SIZE);
                    placed = true;
                    break;
                }
            }

            if (!placed) {
                cd_ws_close_socket(client_sock);
            }
        }
    }

    /* Read from active clients */
    for (int i = 0; i < CD_MCP_WS_MAX_CLIENTS; i++) {
        if (!ws->clients[i].active) continue;
        if (!FD_ISSET(ws->clients[i].sock, &read_fds)) continue;

        cd_mcp_ws_client_t* client = &ws->clients[i];
        uint32_t space = CD_MCP_WS_CLIENT_BUF_SIZE - client->read_pos - 1;

        if (space == 0) {
            cd_ws_disconnect_client(client);
            continue;
        }

        int n = recv(client->sock,
                     client->read_buf + client->read_pos,
                     (int)space, 0);

        if (n <= 0) {
            if (n < 0 && CD_WS_WOULD_BLOCK(cd_ws_get_error())) {
                continue;
            }
            cd_ws_disconnect_client(client);
            continue;
        }

        client->read_pos += (uint32_t)n;

        if (cd_ws_process_client(client, mcp, kernel, ws->allowed_origin, i)) {
            cd_ws_disconnect_client(client);
        }
    }

    return CD_OK;
}

void cd_mcp_ws_shutdown(cd_mcp_ws_server_t* ws) {
    if (ws == NULL) return;

    for (int i = 0; i < CD_MCP_WS_MAX_CLIENTS; i++) {
        if (ws->clients[i].active) {
            if (ws->clients[i].state == CD_WS_STATE_OPEN) {
                cd_ws_send_close(ws->clients[i].sock);
            }
            cd_ws_disconnect_client(&ws->clients[i]);
        }
    }

    cd_ws_close_socket(ws->listen_sock);
    ws->listen_sock = CD_WS_INVALID_SOCKET;
    ws->initialized = false;
}

void cd_mcp_ws_set_allowed_origin(cd_mcp_ws_server_t* ws, const char* origin) {
    if (ws != NULL) {
        ws->allowed_origin = origin;
    }
}

uint32_t cd_mcp_ws_client_count(const cd_mcp_ws_server_t* ws) {
    if (ws == NULL) return 0;

    uint32_t count = 0;
    for (int i = 0; i < CD_MCP_WS_MAX_CLIENTS; i++) {
        if (ws->clients[i].active) count++;
    }
    return count;
}
