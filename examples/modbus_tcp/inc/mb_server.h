/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Backend-neutral Modbus TCP server (slave).
 *
 * Takes a socket vtable, so the same code drives either stack:
 *   - Ethernet: net_eth_ops (plain lwip_*). With WSM_DRIVER_SOCKET_WRAP=1 those
 *     are redirected to the WIZnet hardware sockets by -Wl,--wrap; with =0 they
 *     are the software LwIP over esp_eth. Correct either way, no #if needed.
 *   - Wi-Fi: net_wifi_ops, which bypasses that wrap to reach the real LwIP.
 *
 * One master at a time. That is a property of the chip, not a shortcut: the TOE
 * has no separate accepted socket, so serving N masters means N listening
 * hardware sockets (see examples/tcp_server_multi_socket), which would make
 * this example backend-specific. mb_transport.h has the detail.
 *
 * Each interface gets its own mb_datastore_t. Sharing one across both would
 * need a lock, and the point of the example is the protocol, not the mutex --
 * writing over Ethernet and reading over Wi-Fi are two independent slaves here.
 */
#ifndef MB_SERVER_H
#define MB_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#include "mb_store.h"

/*
 * A running server. Opaque because the only things a caller can do with one are
 * stop it and restart it on a different port -- everything else it owns (the
 * task, the listening socket, the current session) is its own business.
 */
struct mb_server;
typedef struct mb_server mb_server_t;

/*
 * Spawn a task that waits for the interface, then serves Modbus TCP masters on
 * `port`. Returns a handle, or NULL if the task could not be created.
 *   name   - short label; also the task name and log prefix (e.g. "eth")
 *   ops    - socket vtable for this interface
 *   port   - TCP port to listen on (502 is the registered Modbus port)
 *   store  - the data model to serve; may be shared with the web UI
 *   is_up  - predicate the task polls for link readiness
 */
mb_server_t *mb_server_start(const char *name, const void *ops, uint16_t port,
                             mb_store_t *store, bool (*is_up)(void));

/*
 * Stop the server and wait for its task to finish, up to `timeout_ms`.
 *
 * The wait is the point. Restarting on a new port has to know the old listening
 * socket is gone, and on the TOE that socket is a hardware resource -- coming
 * back before it is released would either fail to bind or, worse, hand the new
 * server a socket the old task is still writing to. Returns false if the task
 * did not finish in time, in which case the handle is leaked deliberately
 * rather than freed underneath a task that is still using it.
 */
bool mb_server_stop(mb_server_t *server, uint32_t timeout_ms);

/* Current port, for a caller that wants to report what is actually running
 * rather than what was last requested. */
uint16_t mb_server_port(const mb_server_t *server);

#endif /* MB_SERVER_H */
