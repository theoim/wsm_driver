/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Ethernet (WIZnet chip) side of the DHCP + DNS example.
 *
 * The protocol itself is NOT here — dhcp_dns.c runs it over dhcp_lwip_ops, the
 * plain lwip_* entry points, which the linker aims at the chip's hardware
 * sockets (TOE backend, via --wrap) or at software LwIP (ETH backend). That is
 * the whole TOE/LwIP branch, and it needs no #if.
 *
 * What is left here is the pair of operations a BSD socket cannot perform —
 * reading the interface identity and installing a lease into the stack — and
 * those really do differ between the backends:
 *
 *   - TOE backend (CONFIG_WSM_DRIVER_BACKEND_TOE, the default): the identity
 *     lives in the chip's registers, so it is wizchip_get/setnetinfo(). The chip
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
 * SOCK_STREAM/SOCK_DGRAM as the Sn_MR protocol values, unguarded, and
 * dhcp_dns.h pulls in lwip/sockets.h which defines the same names as the POSIX
 * socket types. net_backend.h drops the ioLibrary aliases (see its header
 * comment) so the lwIP definitions win; that #undef only works if it is reached
 * before w5500.h, hence this block sits above dhcp_dns.h. */
#include <string.h>

#include "esp_log.h"
#include "net_backend.h"    /* wizchip_conf.h: wiz_NetInfo, wizchip_get/setnetinfo */
#endif

#include "dhcp_dns.h"

#if defined(CONFIG_WSM_DRIVER_BACKEND_ETH)

/* ======================= ETH backend: esp_netif + LwIP ======================= */

#include "netif_dhcp_dns.h"

static void eth_prepare(uint8_t mac[6], char *ifname, size_t ifname_len)
{
    netif_dhcp_prepare(NETIF_KEY_ETH, mac, ifname, ifname_len);
}

static void eth_apply_lease(const dhcp_dns_netinfo_t *info)
{
    netif_apply_lease(NETIF_KEY_ETH, info);
}

#else /* CONFIG_WSM_DRIVER_BACKEND_TOE */

/* ================== TOE backend: the chip's own network identity ============= */

/* string.h / esp_log.h / wizchip_conf.h are included at the top of the file —
 * they have to precede lwip/sockets.h. */

static const char *TAG = "eth_dhcp_dns";

static void eth_prepare(uint8_t mac[6], char *ifname, size_t ifname_len)
{
    /* The MAC is never leased; it is whatever wiznet_net_init() wrote. Reading
     * the whole struct back also preserves the W6300 ipmode field. */
    wiz_NetInfo ni;
    wizchip_getnetinfo(&ni);
    memcpy(mac, ni.mac, 6);

    /* Drop the placeholder address main.c seeded the chip with, so DISCOVER
     * goes out from 0.0.0.0 as RFC 2131 wants. This is what the ioLibrary DHCP
     * client did in DHCP_init(); with a socket there is no other way to say it. */
    memset(ni.ip, 0, sizeof(ni.ip));
    memset(ni.sn, 0, sizeof(ni.sn));
    memset(ni.gw, 0, sizeof(ni.gw));
    ni.dhcp = NETINFO_DHCP;
    wizchip_setnetinfo(&ni);

    /* No LwIP netif to pin the socket to — the chip is the interface. */
    if (ifname_len > 0) {
        ifname[0] = '\0';
    }
}

static void eth_apply_lease(const dhcp_dns_netinfo_t *info)
{
    wiz_NetInfo ni;
    wizchip_getnetinfo(&ni);            /* keeps the MAC (+ the W6300 ipmode) */

    memcpy(ni.ip,  info->ip,  sizeof(ni.ip));
    memcpy(ni.sn,  info->sn,  sizeof(ni.sn));
    memcpy(ni.gw,  info->gw,  sizeof(ni.gw));
    memcpy(ni.dns, info->dns, sizeof(ni.dns));
    ni.dhcp = NETINFO_DHCP;

    wizchip_setnetinfo(&ni);            /* the chip's TCP/IP now owns the lease */

    ESP_LOGI(TAG, "lease applied to the chip: %u.%u.%u.%u",
             info->ip[0], info->ip[1], info->ip[2], info->ip[3]);
}

#endif /* backend */

const dhcp_dns_ops_t dhcp_dns_eth_ops = {
    /* Plain lwip_*: --wrap sends them to the TOE, or they are software LwIP. */
    .sock        = &dhcp_lwip_ops,
    .prepare     = eth_prepare,
    .apply_lease = eth_apply_lease,
};
