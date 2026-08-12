/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/*
 * WebSocket echo server on the WIZnet WSM driver (W5500 / W6300).
 *
 * app_main only orchestrates: bring the interfaces up, start a server on each,
 * and return. The protocol lives in the backend-neutral ws_core.c and the
 * session in ws_server.c, both of which take a socket vtable -- net_eth_ops for
 * the plain lwIP BSD entry points that the wsm_driver component redirects to
 * the WIZnet hardware sockets at link time via -Wl,--wrap (see wiztoe_wrap.c),
 * and net_wifi_ops for the software LwIP behind the Wi-Fi netif.
 *
 * The RFC 6455 implementation is ported from mWebSockets (MIT, Dawid Kurek),
 * carried in the example rather than pulled in as a dependency so its network
 * calls could be swapped for BSD ones. It reaches the network only through
 * ws_transport.h.
 *
 * A browser is the whole test rig:
 *
 *     const ws = new WebSocket('ws://192.168.11.2/');
 *     ws.onmessage = (e) => console.log(e.data);
 *     ws.onopen = () => ws.send('hello');
 *
 * Wi-Fi is optional: leave WIFI_SSID empty in net_config.h to run Ethernet-only.
 *
 * Config conventions follow wsm_driver:
 *   - SPI / pins  -> component Kconfig (Board), applied by the TOE backend.
 *   - network id  -> the wiz_NetInfo below (byte arrays from net_config.h),
 *                    applied by wiznet_net_init() -> wizchip_setnetinfo().
 *   - ports       -> net_config.h.
 *
 * Works with W5500 or W6300 — select the board in menuconfig:
 *   Component config -> WIZnet WSM Driver -> Board
 */
#include "sdkconfig.h"
#include "wizchip_conf.h"       /* wiz_NetInfo, NETINFO_STATIC */

#include "net_backend.h"
#include "wifi_backend.h"
#include "net_sock_ops.h"
#include "net_config.h"
#include "ws_server.h"

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

    /* Start both servers as sibling tasks; each waits for its own link. Same
     * call shape — only the label, vtable, port and readiness predicate differ. */
    ws_server_start("eth", &net_eth_ops, WS_PORT, wiznet_net_is_up);
    if (WIFI_CONFIGURED) {
        ws_server_start("wifi", &net_wifi_ops, WIFI_WS_PORT, wifi_net_is_up);
    }
}
