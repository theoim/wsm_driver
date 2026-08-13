/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * The web UI (see web_server.h).
 *
 * Reaches the network through mb_transport.c, the same seam the Modbus server
 * uses, so nothing here includes lwIP and both halves of the firmware speak to
 * the chip the same way.
 *
 * JSON is written with snprintf and read with a hand-rolled scan of a handful
 * of keys. That is not a claim that writing a parser is better than using one;
 * it is that the whole exchange is four flat objects with fixed field names,
 * and adding a JSON library to parse `{"ip":"192.168.11.2"}` would be a
 * dependency the rest of the example does not need.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wizchip_conf.h"     /* wizchip_getnetinfo, CW_GET_PHYLINK */

#include "app_config.h"
#include "app_control.h"
#include "mb_store.h"
#include "mb_transport.h"
#include "net_config.h"
#include "web_page.h"
#include "web_server.h"

static const char *TAG = "web";

/*
 * Three listeners on the TOE, for the same reason the camera example needs
 * them: a listening hardware socket becomes the connection, so one listener
 * serves one browser request and nothing is left listening while it does. The
 * page polls once a second and a reload can arrive on top of that.
 *
 * On software LwIP one listener feeds every connection, and the Modbus server
 * is sharing the same eight hardware sockets, so this asks for no more than it
 * needs.
 */
#if defined(WSM_DRIVER_SOCKET_WRAP) && WSM_DRIVER_SOCKET_WRAP
#define WEB_LISTENERS   3
#else
#define WEB_LISTENERS   1
#endif

/*
 * How long each listener is offered per pass round the loop.
 *
 * One task polls every listener in turn with a blocking accept, so a connection
 * waits up to listeners x timeout before anyone looks at it. That is not a
 * theoretical cost: at 200 ms and three listeners, a browser measured 430 ms to
 * first byte -- almost exactly two idle waits -- and a load test refused 261 of
 * 300 requests, because a second connection arriving for a listener still being
 * waited on has nowhere to queue.
 *
 * At 20 ms the same walk costs 60 ms worst case. The idle cost of the shorter
 * timeout is one extra pass per 20 ms of a task that is doing nothing else.
 */
#define ACCEPT_TIMEOUT_MS   20
#define REQUEST_TIMEOUT_MS  2000

/* Whole-request ceiling, not per read. See serve_one(). */
#define REQUEST_DEADLINE_MS 5000

/* Pause before rebuilding a listening socket that failed. */
#define LISTEN_RETRY_MS     1000

/*
 * Consecutive empty accepts after which a listener is rebuilt.
 *
 * This works around a state the TOE backend does not recover from on its own.
 * wiztoe_accept() advances a hardware socket when it reads SOCK_ESTABLISHED,
 * and re-arms it when it reads SOCK_CLOSED, but a client that connects and
 * then aborts before sending anything leaves the socket in SOCK_CLOSE_WAIT --
 * neither case -- so accept() times out against it forever and that listener
 * is gone. A browser navigating away mid-request does exactly this, and with
 * three listeners it takes three of them to kill the web UI outright.
 *
 * Measured before this: a load test that abandoned one connection every ten
 * rounds went from 120/120 requests served to 32/120 as the listeners died off
 * one by one. Rebuilding an idle listener costs a socket open and a listen, so
 * doing it every RECYCLE_AFTER x ACCEPT_TIMEOUT_MS of genuine silence is far
 * cheaper than the failure it prevents.
 *
 * The real fix belongs in wiztoe_accept(), which should treat SOCK_CLOSE_WAIT
 * the way it treats SOCK_CLOSED. Until then, every TOE server that faces a
 * browser needs something like this.
 */
#define RECYCLE_AFTER       100     /* x 20 ms = 2 s of nothing arriving */
#define REQUEST_BUF_SIZE    1024
#define JSON_BUF_SIZE       3072
#define STORE_TIMEOUT_MS    500

struct web_server {
    const void *ops;
    uint16_t    port;
    mb_store_t *store;
    bool      (*is_up)(void);

    volatile bool stop;
    volatile bool finished;
};

typedef struct web_server web_ctx_t;

/* ---- responses ------------------------------------------------------------ */

static int send_response(const void *ops, int fd, const char *status,
                         const char *type, const char *body, size_t len)
{
    char head[192];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %u\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     status, type, (unsigned)len);
    if (n < 0 || (size_t)n >= sizeof(head)) {
        return -1;
    }
    if (mb_transport_send(ops, fd, head, (size_t)n) < 0) {
        return -1;
    }
    return (len > 0) ? mb_transport_send(ops, fd, body, len) : 0;
}

/* An error the page can show verbatim. The reason matters: "invalid
 * configuration" sends a user back to guess which of four fields was wrong. */
static int send_error(const void *ops, int fd, const char *status,
                      const char *reason)
{
    char body[192];
    int n = snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}",
                     reason);
    return send_response(ops, fd, status, "application/json", body, (size_t)n);
}

/* ---- JSON out ------------------------------------------------------------- */

static int json_ip(char *buf, size_t size, const uint8_t ip[4])
{
    return snprintf(buf, size, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static int build_status(const mb_snapshot_t *snap, char *body, size_t size)
{
    app_config_t cfg;
    app_control_current(&cfg);

    char ip[16], mask[16], gw[16];
    json_ip(ip, sizeof(ip), cfg.ip);
    json_ip(mask, sizeof(mask), cfg.mask);
    json_ip(gw, sizeof(gw), cfg.gateway);

    /*
     * The MAC and the PHY link come from the chip rather than from what was
     * configured, because those are the two facts a person standing next to an
     * unresponsive device actually wants. wiznet_net_is_up() is not one of
     * them: on the TOE backend it reports that bring-up finished, not that a
     * cable is plugged in, so it would answer "up" into a dead socket.
     */
    wiz_NetInfo ni;
    memset(&ni, 0, sizeof(ni));
    wizchip_getnetinfo(&ni);

    uint8_t phy = 0;
    bool link = (ctlwizchip(CW_GET_PHYLINK, &phy) != -1) && (phy == PHY_LINK_ON);

    return snprintf(body, size,
        "{\"ip\":\"%s\",\"mask\":\"%s\",\"gateway\":\"%s\",\"port\":%u,"
        "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"link\":%s,"
        "\"running\":%s,\"client\":%s,\"pending\":%s,"
        "\"requests\":%u,\"exceptions\":%u,\"sessions\":%u,"
        "\"last_function\":%u,\"last_exception\":%u,"
        "\"regs\":%d,\"coils\":%d}",
        ip, mask, gw, cfg.modbus_port,
        ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5],
        link ? "true" : "false",
        snap->stats.running ? "true" : "false",
        snap->stats.client_connected ? "true" : "false",
        app_control_pending() ? "true" : "false",
        (unsigned)snap->stats.requests, (unsigned)snap->stats.exceptions,
        (unsigned)snap->stats.sessions,
        snap->stats.last_function, snap->stats.last_exception,
        MB_REG_COUNT, MB_COIL_COUNT);
}

/*
 * The register dump. Four arrays, whole tables at a time.
 *
 * 64 registers and 64 bits fit comfortably; a real device with thousands would
 * want a range in the query rather than this. Said plainly because the shape of
 * the endpoint invites the assumption that it scales.
 */
static int build_registers(const mb_snapshot_t *snap, char *body, size_t size)
{
    int n = snprintf(body, size, "{\"holding\":[");
    for (int i = 0; i < MB_REG_COUNT && n > 0 && (size_t)n < size; i++) {
        n += snprintf(body + n, size - n, "%s%u", i ? "," : "",
                      snap->data.holding[i]);
    }
    n += snprintf(body + n, size - n, "],\"input\":[");
    for (int i = 0; i < MB_REG_COUNT && n > 0 && (size_t)n < size; i++) {
        n += snprintf(body + n, size - n, "%s%u", i ? "," : "",
                      snap->data.input[i]);
    }
    n += snprintf(body + n, size - n, "],\"coil\":[");
    for (int i = 0; i < MB_COIL_COUNT && n > 0 && (size_t)n < size; i++) {
        n += snprintf(body + n, size - n, "%s%d", i ? "," : "",
                      snap->data.coil[i] ? 1 : 0);
    }
    n += snprintf(body + n, size - n, "],\"discrete\":[");
    for (int i = 0; i < MB_COIL_COUNT && n > 0 && (size_t)n < size; i++) {
        n += snprintf(body + n, size - n, "%s%d", i ? "," : "",
                      snap->data.discrete[i] ? 1 : 0);
    }
    n += snprintf(body + n, size - n, "]}");
    return n;
}

static int build_config(char *body, size_t size)
{
    app_config_t cfg;
    app_control_current(&cfg);

    char ip[16], mask[16], gw[16];
    json_ip(ip, sizeof(ip), cfg.ip);
    json_ip(mask, sizeof(mask), cfg.mask);
    json_ip(gw, sizeof(gw), cfg.gateway);

    return snprintf(body, size,
                    "{\"ip\":\"%s\",\"mask\":\"%s\",\"gateway\":\"%s\","
                    "\"port\":%u}",
                    ip, mask, gw, cfg.modbus_port);
}

/* ---- JSON in -------------------------------------------------------------- */

/*
 * Pull one string value out of a flat JSON object.
 *
 * Deliberately narrow: it finds "key" followed by a colon and a quoted value,
 * and refuses everything else. Nested objects, arrays and escapes are not
 * handled because the request bodies this accepts have none, and a parser that
 * quietly half-handles them would be worse than one that says no.
 */
static bool json_string(const char *body, const char *key, char *out,
                        size_t size)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *at = strstr(body, pattern);
    if (at == NULL) {
        return false;
    }
    at = strchr(at + strlen(pattern), ':');
    if (at == NULL) {
        return false;
    }
    at = strchr(at, '"');
    if (at == NULL) {
        return false;
    }
    at++;

    const char *end = strchr(at, '"');
    if (end == NULL || (size_t)(end - at) >= size) {
        return false;
    }
    memcpy(out, at, (size_t)(end - at));
    out[end - at] = '\0';
    return true;
}

static bool json_number(const char *body, const char *key, long *out)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *at = strstr(body, pattern);
    if (at == NULL) {
        return false;
    }
    at = strchr(at + strlen(pattern), ':');
    if (at == NULL) {
        return false;
    }

    char *end = NULL;
    long value = strtol(at + 1, &end, 10);
    if (end == at + 1) {
        return false;
    }
    *out = value;
    return true;
}

/*
 * POST /api/config.
 *
 * Every field is optional and anything absent keeps its current value, so a
 * page that only wants to change the port sends only the port. The whole
 * candidate is validated together afterwards, because the fields constrain each
 * other -- a gateway is only wrong relative to an address and a mask.
 */
static int handle_post_config(const web_ctx_t *c, int fd, const char *body)
{
    app_config_t cfg;
    app_control_current(&cfg);

    char text[24];
    if (json_string(body, "ip", text, sizeof(text)) &&
        !app_config_parse_ip(text, cfg.ip)) {
        return send_error(c->ops, fd, "400 Bad Request", "IP is not an address");
    }
    if (json_string(body, "mask", text, sizeof(text)) &&
        !app_config_parse_ip(text, cfg.mask)) {
        return send_error(c->ops, fd, "400 Bad Request",
                          "mask is not an address");
    }
    if (json_string(body, "gateway", text, sizeof(text)) &&
        !app_config_parse_ip(text, cfg.gateway)) {
        return send_error(c->ops, fd, "400 Bad Request",
                          "gateway is not an address");
    }

    long port;
    if (json_number(body, "port", &port)) {
        if (port < 1 || port > 65535) {
            return send_error(c->ops, fd, "400 Bad Request",
                              "port must be 1..65535");
        }
        cfg.modbus_port = (uint16_t)port;
    }

    char reason[80];
    if (!app_config_validate(&cfg, reason, sizeof(reason))) {
        return send_error(c->ops, fd, "400 Bad Request", reason);
    }

    if (app_config_save(&cfg) != 0) {
        return send_error(c->ops, fd, "500 Internal Server Error",
                          "could not write to NVS");
    }

    /* Answer before anything moves. The apply runs on the control task once
     * this response has been sent -- see app_control.h. */
    char ip[16];
    json_ip(ip, sizeof(ip), cfg.ip);

    char reply[192];
    int n = snprintf(reply, sizeof(reply),
                     "{\"ok\":true,\"ip\":\"%s\",\"port\":%u,"
                     "\"message\":\"Saved. Device is moving to %s:%u -- "
                     "reconnect there.\"}",
                     ip, cfg.modbus_port, ip, cfg.modbus_port);
    int rc = send_response(c->ops, fd, "200 OK", "application/json", reply,
                           (size_t)n);

    app_control_apply(&cfg);
    return rc;
}

/* ---- routing -------------------------------------------------------------- */

static int route(const web_ctx_t *c, int fd, const char *method,
                 const char *path, const char *body)
{
    if (strcmp(method, "GET") == 0 &&
        (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
        return send_response(c->ops, fd, "200 OK", "text/html; charset=utf-8",
                             WEB_INDEX_PAGE, strlen(WEB_INDEX_PAGE));
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/config") == 0) {
        return handle_post_config(c, fd, body);
    }

    if (strcmp(method, "GET") != 0) {
        return send_error(c->ops, fd, "405 Method Not Allowed", "GET or POST");
    }

    char *json = malloc(JSON_BUF_SIZE);
    if (json == NULL) {
        return send_error(c->ops, fd, "500 Internal Server Error", "no memory");
    }

    int n = -1;
    if (strcmp(path, "/api/config") == 0) {
        n = build_config(json, JSON_BUF_SIZE);
    } else if (strcmp(path, "/api/status") == 0 ||
               strcmp(path, "/api/registers") == 0) {
        /* Both need the same snapshot, so they share the acquire; which one is
         * being answered is then spelled out rather than derived from a
         * character offset that happens to differ. */
        mb_snapshot_t snap;
        if (!mb_store_snapshot(c->store, &snap, STORE_TIMEOUT_MS)) {
            free(json);
            return send_error(c->ops, fd, "503 Service Unavailable",
                              "data model busy");
        }
        n = (strcmp(path, "/api/status") == 0)
                ? build_status(&snap, json, JSON_BUF_SIZE)
                : build_registers(&snap, json, JSON_BUF_SIZE);
    } else {
        free(json);
        return send_error(c->ops, fd, "404 Not Found", "no such endpoint");
    }

    int rc;
    if (n < 0 || n >= JSON_BUF_SIZE) {
        ESP_LOGE(TAG, "%s needs %d bytes, buffer is %d", path, n, JSON_BUF_SIZE);
        rc = send_error(c->ops, fd, "500 Internal Server Error",
                        "response too large");
    } else {
        rc = send_response(c->ops, fd, "200 OK", "application/json", json,
                           (size_t)n);
    }
    free(json);
    return rc;
}

/*
 * Read one request and act on it.
 *
 * A POST body may not have arrived with the headers, so the read continues
 * until Content-Length bytes are in hand. Requests larger than the buffer are
 * refused rather than truncated: half a JSON object parses into something, and
 * something is worse than nothing when it is about to become the device's IP.
 */
static void serve_one(const web_ctx_t *c, int fd)
{
    char *buf = malloc(REQUEST_BUF_SIZE);
    if (buf == NULL) {
        return;
    }

    size_t got = 0;
    const char *body = NULL;
    long content_length = 0;

    /*
     * An absolute deadline as well as a per-read timeout.
     *
     * The per-read timeout alone bounds nothing: a client sending one byte just
     * inside it keeps resetting the clock, and with a small pool of listening
     * sockets one such client is enough to make the page stop answering. The
     * deadline is what makes a slow request finite.
     */
    int64_t deadline = esp_timer_get_time() + (int64_t)REQUEST_DEADLINE_MS * 1000;

    for (;;) {
        if (esp_timer_get_time() > deadline) {
            ESP_LOGW(TAG, "request abandoned after %d ms", REQUEST_DEADLINE_MS);
            send_error(c->ops, fd, "408 Request Timeout", "took too long");
            free(buf);
            return;
        }

        /* Whatever has arrived, not one byte at a time. The blank line that ends
         * the headers is found by scanning what was read rather than by asking
         * the socket for each character, which on this chip is an SPI round
         * trip per byte. */
        int n = mb_transport_recv_some(c->ops, fd, buf + got,
                                       REQUEST_BUF_SIZE - got - 1,
                                       REQUEST_TIMEOUT_MS);
        if (n < 0) {
            free(buf);
            return;
        }
        if (n == 0) {
            continue;                   /* nothing yet; the deadline still runs */
        }
        got += (size_t)n;
        buf[got] = '\0';

        if (body == NULL) {
            char *end = strstr(buf, "\r\n\r\n");
            if (end != NULL) {
                body = end + 4;
                const char *cl = strcasestr(buf, "content-length:");
                if (cl != NULL) {
                    content_length = strtol(cl + 15, NULL, 10);
                }
            }
        }
        if (body != NULL &&
            (long)(got - (size_t)(body - buf)) >= content_length) {
            break;
        }
        if (got + 2 >= REQUEST_BUF_SIZE) {
            send_error(c->ops, fd, "413 Payload Too Large", "request too large");
            free(buf);
            return;
        }
    }

    char method[8] = {0};
    char path[64] = {0};
    if (sscanf(buf, "%7s %63s", method, path) != 2) {
        send_error(c->ops, fd, "400 Bad Request", "malformed request line");
        free(buf);
        return;
    }

    /* Query strings are not used by this UI; trimming one keeps a cache-busting
     * suffix from turning into a 404. */
    char *question = strchr(path, '?');
    if (question != NULL) {
        *question = '\0';
    }

    route(c, fd, method, path, body ? body : "");
    free(buf);
}

static void web_server_task(void *arg)
{
    web_ctx_t *c = (web_ctx_t *)arg;

    ESP_LOGI(TAG, "waiting for link...");
    while (!c->is_up()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    int listeners[WEB_LISTENERS];
    int idle_passes[WEB_LISTENERS] = {0};
    int opened = 0;
    for (int i = 0; i < WEB_LISTENERS; i++) {
        int fd = mb_transport_listen(c->ops, c->port);
        if (fd < 0) {
            /* Fewer listeners means a slower page, not a broken one, and the
             * Modbus server has first claim on the chip's sockets. */
            ESP_LOGW(TAG, "listener %d of %d failed", i + 1, WEB_LISTENERS);
            break;
        }
        listeners[opened++] = fd;
    }
    if (opened == 0) {
        ESP_LOGE(TAG, "cannot listen on %u", c->port);
        free(c);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "web UI on port %u, %d listener%s", c->port, opened,
             opened == 1 ? "" : "s");

    while (!c->stop) {
        for (int i = 0; i < opened && !c->stop; i++) {
            if (listeners[i] < 0) {
                /* Reopened below when a socket frees; accepting on -1 would
                 * just fail immediately, every pass. */
                listeners[i] = mb_transport_listen(c->ops, c->port);
                if (listeners[i] < 0) {
                    continue;
                }
                ESP_LOGI(TAG, "listener %d is back", i);
            }

            int fd = mb_transport_accept(c->ops, listeners[i],
                                         ACCEPT_TIMEOUT_MS);
            if (fd == 0) {
                /* Nothing arrived -- which is either an idle server or a
                 * listener stuck in CLOSE_WAIT, and from here the two look
                 * identical. Rebuilding after a while costs nothing in the
                 * first case and is the only escape from the second. */
                if (++idle_passes[i] >= RECYCLE_AFTER) {
                    idle_passes[i] = 0;
                    mb_transport_close(c->ops, listeners[i]);
                    listeners[i] = mb_transport_listen(c->ops, c->port);
                    if (listeners[i] < 0) {
                        ESP_LOGW(TAG, "listener %d did not come back", i);
                    }
                }
                continue;
            }
            idle_passes[i] = 0;
            if (fd < 0) {
                /* Drop this listener and try to open a replacement. If that
                 * fails the slot stays -1 and is skipped from here on rather
                 * than being accepted on again -- accept() on -1 returns an
                 * error every time, which would turn one dead listener into a
                 * loop that spends the whole task doing nothing else. The
                 * remaining listeners keep serving, and the retry above picks
                 * the slot back up whenever a socket frees. */
                ESP_LOGE(TAG, "listener %d failed; reopening", i);
                mb_transport_close(c->ops, listeners[i]);
                listeners[i] = -1;
                vTaskDelay(pdMS_TO_TICKS(LISTEN_RETRY_MS));

                listeners[i] = mb_transport_listen(c->ops, c->port);
                if (listeners[i] < 0) {
                    ESP_LOGW(TAG, "listener %d is gone; serving on %d", i,
                             opened - 1);
                }
                continue;
            }

            serve_one(c, fd);

            /* One request per connection, so the socket goes back immediately.
             * On the TOE that is also what re-arms this listener. */
            mb_transport_close(c->ops, fd);
        }
    }

    for (int i = 0; i < opened; i++) {
        if (listeners[i] >= 0) {
            mb_transport_close(c->ops, listeners[i]);
        }
    }
    ESP_LOGI(TAG, "stopped");

    /* web_server_stop() is waiting on this and owns the free. */
    c->finished = true;
    vTaskDelete(NULL);
}

web_server_t *web_server_start(const void *ops, uint16_t port,
                               mb_store_t *store, bool (*is_up)(void))
{
    web_ctx_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return NULL;
    }
    c->ops = ops;
    c->port = port;
    c->store = store;
    c->is_up = is_up;

    /* 5 KB: the request buffer is malloc'd but sscanf, snprintf and the JSON
     * builders all work on this stack. */
    if (xTaskCreate(web_server_task, "web", 5120, c, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        free(c);
        return NULL;
    }
    return c;
}

bool web_server_stop(web_server_t *server, uint32_t timeout_ms)
{
    if (server == NULL) {
        return true;
    }
    server->stop = true;

    for (uint32_t waited = 0; waited < timeout_ms; waited += 20) {
        if (server->finished) {
            free(server);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGE(TAG, "did not stop within %u ms", (unsigned)timeout_ms);
    return false;
}
