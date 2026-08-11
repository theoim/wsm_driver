/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Backend-neutral DHCP + DNS engine, shared by the Ethernet (WIZnet chip) and
 * Wi-Fi paths.
 *
 * Like examples/loopback, the protocol logic is written ONCE against a BSD
 * socket vtable (dhcp_sock_ops_t) and the TOE/LwIP choice is made by the LINKER,
 * not by the source:
 *   - dhcp_lwip_ops (dhcp_dns.c) is the plain lwip_* entry points. With
 *     WSM_DRIVER_SOCKET_WRAP=1 (TOE backend) they are --wrap-redirected to the
 *     WIZnet hardware sockets; with =0 (ETH backend) they are the software LwIP
 *     over esp_eth. Correct either way, so no #if.
 *   - dhcp_wifi_sock_ops (netif_dhcp_dns.c) binds __real_lwip_* when the wrap is
 *     active, so Wi-Fi always reaches the software stack its netif sits on.
 * Nothing here calls the ioLibrary DHCP/DNS clients — dhcp_dns.c speaks RFC 2131
 * and RFC 1035 over ops->sock->sendto()/recvfrom() directly.
 *
 * Two things a BSD socket genuinely cannot express, so they stay in a per-
 * interface vtable (dhcp_dns_ops_t):
 *   - prepare()     : the chaddr (MAC) to put in the request, the netif name to
 *                     pin the socket to, and stopping whatever DHCP client the
 *                     stack runs on its own.
 *   - apply_lease() : installing the leased address INTO the stack —
 *                     wizchip_setnetinfo() on TOE, esp_netif_set_ip_info() on
 *                     LwIP. There is no socket call for this.
 */
#ifndef DHCP_DNS_H
#define DHCP_DNS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lwip/sockets.h"   /* struct sockaddr, socklen_t, ssize_t */

/* Outcome of one poll of the DHCP client. */
typedef enum {
    DHCP_DNS_PENDING = 0,   /* still negotiating — keep polling */
    DHCP_DNS_LEASED,        /* address in hand; `info` has been filled in */
    DHCP_DNS_FAILED,        /* a full retransmission round expired */
    DHCP_DNS_CONFLICT,      /* the offered address is already in use */
} dhcp_dns_status_t;

/* Leased IPv4 identity, in the byte-array form wiz_NetInfo uses. */
typedef struct {
    uint8_t  ip[4];
    uint8_t  sn[4];
    uint8_t  gw[4];
    uint8_t  dns[4];
    uint32_t lease_s;       /* 0 when the server sends no lease-time option */
} dhcp_dns_netinfo_t;

/* BSD socket vtable — the ONLY way dhcp_dns.c touches the network, so the whole
 * TOE-vs-LwIP split reduces to which set of symbols these point at. Signatures
 * match lwip_*, same as loopback_ops_t. */
typedef struct {
    int     (*socket)(int domain, int type, int protocol);
    int     (*bind)(int s, const struct sockaddr *name, socklen_t namelen);
    ssize_t (*sendto)(int s, const void *data, size_t size, int flags,
                      const struct sockaddr *to, socklen_t tolen);
    ssize_t (*recvfrom)(int s, void *mem, size_t len, int flags,
                        struct sockaddr *from, socklen_t *fromlen);
    int     (*setsockopt)(int s, int level, int optname, const void *optval, socklen_t optlen);
    int     (*close)(int s);
} dhcp_sock_ops_t;

/* Per-interface hooks for what BSD sockets cannot do (see the file header). */
typedef struct {
    const dhcp_sock_ops_t *sock;

    /* Called once, after is_up(). Fills `mac` with the chaddr to advertise and
     * `ifname` with the LwIP netif name to pin the socket to via
     * SO_BINDTODEVICE (empty string when the stack has no such concept, as on
     * the TOE where the chip IS the interface). Also stops any DHCP client the
     * stack runs by itself, so nothing else is holding UDP port 68. */
    void (*prepare)(uint8_t mac[6], char *ifname, size_t ifname_len);

    /* Install a fresh lease into the stack that owns this interface. */
    void (*apply_lease)(const dhcp_dns_netinfo_t *info);
} dhcp_dns_ops_t;

/* Plain lwip_* — Ethernet. --wrap sends these to the TOE when it is active. */
extern const dhcp_sock_ops_t dhcp_lwip_ops;
/* __real_lwip_* when wrapped, plain lwip_* otherwise — Wi-Fi. */
extern const dhcp_sock_ops_t dhcp_wifi_sock_ops;

/* Ethernet (WIZnet chip) hooks — wizchip_* on TOE, esp_netif on ETH. */
extern const dhcp_dns_ops_t dhcp_dns_eth_ops;
/* Wi-Fi hooks — always esp_netif. */
extern const dhcp_dns_ops_t dhcp_dns_wifi_ops;

/*
 * Spawn a FreeRTOS task that waits until is_up() reports the interface ready,
 * leases an address with the socket-based DHCP client, then resolves `domain`
 * with the socket-based DNS client and keeps polling DHCP so the lease is
 * renewed. Ethernet and Wi-Fi are started with identical calls — same level,
 * same shape; only the vtable differs.
 *   name    - short label; also the task name and log prefix (e.g. "eth", "wifi")
 *   ops     - socket vtable + platform hooks for this interface
 *   domain  - hostname to resolve once the lease is in hand
 *   is_up   - predicate the task polls for link readiness
 */
void dhcp_dns_start(const char *name, const dhcp_dns_ops_t *ops,
                    const char *domain, bool (*is_up)(void));

#endif /* DHCP_DNS_H */
