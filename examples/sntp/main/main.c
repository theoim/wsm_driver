/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * WIZnet TOE (W5500 / W6300) + Wi-Fi SNTP demo.
 *
 * app_main brings up BOTH interfaces and starts an SNTP query on each as its
 * OWN task, at the same level (see sntp_client_start):
 *   - Ethernet (WIZnet chip) from local UDP port SNTP_LOCAL_PORT
 *   - Wi-Fi STA              from local UDP port WIFI_SNTP_LOCAL_PORT
 * Both ask the same server, so the two printed times should agree.
 *
 * The query logic lives in the backend-neutral client sntp_client.c; each
 * interface supplies a socket vtable. Ethernet uses the plain lwIP BSD entry
 * points, which the wsm_driver component redirects to the WIZnet hardware
 * sockets at link time via -Wl,--wrap (see wiztoe_wrap.c). The Wi-Fi path's
 * --wrap awareness is isolated to wifi_sntp.c.
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
#include "sntp_client.h"

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

    /* Start both queries as sibling tasks; each waits for its own link. Same
     * call shape - only the label, vtable, local port and readiness predicate
     * differ. Ethernet uses the standard lwIP vtable (--wrap-redirected to the
     * WIZnet chip); Wi-Fi uses its own (__real_lwip_* to bypass the wrap). */
    sntp_client_start("eth",  &sntp_lwip_ops, SNTP_SERVER_IP,
                      SNTP_LOCAL_PORT, wiznet_net_is_up);
    sntp_client_start("wifi", &wifi_sntp_ops, SNTP_SERVER_IP,
                      WIFI_SNTP_LOCAL_PORT, wifi_net_is_up);
}
