/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Ethernet (WIZnet chip) side of the NetBIOS example.
 *
 * The protocol itself is NOT here — netbios.c runs it over net_eth_ops, the
 * plain lwip_* entry points, which the linker aims at the chip's hardware
 * sockets (TOE backend, via --wrap, so every call lands in __wrap_lwip_*) or at
 * software LwIP (ETH backend, so every call lands in lwip_*). That is the whole
 * TOE/LwIP branch, and it needs no #if.
 *
 * What is left here is the pair of operations a BSD socket cannot perform —
 * reading the address to answer with, and naming the netif to pin the socket to
 * — and those really do differ between the backends:
 *
 *   - TOE backend (CONFIG_WSM_DRIVER_BACKEND_TOE, the default): the address
 *     lives in the chip's registers, so it is wizchip_getnetinfo(). The chip
 *     also has no LwIP netif to name, so the socket is not pinned to one — it
 *     IS the interface.
 *
 *   - ETH backend (CONFIG_WSM_DRIVER_BACKEND_ETH): the chip is an esp_eth
 *     MACRAW MAC and the ESP32-S3's software LwIP owns TCP/IP, so this is
 *     exactly the same esp_netif code Wi-Fi uses — only the netif key differs.
 */
#include "sdkconfig.h"

#if !defined(CONFIG_WSM_DRIVER_BACKEND_ETH)
/* ORDER MATTERS — ioLibrary BEFORE lwIP. wizchip_conf.h -> w5500.h defines
 * SOCK_STREAM/SOCK_DGRAM as the Sn_MR protocol values, unguarded, and netbios.h
 * pulls in lwip/sockets.h which defines the same names as the POSIX socket
 * types. net_backend.h drops the ioLibrary aliases (see its header comment) so
 * the lwIP definitions win; that #undef only works if it is reached before
 * w5500.h, hence this block sits above netbios.h. */
#include <string.h>

#include "net_backend.h"    /* wizchip_conf.h: wiz_NetInfo, wizchip_getnetinfo */
#endif

#include "netbios.h"

#if defined(CONFIG_WSM_DRIVER_BACKEND_ETH)

/* ======================= ETH backend: esp_netif + LwIP ======================= */

#include "netif_netbios.h"

static bool eth_get_ip(uint8_t ip[4])
{
    return netif_netbios_get_ip(NETIF_KEY_ETH, ip);
}

static void eth_get_ifname(char *ifname, size_t ifname_len)
{
    netif_netbios_get_ifname(NETIF_KEY_ETH, ifname, ifname_len);
}

#else /* CONFIG_WSM_DRIVER_BACKEND_TOE */

/* ================== TOE backend: the chip's own network identity ============= */

/* string.h / wizchip_conf.h are included at the top of the file — they have to
 * precede lwip/sockets.h. */

static bool eth_get_ip(uint8_t ip[4])
{
    /* Read per response rather than cached, so a DHCP renewal that rewrote the
     * chip's registers is picked up without restarting the responder. */
    wiz_NetInfo ni;
    wizchip_getnetinfo(&ni);

    if (ni.ip[0] == 0 && ni.ip[1] == 0 && ni.ip[2] == 0 && ni.ip[3] == 0) {
        return false;                   /* no address yet — stay quiet */
    }
    memcpy(ip, ni.ip, 4);
    return true;
}

static void eth_get_ifname(char *ifname, size_t ifname_len)
{
    /* No LwIP netif to pin the socket to — the chip is the interface. */
    if (ifname_len > 0) {
        ifname[0] = '\0';
    }
}

#endif /* backend */

const netbios_ops_t netbios_eth_ops = {
    /* Plain lwip_*: --wrap sends them to the TOE, or they are software LwIP. */
    .sock       = &net_eth_ops,
    .get_ip     = eth_get_ip,
    .get_ifname = eth_get_ifname,
};
