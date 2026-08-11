/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Backend-neutral loopback (echo) engine, shared by the Ethernet (W5500) and
 * Wi-Fi paths. The socket entry points are injected via a vtable so the exact
 * same echo logic drives either stack:
 *   - Ethernet: plain lwIP BSD ops (lwip_socket/...). With WSM_DRIVER_SOCKET_WRAP=1
 *     these are redirected to the W5500 hardware sockets by -Wl,--wrap
 *     (wiztoe_wrap.c); with =0 they are the software LwIP over esp_eth.
 *   - Wi-Fi: with WSM_DRIVER_SOCKET_WRAP=1 the __real_lwip_* symbols (bypassing
 *     --wrap) so the traffic goes to the REAL software LwIP; with =0 the same
 *     lwip_* as Ethernet (both share one stack). See wifi_loopback.c.
 *
 * The compile-time LOOPBACK_MODE (TCP server / client / UDP) selects which echo
 * routine loopback_run() dispatches to (override with -DLOOPBACK_MODE=n).
 */
#ifndef LOOPBACK_H
#define LOOPBACK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "lwip/sockets.h"   /* struct sockaddr, socklen_t, ssize_t */

/* Socket ops injected into the echo engine (signatures match lwip_*). */
typedef struct {
    int     (*socket)(int domain, int type, int protocol);
    int     (*bind)(int s, const struct sockaddr *name, socklen_t namelen);
    int     (*listen)(int s, int backlog);
    int     (*accept)(int s, struct sockaddr *addr, socklen_t *addrlen);
    int     (*connect)(int s, const struct sockaddr *name, socklen_t namelen);
    ssize_t (*recv)(int s, void *mem, size_t len, int flags);
    ssize_t (*send)(int s, const void *data, size_t size, int flags);
    ssize_t (*recvfrom)(int s, void *mem, size_t len, int flags,
                        struct sockaddr *from, socklen_t *fromlen);
    ssize_t (*sendto)(int s, const void *data, size_t size, int flags,
                      const struct sockaddr *to, socklen_t tolen);
    int     (*setsockopt)(int s, int level, int optname, const void *optval, socklen_t optlen);
    int     (*close)(int s);
} loopback_ops_t;

/*
 * Standard lwIP BSD socket vtable, used by the Ethernet loopback. These plain
 * lwip_* entry points are redirected to the W5500 hardware sockets by -Wl,--wrap
 * with WSM_DRIVER_SOCKET_WRAP=1, and are software LwIP over esp_eth with =0. (The
 * Wi-Fi loopback uses wifi_loopback_ops instead — see wifi_backend.h.)
 */
extern const loopback_ops_t loopback_lwip_ops;

/*
 * Wi-Fi socket vtable for loopback_start() (defined in wifi_loopback.c). With
 * WSM_DRIVER_SOCKET_WRAP=1 this is the __real_lwip_* set that bypasses the W5500
 * --wrap so Wi-Fi traffic reaches the real software LwIP stack.
 */
extern const loopback_ops_t wifi_loopback_ops;

/*
 * Spawn a FreeRTOS task that waits until is_up() reports the interface ready,
 * then runs the LOOPBACK_MODE echo loop (server / client / UDP) forever.
 * Ethernet and Wi-Fi are started with identical calls — same level, same shape.
 *   name          - short label; also the task name and log tag (e.g. "eth", "wifi")
 *   ops           - socket vtable for this interface
 *   listen_port   - TCP/UDP server listen port (server & UDP modes)
 *   target_ip/port- destination (TCP client mode only)
 *   is_up         - predicate the task polls for link/IP readiness
 */
void loopback_start(const char *name, const loopback_ops_t *ops,
                    uint16_t listen_port, const char *target_ip, uint16_t target_port,
                    bool (*is_up)(void));

#endif /* LOOPBACK_H */
