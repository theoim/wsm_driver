/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Drives one WebSocket connection at a time: accept, handshake, echo, close,
 * then back to accepting.
 *
 * Backend-neutral -- everything network-facing goes through the vtable handed
 * to ws_server_start() and reaches the wire via ws_transport.c.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "net_config.h"
#include "ws_core.h"
#include "ws_index_html.h"
#include "ws_server.h"
#include "ws_transport.h"

static const char *TAG = "ws_server";

/* How long accept() and ws_poll() block before returning to the loop. Long
 * enough that an idle server is not spinning, short enough that a shutdown or
 * a log line is not stuck behind it. */
#define ACCEPT_TIMEOUT_MS  1000
#define POLL_TIMEOUT_MS    1000

typedef struct {
    const char *name;
    const void *ops;
    uint16_t    port;
    bool      (*is_up)(void);
} ws_server_ctx_t;

/* The greeting a browser sees the moment its onopen fires. */
static const char kGreeting[] = "WIZnet WSM Driver WebSocket server ready";

static void on_message(ws_conn_t *conn, ws_data_type_t type,
                       const char *message, size_t length)
{
    const char *label = (const char *)conn->user;

    if (type == WS_DATA_TEXT) {
        /* Log it as text. The payload is not NUL-terminated, hence the
         * precision specifier rather than a copy. */
        ESP_LOGI(TAG, "[%s] received %u bytes: %.*s", label,
                 (unsigned)length, (int)length, message);
    } else {
        ESP_LOGI(TAG, "[%s] received %u binary bytes", label, (unsigned)length);
    }

    /* Echo it back unchanged, same type. */
    if (ws_send(conn, type, message, length) < 0) {
        ESP_LOGW(TAG, "[%s] echo failed", label);
    }
}

static void serve_one(const char *name, const void *ops, int fd,
                      char *buffer, size_t buffer_size)
{
    ws_conn_t conn = {
        .ops         = ops,
        .fd          = fd,
        .open        = false,
        .on_message  = on_message,
        .user        = (void *)name,
        .buffer      = buffer,
        .buffer_size = buffer_size,
        .offset      = 0,
        .frag_opcode = -1,
    };

    switch (ws_read_request(&conn)) {
    case WS_REQ_UPGRADED:
        break;                          /* fall through to the session below */

    case WS_REQ_PLAIN_HTTP:
        /* The browser's first visit. Serve the page that will then open the
         * WebSocket, so a user only ever needs the device's address. */
        if (strcmp(conn.path, "/") == 0 || strcmp(conn.path, "/index.html") == 0) {
            ESP_LOGI(TAG, "[%s] serving the page", name);
            ws_http_respond(&conn, "200 OK", "text/html; charset=utf-8",
                            kIndexHtml, strlen(kIndexHtml));
        } else {
            ESP_LOGI(TAG, "[%s] no such path: %s", name, conn.path);
            ws_http_respond(&conn, "404 Not Found", "text/plain",
                            "not found\n", 10);
        }
        return;

    case WS_REQ_FAILED:
    default:
        ESP_LOGW(TAG, "[%s] request rejected", name);
        return;
    }
    ESP_LOGI(TAG, "[%s] connection open", name);

    if (ws_send(&conn, WS_DATA_TEXT, kGreeting, strlen(kGreeting)) < 0) {
        ESP_LOGW(TAG, "[%s] could not send the greeting", name);
        return;
    }

    while (ws_poll(&conn, POLL_TIMEOUT_MS) == 0) {
        /* ws_poll handles ping, close, and fragment reassembly itself, and
         * calls on_message once per complete message. Nothing to do here. */
    }
    ESP_LOGI(TAG, "[%s] connection closed", name);
}

static void ws_server_task(void *arg)
{
    ws_server_ctx_t *c = (ws_server_ctx_t *)arg;

    char *buffer = malloc(WS_MAX_MESSAGE_SIZE);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "[%s] out of memory for the %d-byte message buffer",
                 c->name, WS_MAX_MESSAGE_SIZE);
        free(c);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "[%s] waiting for link...", c->name);
    while (!c->is_up()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    int listen_fd = ws_transport_listen(c->ops, c->port);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "[%s] cannot listen on %u", c->name, c->port);
        goto done;
    }
    ESP_LOGI(TAG, "[%s] WebSocket server on port %u", c->name, c->port);

    for (;;) {
        int fd = ws_transport_accept(c->ops, listen_fd, ACCEPT_TIMEOUT_MS);
        if (fd == 0) {
            continue;                   /* nobody yet */
        }
        if (fd < 0) {
            ESP_LOGE(TAG, "[%s] accept failed", c->name);
            break;
        }

        serve_one(c->name, c->ops, fd, buffer, WS_MAX_MESSAGE_SIZE);

        /* Closing an accepted connection is also what re-arms the listener on
         * the TOE, where the two are the same socket -- see ws_transport.h. */
        ws_transport_close(c->ops, fd);
    }

    ws_transport_close(c->ops, listen_fd);

done:
    free(buffer);
    free(c);
    vTaskDelete(NULL);
}

void ws_server_start(const char *name, const void *ops, uint16_t port,
                     bool (*is_up)(void))
{
    ws_server_ctx_t *c = malloc(sizeof(*c));
    if (c == NULL) {
        ESP_LOGE(TAG, "[%s] out of memory", name);
        return;
    }
    c->name = name;
    c->ops = ops;
    c->port = port;
    c->is_up = is_up;

    if (xTaskCreate(ws_server_task, name, 4096, c, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "[%s] xTaskCreate failed", name);
        free(c);
    }
}
