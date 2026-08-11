/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Backend-neutral TLS echo server. The socket entry points are injected via the
 * component's net_sock_ops_t vtable, so the same server drives either stack:
 *   - Ethernet: net_eth_ops (plain lwip_*). With WSM_DRIVER_SOCKET_WRAP=1 these
 *     are redirected to the WIZnet hardware sockets by -Wl,--wrap; with =0 they
 *     are the software LwIP over esp_eth. Correct either way, no #if needed.
 *   - Wi-Fi: net_wifi_ops, which bypasses that wrap to reach the real LwIP.
 *
 * mbedTLS talks to the socket through a BIO bound to the accepted fd plus the
 * vtable, so no part of the TLS code knows which interface it is running on.
 * Each interface gets its own task and its own complete mbedTLS state — nothing
 * is shared, so the two servers can handshake at the same time.
 */
#ifndef SSL_ECHO_H
#define SSL_ECHO_H

#include <stdbool.h>
#include <stdint.h>

#include "net_sock_ops.h"   /* net_sock_ops_t, net_eth_ops, net_wifi_ops */

/*
 * Spawn a FreeRTOS task that waits until is_up() reports the interface ready,
 * then serves TLS clients forever: accept -> handshake -> banner -> echo.
 *   name   - short label; also the task name and log prefix (e.g. "eth")
 *   ops    - socket vtable for this interface
 *   port   - TCP port to listen on
 *   is_up  - predicate the task polls for link readiness
 */
void ssl_echo_start(const char *name, const net_sock_ops_t *ops,
                    uint16_t port, bool (*is_up)(void));

#endif /* SSL_ECHO_H */
