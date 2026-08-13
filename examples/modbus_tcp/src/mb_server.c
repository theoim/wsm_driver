/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Serves one Modbus TCP master at a time: accept, then read/execute/reply until
 * the master goes away, then back to accepting.
 *
 * Backend-neutral -- everything network-facing goes through the vtable handed
 * to mb_server_start() and reaches the wire via mb_transport.c.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mb_core.h"
#include "mb_server.h"
#include "mb_store.h"
#include "mb_transport.h"

static const char *TAG = "mb_server";

/* How long accept() and the request read block before returning to the loop.
 * Long enough that an idle server is not spinning, short enough that a log line
 * is not stuck behind it. A Modbus master may sit silent between polls, so the
 * request timeout must not be mistaken for a dead connection. */
#define ACCEPT_TIMEOUT_MS   1000
#define REQUEST_TIMEOUT_MS  1000

/* Pause before rebuilding a listening socket that failed, so a persistent fault
 * logs at a readable rate instead of filling the console. */
#define LISTEN_RETRY_MS     1000

/* How long a request will wait for the data model. Generous, because the only
 * other holder is a snapshot copy of ~400 bytes; if it ever times out something
 * is wrong rather than merely busy. */
#define STORE_TIMEOUT_MS    500

struct mb_server {
    const char  *name;
    const void  *ops;
    uint16_t     port;
    mb_store_t  *store;
    bool       (*is_up)(void);

    /* Set by mb_server_stop(), read by the task once per accept timeout. A
     * plain bool rather than an event group: the task already wakes every
     * ACCEPT_TIMEOUT_MS, so there is nothing to signal it with that it is not
     * about to notice anyway. */
    volatile bool stop;
    volatile bool finished;
};

typedef struct mb_server mb_server_ctx_t;

/*
 * Read one ADU, execute it, write the reply.
 *
 * Returns 0 to keep the connection, -1 to drop it. Two reads rather than one:
 * the MBAP header carries the length of what follows, so this never has to
 * guess how much is in flight or resynchronise on a stream that got out of step.
 */
static int serve_request(const char *name, const void *ops, int fd,
                         mb_store_t *store)
{
    uint8_t adu[MB_ADU_MAX];

    int n = mb_transport_recv_exact(ops, fd, adu, MB_MBAP_LEN,
                                    REQUEST_TIMEOUT_MS);
    if (n == 0) {
        return 0;                       /* master is idle between polls */
    }
    if (n < 0) {
        return -1;
    }

    uint16_t tid    = (uint16_t)((adu[0] << 8) | adu[1]);
    uint16_t pid    = (uint16_t)((adu[2] << 8) | adu[3]);
    uint16_t length = (uint16_t)((adu[4] << 8) | adu[5]);
    uint8_t  uid    = adu[6];

    /* PID is 0 for Modbus and nothing else is defined; a non-zero one means
     * this is not a Modbus master, so there is no sensible reply to send. */
    if (pid != 0) {
        ESP_LOGW(TAG, "[%s] protocol id %u is not Modbus", name, pid);
        return -1;
    }

    /* Length counts the unit id plus the PDU. Under 2 leaves no function code;
     * over the maximum would not fit a legal PDU and cannot be trusted enough
     * to read that many bytes off the socket. */
    if (length < 2 || length > MB_PDU_MAX + 1) {
        ESP_LOGW(TAG, "[%s] bad MBAP length %u", name, length);
        return -1;
    }

    size_t pdu_len = (size_t)length - 1;
    if (mb_transport_recv_exact(ops, fd, adu + MB_MBAP_LEN, pdu_len,
                                REQUEST_TIMEOUT_MS) < 0) {
        return -1;
    }

    /* The lock covers the execution only. Everything before it was reading the
     * socket and everything after is writing to it, and holding a mutex across
     * either would let one slow master stall the web UI's poll. */
    mb_datastore_t *ds = mb_store_acquire(store, STORE_TIMEOUT_MS);
    if (ds == NULL) {
        /* Drop the connection rather than the request. The ADU has already been
         * read off the socket, so returning 0 would leave the master waiting
         * for a reply that is never coming while the stream carries on -- and
         * its next request would then be answered with this one's transaction
         * id. Closing makes the failure obvious and the master reconnects. */
        ESP_LOGE(TAG, "[%s] data model busy; closing the session", name);
        return -1;
    }
    uint8_t resp_pdu[MB_PDU_MAX];
    size_t resp_len = mb_pdu_execute(ds, adu + MB_MBAP_LEN, pdu_len, resp_pdu);
    mb_store_release(store);

    mb_store_note_request(store, adu[MB_MBAP_LEN],
                          (resp_pdu[0] & 0x80) ? resp_pdu[1] : 0);

    if (resp_pdu[0] & 0x80) {
        ESP_LOGW(TAG, "[%s] function 0x%02X refused: exception 0x%02X",
                 name, adu[MB_MBAP_LEN], resp_pdu[1]);
    } else {
        ESP_LOGI(TAG, "[%s] function 0x%02X -> %u bytes",
                 name, adu[MB_MBAP_LEN], (unsigned)resp_len);
    }

    /* Reply with the request's own transaction id and unit id: the master pairs
     * replies to requests by TID, and a unit id it did not send would look like
     * a reply for a different slave. */
    uint8_t out[MB_ADU_MAX];
    out[0] = (uint8_t)(tid >> 8);
    out[1] = (uint8_t)(tid & 0xFF);
    out[2] = 0;
    out[3] = 0;
    out[4] = (uint8_t)((resp_len + 1) >> 8);
    out[5] = (uint8_t)((resp_len + 1) & 0xFF);
    out[6] = uid;
    memcpy(out + MB_MBAP_LEN, resp_pdu, resp_len);

    return mb_transport_send(ops, fd, out, MB_MBAP_LEN + resp_len);
}

static void mb_server_task(void *arg)
{
    mb_server_ctx_t *c = (mb_server_ctx_t *)arg;

    ESP_LOGI(TAG, "[%s] waiting for link...", c->name);
    while (!c->is_up() && !c->stop) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    int listen_fd = -1;
    if (!c->stop) {
        listen_fd = mb_transport_listen(c->ops, c->port);
        if (listen_fd < 0) {
            ESP_LOGE(TAG, "[%s] cannot listen on %u", c->name, c->port);
            goto done;
        }
        ESP_LOGI(TAG, "[%s] Modbus TCP server on port %u", c->name, c->port);
        mb_store_note_running(c->store, true);
    }

    while (!c->stop) {
        int fd = mb_transport_accept(c->ops, listen_fd, ACCEPT_TIMEOUT_MS);
        if (fd == 0) {
            continue;                   /* nobody yet, or time to re-check stop */
        }
        if (fd < 0) {
            /* The listening socket itself failed, so polling it again would
             * spin on the same error. Rebuild it rather than ending the task:
             * a server that quietly stops accepting looks identical to an idle
             * one from the outside, and only a reboot would recover it. */
            ESP_LOGE(TAG, "[%s] accept failed, reopening the listener", c->name);
            mb_transport_close(c->ops, listen_fd);
            listen_fd = -1;
            vTaskDelay(pdMS_TO_TICKS(LISTEN_RETRY_MS));
            if (c->stop) {
                break;
            }

            listen_fd = mb_transport_listen(c->ops, c->port);
            if (listen_fd < 0) {
                ESP_LOGE(TAG, "[%s] cannot reopen port %u, giving up",
                         c->name, c->port);
                goto done;
            }
            ESP_LOGI(TAG, "[%s] listening again on port %u", c->name, c->port);
            continue;
        }

        ESP_LOGI(TAG, "[%s] session open", c->name);
        mb_store_note_client(c->store, true);

        /* The stop flag is checked between requests rather than only between
         * sessions. A Modbus master holds one connection for as long as it
         * likes, so waiting for the session to end could mean waiting forever,
         * and a port change would appear to hang. */
        while (!c->stop && serve_request(c->name, c->ops, fd, c->store) == 0) {
            /* Modbus TCP keeps the connection open across polls; a master may
             * issue thousands of requests on one socket. */
        }

        mb_store_note_client(c->store, false);
        ESP_LOGI(TAG, "[%s] session closed", c->name);

        /* Closing an accepted connection is also what re-arms the listener on
         * the TOE, where the two are the same socket -- see mb_transport.h. */
        mb_transport_close(c->ops, fd);
    }

done:
    if (listen_fd >= 0) {
        mb_transport_close(c->ops, listen_fd);
    }
    mb_store_note_running(c->store, false);
    ESP_LOGI(TAG, "[%s] stopped", c->name);

    /* The handle outlives the task by design: mb_server_stop() is waiting on
     * this flag and frees it, so the task must not. */
    c->finished = true;
    vTaskDelete(NULL);
}

mb_server_t *mb_server_start(const char *name, const void *ops, uint16_t port,
                             mb_store_t *store, bool (*is_up)(void))
{
    mb_server_ctx_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        ESP_LOGE(TAG, "[%s] out of memory", name);
        return NULL;
    }
    c->name = name;
    c->ops = ops;
    c->port = port;
    c->store = store;
    c->is_up = is_up;

    if (xTaskCreate(mb_server_task, name, 4096, c, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "[%s] xTaskCreate failed", name);
        free(c);
        return NULL;
    }
    return c;
}

bool mb_server_stop(mb_server_t *server, uint32_t timeout_ms)
{
    if (server == NULL) {
        return true;
    }

    server->stop = true;

    /* Poll rather than block on a semaphore: the task can be anywhere from an
     * accept timeout to a partially-read ADU, and the wait is bounded by the
     * longest of those rather than by anything worth signalling. */
    for (uint32_t waited = 0; waited < timeout_ms; waited += 20) {
        if (server->finished) {
            free(server);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGE(TAG, "[%s] did not stop within %u ms; leaking the handle rather "
             "than freeing it under a live task", server->name,
             (unsigned)timeout_ms);
    return false;
}

uint16_t mb_server_port(const mb_server_t *server)
{
    return (server != NULL) ? server->port : 0;
}
