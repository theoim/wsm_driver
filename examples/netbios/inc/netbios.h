/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Backend-neutral NetBIOS name responder, shared by the Ethernet (WIZnet chip)
 * and Wi-Fi paths.
 *
 * Like examples/loopback and examples/dhcp_dns, every network call goes through
 * the component's BSD socket vtable (net_sock_ops_t) and the TOE/LwIP choice is
 * made by the LINKER, not by the source:
 *   - net_eth_ops  is the plain lwip_* entry points. Under
 *     `Component config -> WIZnet WSM Driver -> Network backend`
 *       TOE (hardware TCP/IP)          -> --wrap redirects them to the chip's
 *                                         hardware sockets (__wrap_lwip_*)
 *       esp_eth MACRAW + software LwIP -> they are the software LwIP (lwip_*)
 *     Correct either way, so netbios.c contains no #if for it.
 *   - net_wifi_ops binds __real_lwip_* when the wrap is active, so Wi-Fi always
 *     reaches the software LwIP stack its netif sits on.
 *
 * The ioLibrary socket API (socket(sn, Sn_MR_UDP, ...) / recvfrom(sn, ...) and
 * the getSn_SR() state machine the original example used) is NOT used: it talks
 * to the chip's socket registers directly, so --wrap has nothing to intercept
 * and one source could not serve both backends. netbios.c speaks the NetBIOS
 * name service over ops->sock->recvfrom() / ops->sock->sendto() instead.
 *
 * Two things a BSD socket cannot express stay in a per-interface vtable
 * (netbios_ops_t): the address to answer queries with, and the netif to pin the
 * listening socket to.
 */
#ifndef NETBIOS_H
#define NETBIOS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "net_sock_ops.h"   /* net_sock_ops_t, net_eth_ops, net_wifi_ops */

/* Per-interface hooks for what BSD sockets cannot do (see the file header). */
typedef struct {
    const net_sock_ops_t *sock;

    /* The IPv4 address this interface answers name queries with — the whole
     * point of the responder, and there is no socket call for it: on the TOE it
     * lives in the chip's registers (wizchip_getnetinfo), on LwIP in the
     * esp_netif. Read per response, so a DHCP renewal is picked up. Returns
     * false while the interface has no address yet. */
    bool (*get_ip)(uint8_t ip[4]);

    /* The LwIP netif name to pin the socket to with SO_BINDTODEVICE, so a
     * broadcast query arriving on the OTHER interface is not answered here.
     * Empty string when the stack has no such concept — on the TOE the chip IS
     * the interface. */
    void (*get_ifname)(char *ifname, size_t ifname_len);
} netbios_ops_t;

/* Ethernet (WIZnet chip) hooks — wizchip_* on TOE, esp_netif on ETH. */
extern const netbios_ops_t netbios_eth_ops;
/* Wi-Fi hooks — always esp_netif. */
extern const netbios_ops_t netbios_wifi_ops;

/*
 * Spawn a FreeRTOS task that waits until is_up() reports the interface ready,
 * then answers NetBIOS name queries for `nb_name` forever. Ethernet and Wi-Fi
 * are started with identical calls — same level, same shape; only the label,
 * vtable, name and readiness predicate differ.
 *   name     - short label; also the task name and log prefix (e.g. "eth")
 *   ops      - socket vtable + platform hooks for this interface
 *   nb_name  - NetBIOS name to answer to (case-insensitive, <= 15 characters)
 *   is_up    - predicate the task polls for link readiness
 */
void netbios_start(const char *name, const netbios_ops_t *ops,
                   const char *nb_name, bool (*is_up)(void));

#endif /* NETBIOS_H */
