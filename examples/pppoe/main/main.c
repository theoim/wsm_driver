/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * WIZnet TOE (W5500) PPPoE example.
 *
 * Establishes a PPPoE session (e.g. against an ISP modem or a test PPPoE
 * server) and prints the assigned IP. PPPoE.c / md5.c are carried with the
 * example, same as in the original; set the credentials in net_config.h.
 *
 * Ethernet only, and W5500 only, deliberately: the vendored PPPoE.c drives the
 * chip's PPPoE registers directly. That is below the BSD socket layer the other
 * examples switch between Ethernet and Wi-Fi, and the W6300 exposes PPPoE
 * through a different register map. Only the file layout and the config split
 * follow the loopback-style examples.
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
#include "net_config.h"
#include "pppoe_session.h"

/* Network identity - wsm_driver style (wiz_NetInfo). Applied to the WIZnet
 * chip's hardware TCP/IP stack by wiznet_net_init() -> wizchip_setnetinfo().
 * PPPoE replaces the address once the session comes up. */
static const wiz_NetInfo g_net_info = {
    .mac = NET_MAC_ADDR,
    .ip  = NET_IP_ADDR,
    .sn  = NET_SUBNET_MASK,
    .gw  = NET_GATEWAY,
    .dns = NET_DNS_ADDR,
    .dhcp = NETINFO_STATIC,
};

void app_main(void)
{
    wiznet_net_init(&g_net_info);
    pppoe_session_start("eth", wiznet_net_is_up);
}
