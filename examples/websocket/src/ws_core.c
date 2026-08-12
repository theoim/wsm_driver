/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * RFC 6455 handshake and framing, ported from mWebSockets.
 *
 *   mWebSockets - https://github.com/skaarj1989/mWebSockets
 *   Copyright (c) 2017-2024 Dawid Kurek, MIT License.
 *
 * See ws_core.h for what the port changed and why.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "mbedtls/base64.h"
/*
 * SHA-1 through PSA rather than mbedtls/sha1.h. mbedTLS 4.0 (ESP-IDF v6) moved
 * the low-level hash modules behind mbedtls/private/, so the supported way to
 * ask for a one-shot digest is psa_hash_compute(). Base64 stayed public.
 *
 * SHA-1 is weak for signatures, and using it here is not a judgement about
 * that: RFC 6455 section 1.3 fixes it as the accept-key derivation, and the
 * value is a handshake token with no secrecy requirement.
 */
#include "psa/crypto.h"

#include "ws_core.h"
#include "ws_transport.h"

static const char *TAG = "ws";

/* A client that has begun a frame owes us the rest of it. Long enough to ride
 * out a slow link, short enough that a vanished peer does not pin the task. */
#define WS_FRAME_TIMEOUT_MS   3000

/* The handshake request arrives in one burst right after connect. */
#define WS_HANDSHAKE_TIMEOUT_MS 3000

/* Longest header line worth keeping. Browser User-Agent runs to ~145
 * characters (Opera is the worst offender), and anything longer is a header we
 * do not read anyway. */
#define WS_LINE_MAX           192

/* ---- helpers ------------------------------------------------------------ */

static bool is_control_frame(uint8_t opcode)
{
    return opcode == WS_OP_PING || opcode == WS_OP_PONG || opcode == WS_OP_CLOSE;
}

/*
 * UTF-8 validation for text frames, from utf8_check.c by Markus Kuhn
 * (https://www.cl.cam.ac.uk/~mgk25/ucs/utf8_check.c), the same source
 * mWebSockets uses. RFC 6455 section 8.1 requires a text frame carrying
 * invalid UTF-8 to fail the connection, and the Autobahn suite tests it.
 */
static bool is_valid_utf8(const uint8_t *s, size_t length)
{
    const uint8_t *end = s + length;

    while (s < end) {
        if (*s < 0x80) {                                /* 0xxxxxxx */
            s++;
        } else if ((s[0] & 0xE0) == 0xC0) {             /* 110xxxxx 10xxxxxx */
            if (s + 1 == end || (s[1] & 0xC0) != 0x80 ||
                (s[0] & 0xFE) == 0xC0) {                /* overlong */
                break;
            }
            s += 2;
        } else if ((s[0] & 0xF0) == 0xE0) {             /* 1110xxxx 10xx 10xx */
            if (s + 2 >= end || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 ||
                (s[0] == 0xE0 && (s[1] & 0xE0) == 0x80) ||
                (s[0] == 0xED && (s[1] & 0xE0) == 0xA0)) {
                break;
            }
            s += 3;
        } else if ((s[0] & 0xF8) == 0xF0) {             /* 11110xxx 10xx*3 */
            if (s + 3 >= end || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 ||
                (s[3] & 0xC0) != 0x80 ||
                (s[0] == 0xF0 && (s[1] & 0xF0) == 0x80) ||   /* overlong */
                (s[0] == 0xF4 && s[1] > 0x8F) || s[0] > 0xF4) { /* > U+10FFFF */
                break;
            }
            s += 4;
        } else {
            break;
        }
    }
    return s == end;
}

/* ---- handshake ---------------------------------------------------------- */

int ws_accept_key(const char *client_key, char out[29])
{
    /* RFC 6455 section 1.3: SHA-1 of the key concatenated with this GUID,
     * Base64-encoded. The GUID is fixed by the spec, not a secret. */
    static const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    char concat[24 + sizeof(kGuid)];
    size_t key_len = strlen(client_key);

    /* A conforming key is 24 Base64 characters. Anything else is malformed and
     * would silently produce a wrong accept value, so reject it here. */
    if (key_len != 24) {
        return -1;
    }
    memcpy(concat, client_key, 24);
    memcpy(concat + 24, kGuid, sizeof(kGuid) - 1);

    /* Idempotent, and cheap after the first call -- so the caller does not have
     * to remember to initialise PSA before the first handshake. */
    if (psa_crypto_init() != PSA_SUCCESS) {
        return -1;
    }

    uint8_t digest[20];
    size_t digest_len = 0;
    if (psa_hash_compute(PSA_ALG_SHA_1, (const uint8_t *)concat,
                         24 + sizeof(kGuid) - 1,
                         digest, sizeof(digest), &digest_len) != PSA_SUCCESS ||
        digest_len != sizeof(digest)) {
        return -1;
    }

    size_t written = 0;
    if (mbedtls_base64_encode((unsigned char *)out, 29, &written,
                              digest, sizeof(digest)) != 0) {
        return -1;
    }
    out[written] = '\0';
    return 0;
}

/* ---- buffered reads ------------------------------------------------------ */

/* Stage more bytes when the buffer has run dry. Returns 1 on data, 0 on
 * timeout, -1 if the peer closed or the socket failed. */
static int rx_fill(ws_conn_t *conn, uint32_t timeout_ms)
{
    if (conn->rx_pos < conn->rx_len) {
        return 1;                       /* still holding something */
    }

    int n = ws_transport_recv(conn->ops, conn->fd, conn->rx, sizeof(conn->rx), timeout_ms);
    if (n <= 0) {
        return n;                       /* 0 timeout, -1 closed/failed */
    }
    conn->rx_len = (size_t)n;
    conn->rx_pos = 0;
    return 1;
}

/* One byte. Returns it, or -1 on timeout/close/failure. */
static int rx_getc(ws_conn_t *conn, uint32_t timeout_ms)
{
    int r = rx_fill(conn, timeout_ms);
    if (r <= 0) {
        return -1;
    }
    return conn->rx[conn->rx_pos++];
}

/* Exactly `size` bytes, looping until they arrive. A short read mid-frame
 * desynchronises the parser, so this is what the framing code uses rather than
 * a best-effort read. Returns 0 on success, -1 otherwise. */
static int rx_read(ws_conn_t *conn, void *dst, size_t size, uint32_t timeout_ms)
{
    uint8_t *p = (uint8_t *)dst;
    size_t got = 0;

    while (got < size) {
        int r = rx_fill(conn, timeout_ms);
        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            ESP_LOGW(TAG, "timed out %u bytes into a %u-byte read",
                     (unsigned)got, (unsigned)size);
            return -1;
        }
        size_t avail = conn->rx_len - conn->rx_pos;
        size_t take = (avail < size - got) ? avail : size - got;
        memcpy(p + got, &conn->rx[conn->rx_pos], take);
        conn->rx_pos += take;
        got += take;
    }
    return 0;
}

/* ---- handshake ---------------------------------------------------------- */

/* Read one CRLF-terminated line into `line`. Returns its length without the
 * terminator, or -1 on failure. Byte-at-a-time against the staging buffer, so
 * this is memory work rather than one socket round trip per character. */
static int read_line(ws_conn_t *conn, char *line, size_t size)
{
    size_t n = 0;

    for (;;) {
        int ch = rx_getc(conn, WS_HANDSHAKE_TIMEOUT_MS);
        if (ch < 0) {
            return -1;
        }
        char c = (char)ch;
        if (c == '\n') {
            while (n > 0 && line[n - 1] == '\r') {
                n--;
            }
            line[n] = '\0';
            return (int)n;
        }
        /* Overlong headers are dropped rather than fatal -- a long User-Agent
         * is not a protocol error, and we do not read that header anyway. */
        if (n < size - 1) {
            line[n++] = c;
        }
    }
}

static void send_status(ws_conn_t *conn, const char *status_line)
{
    char buf[96];
    int n = snprintf(buf, sizeof(buf), "%s\r\nConnection: close\r\n\r\n",
                     status_line);
    if (n > 0) {
        ws_transport_send(conn->ops, conn->fd, buf, (size_t)n);
    }
}

int ws_http_respond(ws_conn_t *conn, const char *status, const char *content_type,
                    const char *body, size_t body_len)
{
    char header[160];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %u\r\n"
                     "Connection: close\r\n\r\n",
                     status, content_type, (unsigned)body_len);
    if (n <= 0 || ws_transport_send(conn->ops, conn->fd, header, (size_t)n) < 0) {
        return -1;
    }
    if (body_len > 0 && ws_transport_send(conn->ops, conn->fd, body, body_len) < 0) {
        return -1;
    }
    return 0;
}

ws_request_t ws_read_request(ws_conn_t *conn)
{
    char line[WS_LINE_MAX];
    char sec_key[32] = {0};
    bool have_upgrade = false, have_connection = false, have_version = false;
    int line_no = 0;

    conn->path[0] = '\0';

    for (;;) {
        int len = read_line(conn, line, sizeof(line));
        if (len < 0) {
            ESP_LOGW(TAG, "request: connection died mid-request");
            return WS_REQ_FAILED;
        }

        if (line_no == 0) {
            /* "GET /path HTTP/1.1". Only GET is served; a browser asking for
             * anything else is not a case this example needs to cover. */
            if (strncmp(line, "GET ", 4) != 0 ||
                strstr(line, "HTTP/1.1") == NULL) {
                ESP_LOGW(TAG, "request: not a GET/1.1 request: \"%s\"", line);
                send_status(conn, "HTTP/1.1 400 Bad Request");
                return WS_REQ_FAILED;
            }
            /* Keep the target: it is what tells / apart from /ws. */
            const char *p = line + 4;
            size_t i = 0;
            while (p[i] && p[i] != ' ' && i < sizeof(conn->path) - 1) {
                conn->path[i] = p[i];
                i++;
            }
            conn->path[i] = '\0';
            line_no++;
            continue;
        }

        if (len == 0) {
            break;                      /* blank line ends the request */
        }

        char *colon = strchr(line, ':');
        if (colon == NULL) {
            line_no++;
            continue;                   /* not a header; ignore */
        }
        *colon = '\0';
        char *value = colon + 1;
        while (*value == ' ' || *value == '	') {
            value++;
        }

        if (strcasecmp(line, "Upgrade") == 0) {
            have_upgrade = (strcasecmp(value, "websocket") == 0);
        } else if (strcasecmp(line, "Connection") == 0) {
            /* Firefox sends "keep-alive, Upgrade", so this is a substring test
             * rather than an equality one. */
            have_connection = (strcasestr(value, "Upgrade") != NULL);
        } else if (strcasecmp(line, "Sec-WebSocket-Key") == 0) {
            snprintf(sec_key, sizeof(sec_key), "%s", value);
        } else if (strcasecmp(line, "Sec-WebSocket-Version") == 0) {
            int v = atoi(value);
            have_version = (v == 13 || v == 8);
        }
        line_no++;
    }

    /* No upgrade asked for: an ordinary page request. Not an error -- this is
     * the browser's first visit, before any script has run. */
    if (!have_upgrade || !have_connection) {
        ESP_LOGI(TAG, "plain HTTP request for \"%s\"", conn->path);
        return WS_REQ_PLAIN_HTTP;
    }

    if (!have_version || sec_key[0] == '\0') {
        ESP_LOGW(TAG, "handshake: bad version or missing key");
        send_status(conn, "HTTP/1.1 400 Bad Request");
        return WS_REQ_FAILED;
    }

    char accept[29];
    if (ws_accept_key(sec_key, accept) != 0) {
        ESP_LOGW(TAG, "handshake: cannot derive accept key from \"%s\"", sec_key);
        send_status(conn, "HTTP/1.1 400 Bad Request");
        return WS_REQ_FAILED;
    }

    char response[160];
    int n = snprintf(response, sizeof(response),
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n",
                     accept);
    if (n <= 0 || ws_transport_send(conn->ops, conn->fd, response, (size_t)n) < 0) {
        return WS_REQ_FAILED;
    }

    conn->open = true;
    conn->offset = 0;
    conn->frag_opcode = -1;
    ESP_LOGI(TAG, "handshake complete (%s)", conn->path);
    return WS_REQ_UPGRADED;
}

/* ---- framing ------------------------------------------------------------ */

typedef struct {
    bool     fin;
    uint8_t  opcode;
    bool     mask;
    uint8_t  masking_key[4];
    uint32_t length;
} ws_header_t;

int ws_send(ws_conn_t *conn, ws_data_type_t type, const char *data, size_t len)
{
    if (!conn->open) {
        return -1;
    }

    /* A server must not mask (RFC 6455 section 5.1), so the header is 2 bytes
     * for payloads under 126 and 4 for anything up to 64 KB. */
    uint8_t header[4];
    size_t hlen;

    header[0] = 0x80 | (uint8_t)(type == WS_DATA_TEXT ? WS_OP_TEXT : WS_OP_BINARY);
    if (len <= 125) {
        header[1] = (uint8_t)len;
        hlen = 2;
    } else if (len <= 0xFFFF) {
        header[1] = 126;
        header[2] = (uint8_t)(len >> 8);
        header[3] = (uint8_t)(len & 0xFF);
        hlen = 4;
    } else {
        ESP_LOGE(TAG, "message of %u bytes is too large to send", (unsigned)len);
        return -1;
    }

    if (ws_transport_send(conn->ops, conn->fd, header, hlen) < 0) {
        return -1;
    }
    if (len > 0 && ws_transport_send(conn->ops, conn->fd, data, len) < 0) {
        return -1;
    }
    return 0;
}

void ws_close(ws_conn_t *conn, ws_close_code_t code, const char *reason)
{
    if (!conn->open) {
        return;
    }
    conn->open = false;

    size_t rlen = reason ? strlen(reason) : 0;
    if (rlen > 123) {
        rlen = 123;                     /* control frames cap at 125 bytes */
    }

    uint8_t frame[2 + 2 + 123];
    frame[0] = 0x80 | WS_OP_CLOSE;
    frame[1] = (uint8_t)(2 + rlen);
    frame[2] = (uint8_t)(code >> 8);
    frame[3] = (uint8_t)(code & 0xFF);
    if (rlen) {
        memcpy(&frame[4], reason, rlen);
    }
    ws_transport_send(conn->ops, conn->fd, frame, 4 + rlen);
}

/*
 * Finish reading a header whose first byte the caller already has.
 *
 * ws_poll() reads that byte on the caller's idle timeout so a quiet connection
 * costs nothing, then hands it here; everything after it is owed on the shorter
 * frame timeout, because a client that has started a frame is mid-transmission.
 */
static int read_header(ws_conn_t *conn, uint8_t first, ws_header_t *h,
                       uint32_t timeout_ms)
{
    uint8_t b[2] = { first, 0 };
    if (rx_read(conn, &b[1], 1, timeout_ms) < 0) {
        return -1;
    }

    h->fin    = (b[0] & 0x80) != 0;
    h->opcode = b[0] & 0x0F;
    h->mask   = (b[1] & 0x80) != 0;
    h->length = b[1] & 0x7F;

    if (b[0] & 0x70) {                  /* RSV1..3 must be clear */
        ESP_LOGW(TAG, "reserved bits set in frame header");
        ws_close(conn, WS_CLOSE_PROTOCOL_ERROR, NULL);
        return -1;
    }
    if (is_control_frame(h->opcode) && (!h->fin || h->length > 125)) {
        ESP_LOGW(TAG, "control frame fragmented or oversized");
        ws_close(conn, WS_CLOSE_PROTOCOL_ERROR, NULL);
        return -1;
    }

    if (h->length == 126) {
        uint8_t ext[2];
        if (rx_read(conn, ext, 2, timeout_ms) < 0) {
            return -1;
        }
        h->length = ((uint32_t)ext[0] << 8) | ext[1];
    } else if (h->length == 127) {
        /* 64-bit lengths mean payloads past 64 KB. Nothing on a chip with 2 KB
         * socket buffers wants to assemble one, so refuse rather than pretend. */
        ESP_LOGW(TAG, "64-bit frame length is not supported");
        ws_close(conn, WS_CLOSE_MESSAGE_TOO_BIG, NULL);
        return -1;
    }

    /* RFC 6455 section 5.1: every client-to-server frame must be masked. */
    if (!h->mask) {
        ESP_LOGW(TAG, "client sent an unmasked frame");
        ws_close(conn, WS_CLOSE_PROTOCOL_ERROR, NULL);
        return -1;
    }
    if (rx_read(conn, h->masking_key, 4, timeout_ms) < 0) {
        return -1;
    }
    return 0;
}

static void unmask(uint8_t *payload, size_t len, const uint8_t key[4])
{
    for (size_t i = 0; i < len; i++) {
        payload[i] ^= key[i & 3];
    }
}

static void deliver(ws_conn_t *conn, uint8_t opcode, size_t length)
{
    ws_data_type_t type =
        (opcode == WS_OP_TEXT) ? WS_DATA_TEXT : WS_DATA_BINARY;

    if (type == WS_DATA_TEXT &&
        !is_valid_utf8((const uint8_t *)conn->buffer, length)) {
        ESP_LOGW(TAG, "text frame carried invalid UTF-8");
        ws_close(conn, WS_CLOSE_INVALID_PAYLOAD, NULL);
        return;
    }
    if (conn->on_message) {
        conn->on_message(conn, type, conn->buffer, length);
    }
    conn->offset = 0;
    conn->frag_opcode = -1;
}

int ws_poll(ws_conn_t *conn, uint32_t timeout_ms)
{
    if (!conn->open) {
        return -1;
    }

    /* Read only the first byte on the caller's timeout: an idle connection
     * should cost a poll, not a stalled task. */
    int r = rx_fill(conn, timeout_ms);
    if (r == 0) {
        return 0;                       /* idle */
    }
    if (r < 0) {
        conn->open = false;
        return -1;
    }
    uint8_t first = conn->rx[conn->rx_pos++];

    ws_header_t h;
    if (read_header(conn, first, &h, WS_FRAME_TIMEOUT_MS) < 0) {
        conn->open = false;
        return -1;
    }

    /* Control frames get their own small buffer so they can arrive in the
     * middle of a fragmented message without trampling what is assembled so
     * far -- which RFC 6455 section 5.4 explicitly allows. */
    uint8_t control[125];
    uint8_t *payload;
    size_t   at;

    if (is_control_frame(h.opcode)) {
        payload = control;
        at = 0;
    } else {
        if (conn->offset + h.length > conn->buffer_size) {
            ESP_LOGW(TAG, "message exceeds the %u-byte buffer",
                     (unsigned)conn->buffer_size);
            ws_close(conn, WS_CLOSE_MESSAGE_TOO_BIG, NULL);
            return -1;
        }
        payload = (uint8_t *)conn->buffer;
        at = conn->offset;
    }

    if (h.length > 0) {
        if (rx_read(conn, payload + at, h.length, WS_FRAME_TIMEOUT_MS) < 0) {
            conn->open = false;
            return -1;
        }
        unmask(payload + at, h.length, h.masking_key);
    }

    switch (h.opcode) {
    case WS_OP_CONTINUATION:
        if (conn->frag_opcode < 0) {
            ESP_LOGW(TAG, "continuation frame with nothing to continue");
            ws_close(conn, WS_CLOSE_PROTOCOL_ERROR, NULL);
            return -1;
        }
        conn->offset += h.length;
        if (h.fin) {
            deliver(conn, (uint8_t)conn->frag_opcode, conn->offset);
        }
        break;

    case WS_OP_TEXT:
    case WS_OP_BINARY:
        if (conn->offset > 0) {
            ESP_LOGW(TAG, "new data frame while a message was still open");
            ws_close(conn, WS_CLOSE_PROTOCOL_ERROR, NULL);
            return -1;
        }
        if (h.fin) {
            deliver(conn, h.opcode, h.length);
        } else {
            conn->offset = h.length;
            conn->frag_opcode = (int8_t)h.opcode;
        }
        break;

    case WS_OP_PING:
        /* Answer with the same payload, per RFC 6455 section 5.5.3. */
        {
            uint8_t pong[2 + 125];
            pong[0] = 0x80 | WS_OP_PONG;
            pong[1] = (uint8_t)h.length;
            if (h.length) {
                memcpy(&pong[2], control, h.length);
            }
            ws_transport_send(conn->ops, conn->fd, pong, 2 + h.length);
        }
        break;

    case WS_OP_PONG:
        break;                          /* unsolicited pong; nothing owed */

    case WS_OP_CLOSE: {
        uint16_t code = WS_CLOSE_NORMAL;
        if (h.length >= 2) {
            code = (uint16_t)((control[0] << 8) | control[1]);
        }
        ESP_LOGI(TAG, "client closed with code %u", code);
        ws_close(conn, WS_CLOSE_NORMAL, NULL);
        return -1;
    }

    default:
        ESP_LOGW(TAG, "unknown opcode 0x%x", h.opcode);
        ws_close(conn, WS_CLOSE_PROTOCOL_ERROR, NULL);
        return -1;
    }

    return conn->open ? 0 : -1;
}
