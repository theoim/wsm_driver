/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Backend-neutral TFTP read client. The socket entry points are injected via the
 * component's net_sock_ops_t vtable, so the same client drives either stack:
 *   - Ethernet: net_eth_ops (plain lwip_*). With WSM_DRIVER_SOCKET_WRAP=1 these
 *     are redirected to the WIZnet hardware sockets by -Wl,--wrap; with =0 they
 *     are the software LwIP over esp_eth. Correct either way, no #if needed.
 *   - Wi-Fi: net_wifi_ops, which bypasses that wrap to reach the real LwIP.
 *
 * The protocol itself is the ioLibrary implementation, carried in the example
 * (tftp_core.c) and reached only through tftp_transport.h -- see that header for
 * why the seam exists.
 */
#ifndef TFTP_CLIENT_H
#define TFTP_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "net_sock_ops.h"   /* net_sock_ops_t, net_eth_ops, net_wifi_ops */

/*
 * Spawn a FreeRTOS task that waits until is_up() reports the interface ready,
 * then reads `filename` from the TFTP server at `server_ip` and reports the
 * result.
 *   name       - short label; also the task name and log prefix (e.g. "eth")
 *   ops        - socket vtable for this interface
 *   server_ip  - TFTP server, dotted quad (e.g. "192.168.11.4")
 *   filename   - file to request
 *   is_up      - predicate the task polls for link readiness
 *
 * Only one transfer may be in flight: tftp_core.c keeps its state in globals,
 * as the ioLibrary original does. Starting a second client concurrently would
 * share that state, so this example runs one interface at a time.
 */
void tftp_client_start(const char *name, const net_sock_ops_t *ops,
                       const char *server_ip, const char *filename,
                       bool (*is_up)(void));

#endif /* TFTP_CLIENT_H */
