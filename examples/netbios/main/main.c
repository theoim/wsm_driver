/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/*
 * NetBIOS name responder on the WIZnet TOE (W5500 / W6300), ported from
 * WIZnet-PICO-C examples/netbios.
 *
 * app_main only orchestrates: bring the interfaces up, start a responder on
 * each, and return. The responder logic lives in the backend-neutral engine
 * netbios.c, which takes a socket vtable, so both interfaces run the very same
 * code at the same level:
 *   - Ethernet (WIZnet chip) answering to NETBIOS_NAME      (vtable: netbios_eth_ops)
 *   - Wi-Fi STA              answering to WIFI_NETBIOS_NAME (vtable: netbios_wifi_ops)
 *
 * Which stack actually carries the traffic is decided by the LINKER, from
 * `Component config -> WIZnet WSM Driver -> Network backend`:
 *   - TOE (hardware TCP/IP): net_eth_ops' plain lwip_* calls are redirected to
 *     the chip's hardware sockets by -Wl,--wrap, i.e. every call in netbios.c
 *     ends up in __wrap_lwip_* (see wiztoe_wrap.c);
 *   - esp_eth MACRAW + software LwIP: there is no wrap, so the same calls end up
 *     in lwip_* and run over the software stack.
 * netbios.c contains no #if for this — only the vtable it is handed differs, and
 * net_wifi_ops bypasses the wrap so Wi-Fi always reaches the real LwIP.
 *
 * The ioLibrary socket API (socket(sn, Sn_MR_UDP, ...) / recvfrom(sn, ...) and
 * the getSn_SR() state machine the original example used) is NOT used: it drives
 * the chip's socket registers directly, so --wrap has nothing to intercept and
 * one source could not serve both backends.
 *
 * Wi-Fi is optional: leave WIFI_SSID empty in net_config.h to run Ethernet-only.
 *
 * Config conventions follow wsm_driver:
 *   - SPI / pins  -> component Kconfig, applied by the TOE backend.
 *   - network id  -> the wiz_NetInfo below (byte arrays from net_config.h),
 *                    applied by wiznet_net_init() -> wizchip_setnetinfo().
 *   - NetBIOS names / port -> inc/net_config.h.
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
#include "netbios.h"

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

    /* Start both responders as sibling tasks; each waits for its own link. Same
     * call shape — only the label, vtable, NetBIOS name and readiness predicate
     * differ. The two names must differ: a host that can see both interfaces
     * would otherwise get two answers for one name. */
    netbios_start("eth", &netbios_eth_ops, NETBIOS_NAME, wiznet_net_is_up);
    if (WIFI_CONFIGURED) {
        netbios_start("wifi", &netbios_wifi_ops, WIFI_NETBIOS_NAME, wifi_net_is_up);
    }
}
