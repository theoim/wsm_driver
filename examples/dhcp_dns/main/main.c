/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/*
 * WIZnet TOE (W5500 / W6300) + Wi-Fi DHCP & DNS demo.
 *
 * app_main brings up BOTH interfaces and runs the same DHCP-then-DNS sequence
 * on each as its OWN task, at the same level (see dhcp_dns_start):
 *   - Ethernet (WIZnet chip)  (vtable: dhcp_dns_eth_ops)
 *   - Wi-Fi STA               (vtable: dhcp_dns_wifi_ops)
 * app_main itself just orchestrates: init both stacks, start both tasks, return.
 *
 * The whole DHCP/DNS protocol lives in the backend-neutral engine dhcp_dns.c,
 * written against a BSD socket vtable exactly like examples/loopback. The
 * TOE-vs-LwIP choice is made by the LINKER: with the TOE backend the engine's
 * lwip_* calls are --wrap-redirected to the chip's hardware sockets, with the
 * ETH backend they are software LwIP. The ioLibrary DHCP/DNS clients are not
 * used. Each interface supplies only what a socket cannot do — its MAC/netif
 * name and how to install the lease (eth_dhcp_dns.c / netif_dhcp_dns.c).
 *
 * Config conventions follow wsm_driver:
 *   - SPI / pins  -> component Kconfig, applied by net_backend_toe.c.
 *   - network id  -> the wiz_NetInfo below (byte arrays from net_config.h),
 *                    applied by wiznet_net_init() -> wizchip_setnetinfo().
 *
 * Because .dhcp is NETINFO_DHCP, the addresses below are only what the chip
 * carries until the first lease arrives — the DHCP server supplies the real
 * ip/sn/gw/dns. The MAC is never leased and always comes from net_config.h.
 */

#include <stdio.h>

#include "sdkconfig.h"
#include "wizchip_conf.h"       /* wiz_NetInfo, NETINFO_DHCP */

#include "net_backend.h"
#include "wifi_backend.h"
#include "net_config.h"
#include "dhcp_dns.h"

/* Network identity — wsm_driver style (wiz_NetInfo). Applied to the WIZnet
 * chip's hardware TCP/IP stack by wiznet_net_init() -> wizchip_setnetinfo(). */
static const wiz_NetInfo g_net_info = {
    .mac = NET_MAC_ADDR,
    .ip  = NET_IP_ADDR,
    .sn  = NET_SUBNET_MASK,
    .gw  = NET_GATEWAY,
    .dns = NET_DNS_ADDR,
#if _WIZCHIP_ > W5500
    .ipmode = NETINFO_STATIC_ALL,
#endif
    .dhcp = NETINFO_DHCP,
};

void app_main(void)
{
    /* Ethernet (WIZnet chip) first: it initializes esp_netif + the default event
     * loop that Wi-Fi then reuses, and applies g_net_info to the chip. */
    wiznet_net_init(&g_net_info);
    wifi_net_init(WIFI_SSID, WIFI_PASS);

    /* Lease an address and resolve the domain on both interfaces as sibling
     * tasks; each waits for its own link. Same call shape — only the label,
     * vtable and readiness predicate differ. */
    dhcp_dns_start("eth",  &dhcp_dns_eth_ops,  DHCP_DNS_DOMAIN, wiznet_net_is_up);
    dhcp_dns_start("wifi", &dhcp_dns_wifi_ops, DHCP_DNS_DOMAIN, wifi_net_is_up);
}
