/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Backend-neutral camera streaming server.
 *
 * Takes a socket vtable, so the same code drives either stack:
 *   - Ethernet: net_eth_ops (plain lwip_*). With WSM_DRIVER_SOCKET_WRAP=1 those
 *     are redirected to the WIZnet hardware sockets by -Wl,--wrap; with =0 they
 *     are the software LwIP over esp_eth. Correct either way, no #if needed.
 *   - Wi-Fi: net_wifi_ops, which bypasses that wrap to reach the real LwIP.
 *
 * Start it on both and the same page is served twice from one camera, with a
 * badge naming the link it arrived over. Two browser tabs then show the same
 * sensor going out through hardware TCP/IP and through software TCP/IP, with
 * frame rate and link bandwidth plotted side by side -- which is the comparison
 * the example exists to make.
 *
 * Each interface keeps its own cam_stats_t. Sharing them would average the two
 * stacks together and destroy exactly the number being measured.
 */
#ifndef CAM_SERVER_H
#define CAM_SERVER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Spawn a task that waits for the interface, then serves the page, the stream
 * and the control endpoints on `port` until reset.
 *   name       - short label; also the task name and log prefix ("eth")
 *   link_name  - what the page's badge shows: "TOE", "ETHERNET" or "WI-FI".
 *                The link rather than the stack, because that is what someone
 *                comparing two windows already knows -- and because the page
 *                cannot work the stack out for itself: Ethernet is the TOE or
 *                lwIP depending on how the component was built, so the caller,
 *                which knows, passes it in.
 *   ops        - socket vtable for this interface
 *   port       - TCP port to listen on
 *   listeners  - how many listening sockets to open; see http_transport.h for
 *                why this is a property of the backend and not a preference
 *   is_up      - predicate the task polls for link readiness
 */
void cam_server_start(const char *name, const char *link_name, const void *ops,
                      uint16_t port, int listeners, bool (*is_up)(void));

#endif /* CAM_SERVER_H */
