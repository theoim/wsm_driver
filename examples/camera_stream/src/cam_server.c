/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * The camera server: a listener pool, a router, and an MJPEG stream.
 *
 * Backend-neutral -- everything network-facing goes through the vtable handed
 * to cam_server_start() and reaches the wire via http_transport.c.
 *
 * ---- Why one task drives several sockets ---------------------------------
 *
 * The page holds /stream open for as long as it is on screen while polling
 * /api/status every second, so at least two connections are live at once. The
 * obvious answer is a task per connection, but that multiplies the stack of a
 * task that already carries a 4 KB request buffer, and on the TOE the sockets
 * are a fixed hardware resource anyway.
 *
 * Instead one task owns a small array of slots and round-robins: it offers each
 * idle listener a brief accept, then gives each open connection a slice of
 * service. A slice is one frame for a stream, or one whole request-and-response
 * for anything else, so no slot can hold the loop for longer than a frame time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cam_server.h"
#include "cam_source.h"
#include "http_core.h"
#include "http_transport.h"
#include "web_page.h"

static const char *TAG = "cam_server";

/*
 * How many listening sockets each backend needs for the page to work.
 *
 * On LwIP one is enough: accept() returns a new descriptor and the listener
 * stays open. On the TOE the listening hardware socket becomes the connection,
 * so a lone listener can serve one client and nothing is left listening while
 * it does -- the status poll would then hang behind the stream forever. Three
 * covers the stream, the poll, and a reload arriving before the old socket has
 * been re-armed.
 */
#if defined(WSM_DRIVER_SOCKET_WRAP) && WSM_DRIVER_SOCKET_WRAP
#define CAM_ETH_LISTENERS  3
#else
#define CAM_ETH_LISTENERS  1
#endif

/* Wi-Fi is always software LwIP, wrap or no wrap. */
#define CAM_WIFI_LISTENERS 1

/*
 * How long an idle listener is offered per pass round the loop.
 *
 * These two numbers are the whole reason the loop is worth reading. An idle
 * server wants a long wait, so that sitting there costs nothing; a streaming
 * server wants the shortest one possible, because every idle listener's wait is
 * paid once per frame by the stream sharing the task.
 *
 * Measured on a W5500 with one 20 ms timeout for both cases, two idle
 * listeners, so 40 ms of dead time per frame:
 *
 *     640x480   send 32 ms   ->   1/(0.032+0.040) = 13.9 fps   (measured 14.5)
 *     800x600   send 68 ms   ->   1/(0.068+0.040) =  9.3 fps   (measured  9.5)
 *
 * The frame rate was being set by the accept timeout rather than by the sensor
 * or the link, which is exactly the thing this example exists to measure.
 */
#define ACCEPT_IDLE_MS      20
#define ACCEPT_BUSY_MS      1
#define REQUEST_TIMEOUT_MS  2000
#define REQUEST_BUF_SIZE    512
#define MAX_SLOTS           HTTP_MAX_LISTENERS

#define STREAM_BOUNDARY     "wsmframe"

typedef enum {
    SLOT_IDLE = 0,      /* nothing connected; its listener is armed */
    SLOT_STREAM,        /* an MJPEG response in progress            */
} slot_state_t;

typedef struct {
    int          listen_fd;   /* -1 once LwIP is serving from a shared listener */
    int          conn_fd;
    slot_state_t state;
} slot_t;

typedef struct {
    const char *name;
    const char *stack;
    const void *ops;
    uint16_t    port;
    int         listeners;
    bool      (*is_up)(void);
} cam_server_ctx_t;

/* ---- responses ------------------------------------------------------------ */

static int send_simple(const void *ops, int fd, const char *status,
                       const char *type, const char *body, size_t body_len)
{
    char head[256];
    int n = http_header(head, sizeof(head), status, type, (int)body_len, NULL);
    if (n < 0 || http_send(ops, fd, head, (size_t)n) < 0) {
        return -1;
    }
    return (body_len > 0) ? http_send(ops, fd, body, body_len) : 0;
}

/*
 * The status document, which is also the response to every control endpoint:
 * the page's call() feeds whatever comes back straight into render(), so a
 * control and a poll return the same shape and the UI never has to guess what
 * changed.
 */
static int send_status(const cam_server_ctx_t *c, int fd, cam_stats_t *st)
{
    char body[400];
    int n = snprintf(body, sizeof(body),
        "{\"streaming\":%s,\"res\":\"%s\",\"frames\":%u,\"dropped\":%u,"
        "\"fps\":%.1f,\"kb\":%u,\"capture_ms\":%u,\"send_ms\":%u,"
        "\"quality\":%d,\"xclk\":%d,\"stack\":\"%s\"}",
        st->streaming ? "true" : "false",
        cam_res_name(cam_get_resolution()),
        (unsigned)st->frames, (unsigned)st->dropped,
        st->fps, (unsigned)st->frame_kb,
        (unsigned)st->capture_ms, (unsigned)st->send_ms,
        cam_get_quality(), cam_get_xclk_mhz(), c->stack);

    if (n < 0 || (size_t)n >= sizeof(body)) {
        return -1;
    }
    return send_simple(c->ops, fd, "200 OK", "application/json", body,
                       (size_t)n);
}

/* ---- the stream ----------------------------------------------------------- */

static int stream_begin(const void *ops, int fd)
{
    char head[256];
    int n = http_header(head, sizeof(head), "200 OK",
                        "multipart/x-mixed-replace; boundary=" STREAM_BOUNDARY,
                        -1,                     /* no length: it never ends */
                        "Cache-Control: no-store\r\n");
    return (n < 0) ? -1 : http_send(ops, fd, head, (size_t)n);
}

/*
 * One frame of the multipart body. Returns 0 to keep streaming, -1 when the
 * connection is finished with.
 *
 * A failed capture is counted and skipped rather than ending the stream: the
 * sensor recovers on the next frame, and dropping the connection would blank
 * the browser for a fault that lasted 30 ms.
 */
static int stream_frame(const cam_server_ctx_t *c, int fd, cam_stats_t *st)
{
    size_t   len = 0;
    uint32_t capture_ms = 0;

    const uint8_t *jpeg = cam_frame_get(&len, &capture_ms);
    if (jpeg == NULL) {
        st->dropped++;
        return 0;
    }

    char part[128];
    int n = snprintf(part, sizeof(part),
                     "\r\n--" STREAM_BOUNDARY "\r\n"
                     "Content-Type: image/jpeg\r\n"
                     "Content-Length: %u\r\n\r\n", (unsigned)len);

    int64_t t0 = esp_timer_get_time();
    int rc = (n < 0) ? -1 : http_send(c->ops, fd, part, (size_t)n);
    if (rc == 0) {
        rc = http_send(c->ops, fd, jpeg, len);
    }
    uint32_t send_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    cam_frame_release();

    if (rc < 0) {
        return -1;                      /* browser went away */
    }

    st->frames++;
    st->acc_frames++;
    st->acc_bytes += (uint32_t)len;
    st->acc_capture_ms += capture_ms;
    st->acc_send_ms += send_ms;
    return 0;
}

/* ---- routing -------------------------------------------------------------- */

/* Returns 0 if the connection should stay open (a stream started), -1 if it is
 * done and should be closed. */
static int route(const cam_server_ctx_t *c, int fd, const http_request_t *req,
                 cam_stats_t *st, slot_state_t *state)
{
    if (strcmp(req->path, "/") == 0 || strcmp(req->path, "/index.html") == 0) {
        send_simple(c->ops, fd, "200 OK", "text/html; charset=utf-8",
                    HTTP_INDEX_PAGE, strlen(HTTP_INDEX_PAGE));
        return -1;
    }

    if (strcmp(req->path, "/stream") == 0) {
        if (!st->streaming) {
            /* The page can ask for the stream before pressing START; answering
             * 503 lets it show that rather than hanging on a dead socket. */
            send_simple(c->ops, fd, "503 Service Unavailable", "text/plain",
                        "not streaming\n", 14);
            return -1;
        }
        if (stream_begin(c->ops, fd) < 0) {
            return -1;
        }
        ESP_LOGI(TAG, "[%s] stream opened", c->name);
        *state = SLOT_STREAM;
        return 0;
    }

    if (strncmp(req->path, "/api/", 5) == 0) {
        const char *api = req->path + 5;

        if (strcmp(api, "start") == 0) {
            st->streaming = true;
        } else if (strcmp(api, "stop") == 0) {
            st->streaming = false;
        } else if (strcmp(api, "reset") == 0) {
            cam_reset();
        } else if (strcmp(api, "res") == 0) {
            char value[16];
            if (http_query_str(req->query, "v", value, sizeof(value)) == 0) {
                int res = cam_res_from_name(value);
                if (res >= 0) {
                    cam_set_resolution((cam_res_t)res);
                }
            }
        } else if (strcmp(api, "cam") == 0) {
            /* Absent parameters keep their value -- the page sends whichever
             * slider moved, not the whole set. */
            int quality = http_query_int(req->query, "quality",
                                         cam_get_quality());
            int xclk = http_query_int(req->query, "xclk", cam_get_xclk_mhz());
            cam_set_quality(quality);
            cam_set_xclk_mhz(xclk);
        } else if (strcmp(api, "status") != 0) {
            send_simple(c->ops, fd, "404 Not Found", "text/plain",
                        "no such endpoint\n", 17);
            return -1;
        }

        send_status(c, fd, st);
        return -1;
    }

    send_simple(c->ops, fd, "404 Not Found", "text/plain", "not found\n", 10);
    return -1;
}

/* Read the request line and act on it. Returns 0 to keep the connection. */
static int serve_request(const cam_server_ctx_t *c, slot_t *slot,
                         cam_stats_t *st)
{
    char buf[REQUEST_BUF_SIZE];
    int n = http_recv(c->ops, slot->conn_fd, buf, sizeof(buf) - 1,
                      REQUEST_TIMEOUT_MS);
    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';

    /* Only the request line matters here; the headers that follow are read off
     * the socket with it and discarded. A request larger than the buffer is a
     * client this example does not serve. */
    http_request_t req;
    if (http_parse_request(buf, &req) < 0) {
        send_simple(c->ops, slot->conn_fd, "400 Bad Request", "text/plain",
                    "bad request\n", 12);
        return -1;
    }

    return route(c, slot->conn_fd, &req, st, &slot->state);
}

/* ---- the task ------------------------------------------------------------- */

static void close_slot(const cam_server_ctx_t *c, slot_t *slot)
{
    if (slot->conn_fd >= 0) {
        /* On the TOE this is also what re-arms the listener, because the two
         * are the same socket. On LwIP the listener was never disturbed. */
        http_close(c->ops, slot->conn_fd);
        if (slot->conn_fd == slot->listen_fd) {
            /* Same descriptor: it is a listener again, not a spent socket. */
        }
        slot->conn_fd = -1;
    }
    slot->state = SLOT_IDLE;
}

static void cam_server_task(void *arg)
{
    cam_server_ctx_t *c = (cam_server_ctx_t *)arg;
    cam_stats_t st;
    memset(&st, 0, sizeof(st));

    slot_t slots[MAX_SLOTS];
    for (int i = 0; i < MAX_SLOTS; i++) {
        slots[i].listen_fd = -1;
        slots[i].conn_fd = -1;
        slots[i].state = SLOT_IDLE;
    }

    ESP_LOGI(TAG, "[%s] waiting for link...", c->name);
    while (!c->is_up()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    int fds[HTTP_MAX_LISTENERS];
    int opened = http_listen_pool(c->ops, c->port, c->listeners, fds);
    if (opened == 0) {
        ESP_LOGE(TAG, "[%s] cannot listen on %u", c->name, c->port);
        goto done;
    }
    for (int i = 0; i < opened; i++) {
        slots[i].listen_fd = fds[i];
    }
    ESP_LOGI(TAG, "[%s] camera server on port %u, %d listener%s (%s)",
             c->name, c->port, opened, opened == 1 ? "" : "s", c->stack);

    for (;;) {
        /* Decided once per pass rather than per slot, so every listener in this
         * pass agrees on whether a stream is waiting behind it. */
        bool streaming_now = false;
        for (int i = 0; i < opened; i++) {
            if (slots[i].state == SLOT_STREAM) {
                streaming_now = true;
                break;
            }
        }
        uint32_t accept_ms = streaming_now ? ACCEPT_BUSY_MS : ACCEPT_IDLE_MS;

        for (int i = 0; i < opened; i++) {
            slot_t *slot = &slots[i];

            if (slot->conn_fd < 0) {
                int fd = http_accept(c->ops, slot->listen_fd, accept_ms);
                if (fd > 0) {
                    slot->conn_fd = fd;
                    slot->state = SLOT_IDLE;
                } else if (fd < 0) {
                    ESP_LOGE(TAG, "[%s] listener %d failed, reopening",
                             c->name, i);
                    http_close(c->ops, slot->listen_fd);
                    slot->listen_fd = -1;
                    vTaskDelay(pdMS_TO_TICKS(1000));

                    int again[1];
                    if (http_listen_pool(c->ops, c->port, 1, again) == 1) {
                        slot->listen_fd = again[0];
                    } else {
                        ESP_LOGE(TAG, "[%s] listener %d is gone", c->name, i);
                    }
                }
                continue;
            }

            if (slot->state == SLOT_STREAM) {
                if (!st.streaming || stream_frame(c, slot->conn_fd, &st) < 0) {
                    ESP_LOGI(TAG, "[%s] stream closed", c->name);
                    close_slot(c, slot);
                }
                continue;
            }

            if (serve_request(c, slot, &st) < 0) {
                close_slot(c, slot);
            }
        }

        cam_stats_tick(&st);

        /* Yield when nothing is streaming, so an idle server is not spinning
         * through accept timeouts at full tilt. A streaming pass already
         * yielded inside the frame send and must not add to it. */
        if (!streaming_now) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

done:
    free(c);
    vTaskDelete(NULL);
}

void cam_server_start(const char *name, const char *stack_name, const void *ops,
                      uint16_t port, int listeners, bool (*is_up)(void))
{
    cam_server_ctx_t *c = malloc(sizeof(*c));
    if (c == NULL) {
        ESP_LOGE(TAG, "[%s] out of memory", name);
        return;
    }
    c->name = name;
    c->stack = stack_name;
    c->ops = ops;
    c->port = port;
    c->listeners = listeners;
    c->is_up = is_up;

    /* 6 KB: the request buffer and the JSON body live on this stack, and the
     * LwIP path adds its own frames underneath the socket calls. */
    if (xTaskCreate(cam_server_task, name, 6144, c, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "[%s] xTaskCreate failed", name);
        free(c);
    }
}
