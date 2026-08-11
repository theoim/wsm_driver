/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/*
 * UDP echo demo on the WIZnet TOE (W5500 / W6300).
 *
 * app_main only orchestrates: bring the interfaces up, start an echo task on
 * each, and return. The echo logic lives in the backend-neutral engine
 * udp_echo.c, which takes a socket vtable, so both interfaces run the very same
 * code at the same level:
 *   - Ethernet (WIZnet chip) on UDP_ECHO_PORT      (vtable: net_eth_ops)
 *   - Wi-Fi STA              on WIFI_UDP_ECHO_PORT (vtable: net_wifi_ops)
 * net_eth_ops is the plain lwIP BSD entry points, which the wsm_driver
 * component redirects to the WIZnet hardware sockets at link time via
 * -Wl,--wrap (see wiztoe_wrap.c); net_wifi_ops bypasses that wrap so Wi-Fi
 * traffic reaches the real software LwIP stack. Neither the engine nor this
 * file needs an #if — the component owns that distinction.
 *
 * Wi-Fi is optional: leave WIFI_SSID empty in net_config.h to run Ethernet-only.
 *
 * Config conventions follow wsm_driver:
 *   - SPI / pins  -> component Kconfig, applied by the TOE backend.
 *   - network id  -> the wiz_NetInfo below (byte arrays from net_config.h),
 *                    applied by wiznet_net_init() -> wizchip_setnetinfo().
 *   - UDP role    -> menuconfig: UDP Example Configuration -> UDP role.
 *
 * Works with W5500 or W6300 — select the chip in menuconfig:
 *   Component config -> WIZnet WSM Driver -> WIZnet chip
 */
#include "sdkconfig.h"
#include "wizchip_conf.h"       /* wiz_NetInfo, NETINFO_STATIC */

#include "net_backend.h"
#include "wifi_backend.h"
#include "net_sock_ops.h"
#include "net_config.h"
#include "udp_echo.h"

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
    .dhcp = NETINFO_STATIC,
};

/* Server binds the well-known echo port; client binds the ephemeral one, which
 * is the only difference between the two roles in the ioLibrary reference. */
#ifdef CONFIG_EXAMPLE_UDP_CLIENT
#define UDP_ECHO_BIND_PORT       UDP_ECHO_CLIENT_PORT
#define WIFI_UDP_ECHO_BIND_PORT  WIFI_UDP_ECHO_CLIENT_PORT
#else
#define UDP_ECHO_BIND_PORT       UDP_ECHO_PORT
#define WIFI_UDP_ECHO_BIND_PORT  WIFI_UDP_ECHO_PORT
#endif

/* An empty SSID means "no AP configured" — run Ethernet-only rather than
 * spinning on a connect that can never succeed. A plain runtime test rather than
 * an #if: the preprocessor cannot inspect a string literal, and the compiler
 * folds this away anyway. */
#define WIFI_CONFIGURED  (WIFI_SSID[0] != '\0')

void app_main(void)
{
    /* Ethernet (WIZnet chip) first: it initializes esp_netif + the default event
     * loop that Wi-Fi then reuses, and applies g_net_info to the chip. */
    wiznet_net_init(&g_net_info);
    if (WIFI_CONFIGURED) {
        wifi_net_init(WIFI_SSID, WIFI_PASS);
    }

    /* Start both echo servers as sibling tasks; each waits for its own link.
     * Same call shape — only the label, vtable, port and readiness predicate
     * differ. */
    udp_echo_start("eth", &net_eth_ops, UDP_ECHO_BIND_PORT, wiznet_net_is_up);
    if (WIFI_CONFIGURED) {
        udp_echo_start("wifi", &net_wifi_ops, WIFI_UDP_ECHO_BIND_PORT, wifi_net_is_up);
    }
}
