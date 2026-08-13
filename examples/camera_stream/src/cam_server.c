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
 * Instead one task owns a small array of connection slots and round-robins:
 * each pass offers the free listeners a brief accept, then gives every open
 * connection a slice of service. A slice is one frame for a stream, or one whole
 * request-and-response for anything else, so nothing can hold the loop for
 * longer than a frame time.
 */
#include <limits.h>
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

/* Pause before rebuilding a listening socket that failed, so a persistent fault
 * logs at a readable rate instead of filling the console. */
#define LISTEN_RETRY_MS     1000

/*
 * Consecutive empty accepts after which the listening socket is rebuilt.
 *
 * Works around a state the TOE backend does not leave on its own.
 * wiztoe_accept() advances a hardware socket when it reads SOCK_ESTABLISHED and
 * re-arms it when it reads SOCK_CLOSED, but a client that connects and then
 * drops without sending anything leaves the socket in SOCK_CLOSE_WAIT --
 * neither case -- so accept() times out against it indefinitely and that
 * listener is gone for good.
 *
 * A browser closing a tab does exactly this. Measured in examples/modbus_tcp,
 * where the same shape took the server out for more than ten seconds after six
 * connect-and-close cycles, and kept it out under a client that retried
 * quickly. Rebuilding an idle listener costs a socket open and a listen.
 *
 * The real fix belongs in wiztoe_accept(), which should treat SOCK_CLOSE_WAIT
 * the way it treats SOCK_CLOSED.
 *
 * Counted in passes rather than milliseconds, because a pass is 20 ms when idle
 * and a frame time when streaming. The wall-clock interval is therefore loose
 * on purpose: what matters is that a wedged listener is eventually rebuilt, not
 * that it happens on a schedule.
 *
 * Only on the TOE. LwIP's listener is never consumed by a connection and never
 * enters this state, so recycling there would be a socket rebuild that buys
 * nothing -- and a listener closed for a moment is a listener that can miss a
 * connection arriving in it.
 */
#if defined(WSM_DRIVER_SOCKET_WRAP) && WSM_DRIVER_SOCKET_WRAP
#define RECYCLE_AFTER       200   /* passes; see above */
#else
#define RECYCLE_AFTER 0   /* never: LwIP listeners do not wedge */
#endif
#define REQUEST_TIMEOUT_MS  2000

/* Whole-request ceiling, not per read. See serve_request(). */
#define REQUEST_DEADLINE_MS 5000
#define REQUEST_BUF_SIZE    512

#define STREAM_BOUNDARY     "wsmframe"

/*
 * Connections are tracked separately from listeners, because the relationship
 * between the two is not the same on both backends.
 *
 * On the TOE they are one thing: accept() returns the listening socket itself,
 * so a connection consumes the listener that produced it and closing gives it
 * back. On LwIP they are independent: one listener produces new descriptors
 * indefinitely and is never consumed.
 *
 * An earlier version had one array for both, which is right for the TOE and
 * wrong for LwIP -- with a single listener there was a single slot, so the
 * Wi-Fi side could hold exactly one connection and the page's status poll sat
 * behind its own stream, leaving the charts frozen. Keeping the arrays apart
 * lets each backend contribute what it actually has: listeners for accepting,
 * slots for serving.
 */
#define MAX_CONNS  3

typedef enum {
    SLOT_FREE = 0,      /* nothing here                  */
    SLOT_REQUEST,       /* connected, request not read yet */
    SLOT_STREAM,        /* an MJPEG response in progress   */
} slot_state_t;

typedef struct {
    int          fd;
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
static int send_status_with(const cam_server_ctx_t *c, int fd,
                            cam_stats_t *st, const char *status)
{
    char body[900];
    int n = snprintf(body, sizeof(body),
        "{\"streaming\":%s,\"res\":\"%s\",\"frames\":%u,\"dropped\":%u,"
        "\"fps\":%.1f,\"kb\":%u,\"capture_ms\":%u,\"send_ms\":%u,"
        "\"quality\":%d,\"xclk\":%d,\"stack\":\"%s\",\"ctrl\":{",
        st->streaming ? "true" : "false",
        cam_res_name(cam_get_resolution()),
        (unsigned)st->frames, (unsigned)st->dropped,
        st->fps, (unsigned)st->frame_kb,
        (unsigned)st->capture_ms, (unsigned)st->send_ms,
        cam_get_quality(), cam_get_xclk_mhz(), c->stack);

    /* Current control values only -- the names, ranges and labels come from
     * /api/controls once at page load, because this document is fetched every
     * second and does not need to repeat what cannot change. */
    for (int i = 0; i < cam_ctrl_count() && n > 0 && (size_t)n < sizeof(body); i++) {
        const cam_ctrl_t *ctrl = cam_ctrl_at(i);
        n += snprintf(body + n, sizeof(body) - n, "%s\"%s\":%d",
                      i ? "," : "", ctrl->name, ctrl->value);
    }
    n += snprintf(body + n, sizeof(body) - n, "}}");

    if (n < 0 || (size_t)n >= sizeof(body)) {
        return -1;
    }
    return send_simple(c->ops, fd, status, "application/json", body,
                       (size_t)n);
}

static int send_status(const cam_server_ctx_t *c, int fd, cam_stats_t *st)
{
    return send_status_with(c, fd, st, "200 OK");
}

/*
 * The control descriptor list: name, label, group and range for every sensor
 * control. Fetched once when the page loads, which is what lets the panel be
 * built from the device rather than hardcoded in the HTML -- adding a row to
 * the table in cam_source.c makes a new slider appear with no page edit.
 */
static int send_controls(const cam_server_ctx_t *c, int fd)
{
    /* Roughly 95 bytes per control once the name, label, group and range are
     * quoted, so this holds about thirty of them. Adding controls past that
     * needs a bigger buffer, and the overflow check below says so out loud --
     * silently truncating produces JSON the page cannot parse, which shows up
     * as an empty panel with no clue why. */
    char body[3072];
    int n = snprintf(body, sizeof(body), "[");

    for (int i = 0; i < cam_ctrl_count() && n > 0 && (size_t)n < sizeof(body); i++) {
        const cam_ctrl_t *ctrl = cam_ctrl_at(i);
        n += snprintf(body + n, sizeof(body) - n,
                      "%s{\"name\":\"%s\",\"label\":\"%s\",\"group\":\"%s\","
                      "\"min\":%d,\"max\":%d}",
                      i ? "," : "", ctrl->name, ctrl->label,
                      cam_group_name(ctrl->group), ctrl->min, ctrl->max);
    }
    n += snprintf(body + n, sizeof(body) - n, "]");

    if (n < 0 || (size_t)n >= sizeof(body)) {
        ESP_LOGE(TAG, "control list needs %d bytes, buffer is %u -- "
                 "raise it or the panel arrives empty",
                 n, (unsigned)sizeof(body));
        send_simple(c->ops, fd, "500 Internal Server Error", "text/plain",
                    "control list too large\n", 23);
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

        /*
         * Refused settings are reported, not swallowed.
         *
         * Every control here can be turned down by the sensor -- a frame size
         * it cannot allocate for, an XCLK it will not lock to -- and answering
         * 200 with the current state either way tells the page the change went
         * through. It then shows the old value with no explanation, which reads
         * as the UI being broken rather than the request being refused.
         */
        bool refused = false;

        if (strcmp(api, "start") == 0) {
            st->streaming = true;
        } else if (strcmp(api, "stop") == 0) {
            st->streaming = false;
        } else if (strcmp(api, "reset") == 0) {
            refused = (cam_reset() != 0);
        } else if (strcmp(api, "res") == 0) {
            char value[16];
            if (http_query_str(req->query, "v", value, sizeof(value)) != 0) {
                refused = true;
            } else {
                int res = cam_res_from_name(value);
                refused = (res < 0) || (cam_set_resolution((cam_res_t)res) != 0);
            }
        } else if (strcmp(api, "controls") == 0) {
            send_controls(c, fd);
            return -1;
        } else if (strcmp(api, "cam") == 0) {
            /* Absent parameters keep their value -- the page sends whichever
             * control moved, not the whole set. That is also what makes the
             * sentinel below safe: a control cannot legitimately be set to
             * INT_MIN, so it stands in for "not present" without a second
             * lookup to ask whether the key was there. */
            int quality = http_query_int(req->query, "quality",
                                         cam_get_quality());
            int xclk = http_query_int(req->query, "xclk", cam_get_xclk_mhz());

            /* Every requested change is attempted even if an earlier one
             * failed: they are independent, and stopping at the first refusal
             * would leave the rest of a slider panel silently unapplied. */
            if (quality != cam_get_quality() && cam_set_quality(quality) != 0) {
                refused = true;
            }
            if (xclk != cam_get_xclk_mhz() && cam_set_xclk_mhz(xclk) != 0) {
                refused = true;
            }

            for (int i = 0; i < cam_ctrl_count(); i++) {
                const cam_ctrl_t *ctrl = cam_ctrl_at(i);
                int value = http_query_int(req->query, ctrl->name, INT_MIN);
                if (value != INT_MIN && cam_ctrl_set(ctrl->name, value) != 0) {
                    refused = true;
                }
            }
        } else if (strcmp(api, "status") != 0) {
            send_simple(c->ops, fd, "404 Not Found", "text/plain",
                        "no such endpoint\n", 17);
            return -1;
        }

        if (refused) {
            ESP_LOGW(TAG, "[%s] /api/%s: the sensor refused a setting",
                     c->name, api);
        }
        /* The body is the status document either way, so the page's single
         * render path still works; the status code carries the outcome. */
        send_status_with(c, fd, st, refused ? "409 Conflict" : "200 OK");
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
    size_t got = 0;

    /*
     * Read until the request line is complete, not once.
     *
     * A single recv() returns whatever one TCP segment carried, and there is no
     * rule that a browser's request line arrives in one. Parsing the first
     * fragment gives a truncated path -- "/api/stat" routes to the 404 -- and
     * the failure is intermittent, which is the worst kind: it depends on how
     * the request happened to be split on the way over.
     *
     * The absolute deadline matters as much as the loop. A per-read timeout
     * alone bounds nothing, because a client dribbling one byte inside it keeps
     * resetting the clock, and with three listeners one such client is a
     * noticeable share of the server.
     */
    int64_t deadline = esp_timer_get_time() +
                       (int64_t)REQUEST_DEADLINE_MS * 1000;

    for (;;) {
        if (esp_timer_get_time() > deadline) {
            send_simple(c->ops, slot->fd, "408 Request Timeout", "text/plain",
                        "took too long\n", 14);
            return -1;
        }

        int n = http_recv(c->ops, slot->fd, buf + got, sizeof(buf) - got - 1,
                          REQUEST_TIMEOUT_MS);
        if (n < 0) {
            return -1;
        }
        if (n > 0) {
            got += (size_t)n;
            buf[got] = '\0';
            if (strstr(buf, "\r\n") != NULL) {
                break;                  /* the request line is all we parse */
            }
        }
        if (got + 2 >= sizeof(buf)) {
            send_simple(c->ops, slot->fd, "431 Request Header Fields Too Large",
                        "text/plain", "request too large\n", 18);
            return -1;
        }
    }

    /* Only the request line matters here; the headers that follow are read off
     * the socket with it and discarded. A request larger than the buffer is a
     * client this example does not serve. */
    http_request_t req;
    if (http_parse_request(buf, &req) < 0) {
        send_simple(c->ops, slot->fd, "400 Bad Request", "text/plain",
                    "bad request\n", 12);
        return -1;
    }

    return route(c, slot->fd, &req, st, &slot->state);
}

/* ---- the task ------------------------------------------------------------- */

static void close_slot(const cam_server_ctx_t *c, slot_t *slot)
{
    if (slot->fd >= 0) {
        /* On the TOE this is also what re-arms the listener, because the two
         * are the same socket -- which is why the listener does not need to be
         * reopened here. On LwIP the listener was never disturbed. */
        http_close(c->ops, slot->fd);
        slot->fd = -1;
    }
    slot->state = SLOT_FREE;
}

/*
 * Is this listener currently being used as a connection?
 *
 * Only ever true on the TOE, where accept() hands back the descriptor it was
 * given. Asking the question by descriptor rather than by backend keeps the
 * loop free of an #if: on LwIP no connection can ever equal a listener, so the
 * answer is always false and the listener is always available.
 */
static bool listener_in_use(const slot_t *slots, int listen_fd)
{
    for (int i = 0; i < MAX_CONNS; i++) {
        if (slots[i].state != SLOT_FREE && slots[i].fd == listen_fd) {
            return true;
        }
    }
    return false;
}

static slot_t *free_slot(slot_t *slots)
{
    for (int i = 0; i < MAX_CONNS; i++) {
        if (slots[i].state == SLOT_FREE) {
            return &slots[i];
        }
    }
    return NULL;
}

static void cam_server_task(void *arg)
{
    cam_server_ctx_t *c = (cam_server_ctx_t *)arg;
    cam_stats_t st;
    memset(&st, 0, sizeof(st));

    slot_t slots[MAX_CONNS];
    for (int i = 0; i < MAX_CONNS; i++) {
        slots[i].fd = -1;
        slots[i].state = SLOT_FREE;
    }

    ESP_LOGI(TAG, "[%s] waiting for link...", c->name);
    while (!c->is_up()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    int listen_fds[HTTP_MAX_LISTENERS];
    int idle_passes[HTTP_MAX_LISTENERS] = {0};
    int listeners = http_listen_pool(c->ops, c->port, c->listeners, listen_fds);
    if (listeners == 0) {
        ESP_LOGE(TAG, "[%s] cannot listen on %u", c->name, c->port);
        goto done;
    }
    ESP_LOGI(TAG, "[%s] camera server on port %u, %d listener%s, "
             "%d connections (%s)",
             c->name, c->port, listeners, listeners == 1 ? "" : "s",
             MAX_CONNS, c->stack);

    for (;;) {
        /* Decided once per pass rather than per listener, so every accept in
         * this pass agrees on whether a stream is waiting behind it. */
        bool streaming_now = false;
        for (int i = 0; i < MAX_CONNS; i++) {
            if (slots[i].state == SLOT_STREAM) {
                streaming_now = true;
                break;
            }
        }
        uint32_t accept_ms = streaming_now ? ACCEPT_BUSY_MS : ACCEPT_IDLE_MS;

        /* ---- take new connections ---- */
        for (int i = 0; i < listeners; i++) {
            if (listen_fds[i] < 0) {
                /* Lost earlier and worth another try: running out of hardware
                 * sockets is usually momentary -- another connection closing
                 * frees one -- so a listener that could not be rebuilt then
                 * should not be written off for the life of the task. */
                int again[1];
                if (http_listen_pool(c->ops, c->port, 1, again) == 1) {
                    listen_fds[i] = again[0];
                    ESP_LOGI(TAG, "[%s] listener %d is back", c->name, i);
                } else {
                    continue;
                }
            }
            if (listener_in_use(slots, listen_fds[i])) {
                continue;
            }
            slot_t *slot = free_slot(slots);
            if (slot == NULL) {
                break;                  /* every slot busy; nothing to accept into */
            }

            int fd = http_accept(c->ops, listen_fds[i], accept_ms);
            if (fd > 0) {
                idle_passes[i] = 0;
                slot->fd = fd;
                slot->state = SLOT_REQUEST;
            } else if (fd == 0) {
                /* An idle listener and one stuck in CLOSE_WAIT look identical
                 * from here; rebuilding costs a socket open and is the only
                 * escape from the second. */
                if (RECYCLE_AFTER > 0 && ++idle_passes[i] >= RECYCLE_AFTER) {
                    idle_passes[i] = 0;
                    http_close(c->ops, listen_fds[i]);
                    int again[1];
                    listen_fds[i] = (http_listen_pool(c->ops, c->port, 1, again) == 1)
                                        ? again[0] : -1;
                }
            } else {
                ESP_LOGE(TAG, "[%s] listener %d failed, reopening", c->name, i);
                http_close(c->ops, listen_fds[i]);
                listen_fds[i] = -1;
                vTaskDelay(pdMS_TO_TICKS(LISTEN_RETRY_MS));

                int again[1];
                if (http_listen_pool(c->ops, c->port, 1, again) == 1) {
                    listen_fds[i] = again[0];
                } else {
                    ESP_LOGE(TAG, "[%s] listener %d is gone", c->name, i);
                }
            }
        }

        /* ---- give every open connection a slice ---- */
        for (int i = 0; i < MAX_CONNS; i++) {
            slot_t *slot = &slots[i];

            if (slot->state == SLOT_STREAM) {
                if (!st.streaming || stream_frame(c, slot->fd, &st) < 0) {
                    ESP_LOGI(TAG, "[%s] stream closed", c->name);
                    close_slot(c, slot);
                }
            } else if (slot->state == SLOT_REQUEST) {
                if (serve_request(c, slot, &st) < 0) {
                    close_slot(c, slot);
                }
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
