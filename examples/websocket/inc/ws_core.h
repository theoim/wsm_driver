/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * RFC 6455 handshake and framing, ported from mWebSockets.
 *
 *   mWebSockets - https://github.com/skaarj1989/mWebSockets
 *   Copyright (c) 2017-2024 Dawid Kurek, MIT License.
 *
 * The upstream library is C++ built on Arduino's Client/Server classes. What
 * carried over is the protocol: the handshake validation, the accept-key
 * derivation, the frame layout, the control-frame rules, and the fragmentation
 * and UTF-8 handling. What changed:
 *
 *   - C rather than C++. The three classes flatten into one ws_conn_t; there
 *     was no depth to the hierarchy to lose.
 *   - Sockets reached through ws_transport.h instead of an Arduino Client.
 *   - Reads are bulk instead of byte-at-a-time. Arduino's Client has no
 *     read-with-timeout, so upstream calls read() per byte; over a hardware
 *     socket that is one SPI transaction per byte of every frame.
 *   - SHA-1 and Base64 come from mbedTLS, which ESP-IDF already links. That
 *     drops upstream's bundled CryptoLegacy and base64 (~2,200 lines) and
 *     leaves this file smaller than the original it came from.
 */
#ifndef WS_CORE_H
#define WS_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Frame opcodes (RFC 6455 section 5.2). */
typedef enum {
    WS_OP_CONTINUATION = 0x0,
    WS_OP_TEXT         = 0x1,
    WS_OP_BINARY       = 0x2,
    WS_OP_CLOSE        = 0x8,
    WS_OP_PING         = 0x9,
    WS_OP_PONG         = 0xA,
} ws_opcode_t;

/* Close codes the server sends (RFC 6455 section 7.4.1). */
typedef enum {
    WS_CLOSE_NORMAL          = 1000,
    WS_CLOSE_GOING_AWAY      = 1001,
    WS_CLOSE_PROTOCOL_ERROR  = 1002,
    WS_CLOSE_INVALID_PAYLOAD = 1007,
    WS_CLOSE_MESSAGE_TOO_BIG = 1009,
} ws_close_code_t;

/* What a message carried. */
typedef enum {
    WS_DATA_TEXT,
    WS_DATA_BINARY,
} ws_data_type_t;

typedef struct ws_conn ws_conn_t;

/*
 * Called once per complete message, after any fragments have been reassembled.
 * `message` is not NUL-terminated. Returning is enough; use ws_send() from
 * inside the callback to reply.
 */
typedef void (*ws_on_message_fn)(ws_conn_t *conn, ws_data_type_t type,
                                 const char *message, size_t length);

/*
 * Receive staging buffer.
 *
 * Every read -- handshake line, frame header, payload -- goes through this, for
 * two reasons. Speed: on the TOE a one-byte recv() is a full SPI transaction, so
 * reading a ~500-byte browser handshake a byte at a time cost 877 ms of pure
 * round-trip on a W5500. And correctness: because the handshake and the frame
 * parser share one buffer, bytes read past the blank line are simply still
 * there when the frame parser asks for them, instead of being lost.
 *
 * 256 bytes covers a header line with room to spare; the handshake refills a
 * couple of times rather than the several hundred a byte-at-a-time reader needs.
 */
#define WS_RX_BUFFER_SIZE 256

struct ws_conn {
    int              fd;
    bool             open;

    ws_on_message_fn on_message;
    void            *user;           /* opaque, for the application */

    char            *buffer;         /* message assembly, owned by the caller */
    size_t           buffer_size;
    size_t           offset;         /* bytes of a fragmented message so far */
    int8_t           frag_opcode;    /* opcode being continued, -1 when none */

    uint8_t          rx[WS_RX_BUFFER_SIZE];
    size_t           rx_len;         /* bytes staged */
    size_t           rx_pos;         /* bytes consumed */

    /* Request target of the last request read, for the plain-HTTP case. */
    char             path[64];
};

/* What the client's request turned out to be. */
typedef enum {
    WS_REQ_UPGRADED,      /* a WebSocket handshake; the connection is now open */
    WS_REQ_PLAIN_HTTP,    /* an ordinary GET; conn->path holds what was asked for */
    WS_REQ_FAILED,        /* malformed or rejected; an error response was sent */
} ws_request_t;

/*
 * Read and classify one HTTP request on a freshly accepted socket.
 *
 * A browser pointed at this device sends a plain GET before it ever opens a
 * WebSocket, so the two share a port and this is where they part company. When
 * the request carries Upgrade: websocket the handshake is completed here and
 * WS_REQ_UPGRADED is returned; otherwise the caller gets WS_REQ_PLAIN_HTTP and
 * the requested path, and decides what to serve.
 */
ws_request_t ws_read_request(ws_conn_t *conn);

/* Send a complete HTTP response and close nothing -- the caller owns the
 * socket. `content_type` is a MIME type, `body` need not be NUL-terminated. */
int ws_http_respond(ws_conn_t *conn, const char *status, const char *content_type,
                    const char *body, size_t body_len);

/* Derive Sec-WebSocket-Accept from a client's Sec-WebSocket-Key.
 * `out` needs 29 bytes: 28 Base64 characters plus a NUL. */
int ws_accept_key(const char *client_key, char out[29]);

/* Read and dispatch one frame. Handles ping, close, and fragmentation itself,
 * invoking on_message only for complete messages. Returns 0 when the connection
 * is still open, -1 once it has closed. */
int ws_poll(ws_conn_t *conn, uint32_t timeout_ms);

/* Send one unfragmented message. */
int ws_send(ws_conn_t *conn, ws_data_type_t type, const char *data, size_t len);

/* Send a close frame and mark the connection closed. `reason` may be NULL. */
void ws_close(ws_conn_t *conn, ws_close_code_t code, const char *reason);

#endif /* WS_CORE_H */
