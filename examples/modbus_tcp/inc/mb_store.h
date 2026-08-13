/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * The Modbus data model, shared between the Modbus server and the web UI.
 *
 * Before the web UI existed each server task owned its own mb_datastore_t and
 * nothing was shared, so no lock was needed. A browser reading the same
 * registers a master is writing changes that: the two arrive on different tasks
 * and a 125-register read must not tear.
 *
 * The lock is held for the length of one PDU or one snapshot copy and never
 * across a socket call. That matters more than it looks: the Modbus task can sit
 * in send() for as long as a slow master takes to read, and a browser poll
 * waiting behind that would stall the whole page.
 *
 * Counters live here rather than in mb_server.c for the same reason -- the page
 * shows them, so they are shared state, and putting them behind the same lock
 * means one snapshot gives the UI a consistent view of registers and traffic
 * together rather than two readings taken a moment apart.
 */
#ifndef MB_STORE_H
#define MB_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "mb_core.h"

/* Traffic counters, maintained by whichever server owns this store. */
typedef struct {
    uint32_t requests;        /* PDUs executed, exceptions included    */
    uint32_t exceptions;      /* how many of those were refused        */
    uint32_t sessions;        /* masters accepted since boot           */
    uint8_t  last_function;   /* function code of the last request     */
    uint8_t  last_exception;  /* 0 when the last request was accepted  */
    bool     client_connected;
    bool     running;
} mb_stats_t;

/*
 * A consistent copy of everything the UI shows. Taken under the lock and read
 * without it, so JSON can be built at leisure while the Modbus task carries on.
 */
typedef struct {
    mb_datastore_t data;
    mb_stats_t     stats;
} mb_snapshot_t;

/* Create the store, seed the data model with the demo pattern, and return it.
 * Returns NULL if the lock could not be created. */
struct mb_store;
typedef struct mb_store mb_store_t;

mb_store_t *mb_store_create(void);

/*
 * Borrow the data model for one operation. Returns NULL if the lock could not
 * be taken within `timeout_ms`, which the caller should treat as "skip this
 * request" rather than as a reason to drop a connection.
 */
mb_datastore_t *mb_store_acquire(mb_store_t *store, uint32_t timeout_ms);
void            mb_store_release(mb_store_t *store);

/* Record the outcome of one executed PDU. */
void mb_store_note_request(mb_store_t *store, uint8_t function,
                           uint8_t exception);

/* Session and lifecycle bookkeeping, for the page's status line. */
void mb_store_note_client(mb_store_t *store, bool connected);
void mb_store_note_running(mb_store_t *store, bool running);

/* Copy registers and counters together. Returns false only if the lock timed
 * out, in which case `out` is untouched. */
bool mb_store_snapshot(mb_store_t *store, mb_snapshot_t *out,
                       uint32_t timeout_ms);

#endif /* MB_STORE_H */
