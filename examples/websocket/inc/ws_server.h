/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Backend-neutral WebSocket echo server.
 *
 * Takes a socket vtable, so the same code drives either stack:
 *   - Ethernet: net_eth_ops (plain lwip_*). With WSM_DRIVER_SOCKET_WRAP=1 those
 *     are redirected to the WIZnet hardware sockets by -Wl,--wrap; with =0 they
 *     are the software LwIP over esp_eth. Correct either way, no #if needed.
 *   - Wi-Fi: net_wifi_ops, which bypasses that wrap to reach the real LwIP.
 *
 * One connection at a time. That is a property of the chip, not a shortcut:
 * the TOE has no separate accepted socket, so serving N clients means N
 * listening hardware sockets (see examples/tcp_server_multi_socket), which
 * would make this example backend-specific. ws_transport.h has the detail.
 */
#ifndef WS_SERVER_H
#define WS_SERVER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Spawn a task that waits for the interface, then serves WebSocket clients on
 * `port` until reset.
 *   name   - short label; also the task name and log prefix (e.g. "eth")
 *   ops    - socket vtable for this interface
 *   port   - TCP port to listen on
 *   is_up  - predicate the task polls for link readiness
 */
void ws_server_start(const char *name, const void *ops, uint16_t port,
                     bool (*is_up)(void));

#endif /* WS_SERVER_H */
