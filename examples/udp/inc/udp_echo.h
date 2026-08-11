/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Backend-neutral UDP echo engine. The socket entry points are injected via the
 * component's net_sock_ops_t vtable, so the exact same logic can drive either
 * stack:
 *   - Ethernet: net_eth_ops (plain lwip_*). With WSM_DRIVER_SOCKET_WRAP=1 these
 *     are redirected to the WIZnet hardware sockets by -Wl,--wrap; with =0 they
 *     are the software LwIP over esp_eth. Correct either way, no #if needed.
 *   - Wi-Fi: net_wifi_ops, if this example is ever extended to run on both.
 *
 * The role (server / client) is selected in menuconfig:
 *   UDP Example Configuration -> UDP role
 * Both roles echo; they differ only in which local port they bind and in how
 * loudly they log. That matches the ioLibrary reference this example is ported
 * from (loopback_udps / loopback_udpc), where the "client" is likewise an echo
 * responder on an ephemeral port rather than an initiator.
 */
#ifndef UDP_ECHO_H
#define UDP_ECHO_H

#include <stdbool.h>
#include <stdint.h>

#include "net_sock_ops.h"   /* net_sock_ops_t, net_eth_ops, net_wifi_ops */

/*
 * Spawn a FreeRTOS task that waits until is_up() reports the interface ready,
 * then runs the UDP echo loop forever.
 *   name       - short label; also the task name and log prefix (e.g. "eth")
 *   ops        - socket vtable for this interface
 *   bind_port  - local UDP port to bind
 *   is_up      - predicate the task polls for link readiness
 */
void udp_echo_start(const char *name, const net_sock_ops_t *ops,
                    uint16_t bind_port, bool (*is_up)(void));

#endif /* UDP_ECHO_H */
