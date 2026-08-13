/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * The shared Modbus data model (see mb_store.h).
 */
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "mb_store.h"

struct mb_store {
    SemaphoreHandle_t lock;
    mb_datastore_t    data;
    mb_stats_t        stats;
};

mb_store_t *mb_store_create(void)
{
    mb_store_t *store = calloc(1, sizeof(*store));
    if (store == NULL) {
        return NULL;
    }

    store->lock = xSemaphoreCreateMutex();
    if (store->lock == NULL) {
        free(store);
        return NULL;
    }

    mb_datastore_init(&store->data);
    return store;
}

mb_datastore_t *mb_store_acquire(mb_store_t *store, uint32_t timeout_ms)
{
    if (store == NULL ||
        xSemaphoreTake(store->lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return NULL;
    }
    return &store->data;
}

void mb_store_release(mb_store_t *store)
{
    if (store != NULL) {
        xSemaphoreGive(store->lock);
    }
}

/*
 * The counters are updated under the same lock as the registers even though a
 * uint32_t increment is atomic on this core. The point is not the increment but
 * the pairing: a snapshot taken between "registers written" and "counter
 * incremented" would show the page a write that had not been counted, and a
 * reader trying to reconcile the two would be chasing a race that does not
 * exist in the data.
 */
static void with_lock(mb_store_t *store, void (*fn)(mb_stats_t *, void *),
                      void *arg)
{
    if (store == NULL ||
        xSemaphoreTake(store->lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    fn(&store->stats, arg);
    xSemaphoreGive(store->lock);
}

typedef struct {
    uint8_t function;
    uint8_t exception;
} note_request_arg_t;

static void do_note_request(mb_stats_t *stats, void *arg)
{
    const note_request_arg_t *a = arg;

    stats->requests++;
    stats->last_function = a->function;
    stats->last_exception = a->exception;
    if (a->exception != 0) {
        stats->exceptions++;
    }
}

void mb_store_note_request(mb_store_t *store, uint8_t function,
                           uint8_t exception)
{
    note_request_arg_t arg = { function, exception };
    with_lock(store, do_note_request, &arg);
}

static void do_note_client(mb_stats_t *stats, void *arg)
{
    bool connected = *(const bool *)arg;

    stats->client_connected = connected;
    if (connected) {
        stats->sessions++;
    }
}

void mb_store_note_client(mb_store_t *store, bool connected)
{
    with_lock(store, do_note_client, &connected);
}

static void do_note_running(mb_stats_t *stats, void *arg)
{
    stats->running = *(const bool *)arg;
}

void mb_store_note_running(mb_store_t *store, bool running)
{
    with_lock(store, do_note_running, &running);
}

bool mb_store_snapshot(mb_store_t *store, mb_snapshot_t *out,
                       uint32_t timeout_ms)
{
    if (store == NULL ||
        xSemaphoreTake(store->lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }

    /* One memcpy of the whole model rather than a field-by-field read: it is
     * about 400 bytes, and copying it wholesale keeps the lock held for a
     * predictable and very short time regardless of what the caller wants. */
    memcpy(&out->data, &store->data, sizeof(out->data));
    memcpy(&out->stats, &store->stats, sizeof(out->stats));

    xSemaphoreGive(store->lock);
    return true;
}
