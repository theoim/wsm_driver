/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/*
 * TFTP read client on the WIZnet TOE (W5500 / W6300), ported from
 * WIZnet-PICO-C examples/tftp.
 *
 * app_main only orchestrates: bring the interface up, start the client, and
 * return. The transfer lives in the backend-neutral client tftp_client.c, which
 * takes a socket vtable -- here net_eth_ops, the plain lwIP BSD entry points
 * that the wsm_driver component redirects to the WIZnet hardware sockets at
 * link time via -Wl,--wrap (see wiztoe_wrap.c).
 *
 * The protocol implementation (tftp_core.c) is ioLibrary's, carried in the
 * example rather than used from third_party, so its four network wrappers could
 * be swapped for BSD ones without forking the submodule. It reaches the network
 * only through tftp_transport.h.
 *
 * Unlike the other converted examples this one does not start a second client
 * on Wi-Fi at the same time: tftp_core.c keeps its state in globals, so two
 * concurrent transfers would share it. Set WIFI_SSID and TFTP_OVER_WIFI to run
 * the transfer over Wi-Fi instead of Ethernet.
 *
 * Config conventions follow wsm_driver:
 *   - SPI / pins  -> component Kconfig, applied by the TOE backend.
 *   - network id  -> the wiz_NetInfo below (byte arrays from net_config.h),
 *                    applied by wiznet_net_init() -> wizchip_setnetinfo().
 *   - server/file -> net_config.h.
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
#include "tftp_client.h"

/* Set to 1 to run the transfer over Wi-Fi instead of Ethernet. Requires
 * WIFI_SSID to be filled in. */
#define TFTP_OVER_WIFI  0

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

#if TFTP_OVER_WIFI
    tftp_client_start("wifi", &net_wifi_ops, TFTP_SERVER_IP, TFTP_FILE_NAME,
                      wifi_net_is_up);
#else
    tftp_client_start("eth", &net_eth_ops, TFTP_SERVER_IP, TFTP_FILE_NAME,
                      wiznet_net_is_up);
#endif
}
