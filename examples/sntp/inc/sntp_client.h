/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Backend-neutral SNTP (RFC 4330) client, shared by the Ethernet (WIZnet chip)
 * and Wi-Fi paths. The socket entry points are injected via a vtable so the
 * exact same query logic drives either stack:
 *   - Ethernet: plain lwIP BSD ops (lwip_socket/...). With WSM_DRIVER_SOCKET_WRAP=1
 *     these are redirected to the WIZnet hardware sockets by -Wl,--wrap
 *     (wiztoe_wrap.c); with =0 they are the software LwIP over esp_eth.
 *   - Wi-Fi: with WSM_DRIVER_SOCKET_WRAP=1 the __real_lwip_* symbols (bypassing
 *     --wrap) so the traffic goes to the REAL software LwIP. See wifi_sntp.c.
 *
 * NOTE: this replaces ioLibrary's SNTP_init()/SNTP_run(). That implementation
 * drives a hardware socket number directly, so it never passes through the BSD
 * socket layer the --wrap intercepts and cannot run on Wi-Fi at all. The NTP
 * exchange itself is one 48-byte datagram each way, so doing it over plain BSD
 * UDP costs little and works identically on both interfaces. It also keeps
 * ioLibrary's "sntp.h" out of the include path, where it collides with lwIP's.
 *
 * Deliberately named sntp_client.h, NOT sntp.h: this example's inc/ directory is
 * on the include path and an "sntp.h" here would shadow lwIP's own header.
 */
#ifndef SNTP_CLIENT_H
#define SNTP_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "lwip/sockets.h"   /* struct sockaddr, socklen_t, ssize_t */

/* Socket ops injected into the SNTP client (signatures match lwip_*). */
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
} sntp_ops_t;

/* Standard lwIP BSD socket vtable, used by the Ethernet client. */
extern const sntp_ops_t sntp_lwip_ops;

/* Wi-Fi socket vtable (defined in wifi_sntp.c) - the __real_lwip_* set when the
 * wrap is active, so Wi-Fi traffic reaches the real software LwIP stack. */
extern const sntp_ops_t wifi_sntp_ops;

/*
 * Spawn a task that waits until is_up() reports the interface ready, queries
 * `server_ip` from local UDP port `local_port`, prints the time and then idles
 * (one-shot, like the original example, but with SNTP_RETRY_COUNT attempts).
 * Ethernet and Wi-Fi are started with identical calls.
 *
 *   name       - short label; also the task name and log tag ("eth" / "wifi")
 *   ops        - socket vtable for this interface
 *   server_ip  - NTP server as an IPv4 literal string
 *   local_port - local UDP port to bind
 *   is_up      - predicate the task polls for link/IP readiness
 */
void sntp_client_start(const char *name, const sntp_ops_t *ops,
                       const char *server_ip, uint16_t local_port,
                       bool (*is_up)(void));

#endif /* SNTP_CLIENT_H */
