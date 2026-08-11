/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Backend-neutral TLS client, shared by the Ethernet (WIZnet chip) and Wi-Fi
 * paths. The socket entry points are injected via a vtable so the exact same
 * TLS logic drives either stack:
 *   - Ethernet: plain lwIP BSD ops (lwip_socket/...). With WSM_DRIVER_SOCKET_WRAP=1
 *     these are redirected to the WIZnet hardware sockets by -Wl,--wrap
 *     (wiztoe_wrap.c); with =0 they are the software LwIP over esp_eth.
 *   - Wi-Fi: with WSM_DRIVER_SOCKET_WRAP=1 the __real_lwip_* symbols (bypassing
 *     --wrap) so the traffic goes to the REAL software LwIP. See wifi_ssl_client.c.
 *
 * mbedTLS sits on top unchanged: its BIO callbacks are wired to whichever
 * vtable the caller passed, so the same handshake code runs over the chip's
 * hardware TCP or over Wi-Fi.
 */
#ifndef SSL_CLIENT_H
#define SSL_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "lwip/sockets.h"   /* struct sockaddr, socklen_t, ssize_t */

/* Socket ops injected into the TLS client (signatures match lwip_*). */
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
} ssl_ops_t;

/* Standard lwIP BSD socket vtable, used by the Ethernet client. */
extern const ssl_ops_t ssl_lwip_ops;

/* Wi-Fi socket vtable (defined in wifi_ssl_client.c) - the __real_lwip_* set
 * when the wrap is active, so Wi-Fi traffic reaches the real software LwIP. */
extern const ssl_ops_t wifi_ssl_ops;

/*
 * Spawn a task that waits until is_up() reports the interface ready, then keeps
 * a TLS session to target_ip:target_port: connect, handshake, send the hello
 * message, print whatever the server sends, and reconnect after a drop.
 * Ethernet and Wi-Fi are started with identical calls.
 *
 *   name        - short label; also the task name and log tag ("eth" / "wifi")
 *   ops         - socket vtable for this interface
 *   target_ip   - server as an IPv4 literal string
 *   target_port - server port
 *   is_up       - predicate the task polls for link/IP readiness
 */
void ssl_client_start(const char *name, const ssl_ops_t *ops,
                      const char *target_ip, uint16_t target_port,
                      bool (*is_up)(void));

#endif /* SSL_CLIENT_H */
