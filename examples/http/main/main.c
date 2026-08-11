/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/*
 * HTTP server on the WIZnet TOE (W5500 / W6300), ported from
 * WIZnet-PICO-C examples/http/server.
 *
 * app_main only orchestrates: bring the interfaces up, start an HTTP server on
 * each, and return. The server logic lives in the backend-neutral engine
 * http_server.c, which takes a socket vtable, so both interfaces run the very same
 * code at the same level:
 *   - Ethernet (WIZnet chip) on HTTP_PORT      (vtable: net_eth_ops)
 *   - Wi-Fi STA              on WIFI_HTTP_PORT (vtable: net_wifi_ops)
 *
 * Which stack actually carries the traffic is decided by the LINKER, from
 * `Component config -> WIZnet WSM Driver -> Network backend`:
 *   - TOE (hardware TCP/IP): net_eth_ops' plain lwip_* calls are redirected to
 *     the chip's hardware sockets by -Wl,--wrap, i.e. every call in http_server.c ends
 *     up in __wrap_lwip_* (see wiztoe_wrap.c);
 *   - esp_eth MACRAW + software LwIP: there is no wrap, so the same calls end up
 *     in lwip_* and run over the software stack.
 * http_server.c contains no #if for this — only the vtable it is handed differs, and
 * net_wifi_ops bypasses the wrap so Wi-Fi always reaches the real LwIP.
 *
 * The ioLibrary httpServer (Internet/httpServer) is NOT used: it drives the
 * chip's socket registers directly, so --wrap has nothing to intercept and one
 * source could not serve both backends.
 *
 * Wi-Fi is optional: leave WIFI_SSID empty in net_config.h to run Ethernet-only.
 *
 * Config conventions follow wsm_driver:
 *   - SPI / pins  -> component Kconfig, applied by the TOE backend.
 *   - network id  -> the wiz_NetInfo below (byte arrays from net_config.h),
 *                    applied by wiznet_net_init() -> wizchip_setnetinfo().
 *   - web page    -> inc/web_page.h (index_page).
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
#include "http_server.h"

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

    /* Start both HTTP servers as sibling tasks; each waits for its own link.
     * Same call shape — only the label, vtable, port and readiness predicate
     * differ. */
    http_server_start("eth", &net_eth_ops, HTTP_PORT, wiznet_net_is_up);
    if (WIFI_CONFIGURED) {
        http_server_start("wifi", &net_wifi_ops, WIFI_HTTP_PORT, wifi_net_is_up);
    }
}
