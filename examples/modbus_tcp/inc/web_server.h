/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * The web UI: a dashboard over the Modbus data model and a settings page.
 *
 * Runs on the same socket vtable as the Modbus server, so on the TOE both are
 * hardware sockets on the same chip. That is the constraint worth knowing about
 * before reading the implementation: the chip has eight, the Modbus server holds
 * one listener plus its session, and this takes a small pool of its own.
 *
 * One request per connection, GET and POST only, no keep-alive. A settings page
 * polled once a second does not need more, and every socket this does not hold
 * is one the Modbus side can have.
 */
#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#include "mb_store.h"

struct web_server;
typedef struct web_server web_server_t;

/*
 * Spawn the web server task. Returns a handle, or NULL.
 *   ops   - socket vtable for the interface it serves on
 *   port  - TCP port (WEB_PORT; not configurable, see net_config.h)
 *   store - the shared data model it reports
 *   is_up - predicate the task polls for link readiness
 */
web_server_t *web_server_start(const void *ops, uint16_t port,
                               mb_store_t *store, bool (*is_up)(void));

/*
 * Stop the server and wait for its task, up to `timeout_ms`.
 *
 * Needed because changing the device's address means every socket bound to the
 * old one has to be gone first. The Modbus listener is the obvious one; these
 * are the ones it is easy to forget, and a listening socket that outlives the
 * address it was opened on is the state the chip handles worst.
 *
 * Returns false on timeout, in which case the handle is leaked rather than
 * freed under a live task.
 */
bool web_server_stop(web_server_t *server, uint32_t timeout_ms);

#endif /* WEB_SERVER_H */
