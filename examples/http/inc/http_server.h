/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Backend-neutral HTTP/1.1 server. The socket entry points are injected via the
 * component's net_sock_ops_t vtable, so the same server drives either stack:
 *   - Ethernet: net_eth_ops (plain lwip_*). With WSM_DRIVER_SOCKET_WRAP=1 these
 *     are redirected to the WIZnet hardware sockets by -Wl,--wrap (so every call
 *     lands in __wrap_lwip_*); with =0 they are the software LwIP over esp_eth
 *     (so every call lands in lwip_*). Correct either way, no #if needed.
 *   - Wi-Fi: net_wifi_ops, which bypasses that wrap to reach the real LwIP.
 *
 * Nothing here calls the ioLibrary httpServer (Internet/httpServer): those
 * clients talk to the chip's socket registers directly, so --wrap has nothing to
 * intercept and one source could not serve both backends. http_server.c speaks HTTP/1.1
 * over ops->recv()/ops->send() instead.
 */
#ifndef HTTP_H
#define HTTP_H

#include <stdbool.h>
#include <stdint.h>

#include "net_sock_ops.h"   /* net_sock_ops_t, net_eth_ops, net_wifi_ops */

/*
 * Spawn a FreeRTOS task that waits until is_up() reports the interface ready,
 * then serves HTTP clients forever: accept -> read request -> respond -> close.
 *   name   - short label; also the task name and log prefix (e.g. "eth")
 *   ops    - socket vtable for this interface
 *   port   - TCP port to listen on
 *   is_up  - predicate the task polls for link readiness
 */
void http_server_start(const char *name, const net_sock_ops_t *ops,
                       uint16_t port, bool (*is_up)(void));

#endif /* HTTP_H */
