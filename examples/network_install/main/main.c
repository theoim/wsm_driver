/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * WIZnet TOE (W5500 / W6300) + Wi-Fi network install check.
 *
 * Checks both links and prints what a first bring-up needs to know:
 *   - Ethernet: PHY link, negotiated speed/duplex, and the IP to ping
 *   - Wi-Fi STA: association, SSID/channel/RSSI, and the IP to ping
 * Then it idles. No sockets are opened on either interface.
 *
 * This is where it differs from the socket-based examples: those run ONE engine
 * over a swappable socket vtable. Here there is nothing to swap -- the Ethernet
 * check reads the WIZnet chip's PHY registers (below the socket layer) and Wi-Fi
 * has no such registers, so each side has its own small check. Same question
 * asked twice: "is this link usable?"
 *
 * Config conventions follow wsm_driver:
 *   - SPI / pins  -> component Kconfig, applied by net_backend_toe.c.
 *   - network id  -> the wiz_NetInfo below (byte arrays from net_config.h),
 *                    applied by wiznet_net_init() -> wizchip_setnetinfo().
 */

#include <stdio.h>

#include "sdkconfig.h"
#include "wizchip_conf.h"       /* wiz_NetInfo, NETINFO_STATIC */

#include "net_backend.h"
#include "wifi_backend.h"
#include "net_config.h"
#include "link_check.h"

/* Network identity - wsm_driver style (wiz_NetInfo). Applied to the WIZnet
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
    .dhcp = NETINFO_STATIC,
};

void app_main(void)
{
    /* Ethernet (WIZnet chip) first: it initializes esp_netif + the default event
     * loop that Wi-Fi then reuses, and applies g_net_info to the chip. */
    wiznet_net_init(&g_net_info);
    wifi_net_init(WIFI_SSID, WIFI_PASS);

    /* Start both checks as sibling tasks; each waits for its own interface. */
    link_check_start("eth", &g_net_info, wiznet_net_is_up);
    wifi_link_check_start("wifi", wifi_net_is_up);
}
