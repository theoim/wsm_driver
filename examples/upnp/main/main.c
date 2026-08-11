/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/*
 * UPnP IGD client on the WIZnet TOE (W5500 / W6300), ported from
 * WIZnet-PICO-C examples/upnp.
 *
 * app_main only orchestrates: bring the interface up, start the session, and
 * return. The session lives in the backend-neutral upnp_client.c, which takes a
 * socket vtable -- here net_eth_ops, the plain lwIP BSD entry points that the
 * wsm_driver component redirects to the WIZnet hardware sockets at link time
 * via -Wl,--wrap (see wiztoe_wrap.c).
 *
 * The protocol implementation is ioLibrary's, carried in the example rather
 * than used from third_party, so its network calls could be swapped for BSD
 * ones without forking the submodule. It reaches the network only through
 * upnp_transport.h.
 *
 * Unlike the multicast example this one does not start a second session on
 * Wi-Fi at the same time: upnp_core.c keeps the discovered IGD in globals, so
 * two concurrent sessions would share it. Set WIFI_SSID and UPNP_OVER_WIFI to
 * run over Wi-Fi instead of Ethernet.
 *
 * Config conventions follow wsm_driver:
 *   - SPI / pins  -> component Kconfig, applied by the TOE backend.
 *   - network id  -> the wiz_NetInfo below (byte arrays from net_config.h),
 *                    applied by wiznet_net_init() -> wizchip_setnetinfo().
 *   - mapping     -> net_config.h.
 *
 * Works with W5500 or W6300 — select the chip in menuconfig:
 *   Component config -> WIZnet WSM Driver -> WIZnet chip
 */
#include "sdkconfig.h"
#include "esp_netif.h"
#include "wizchip_conf.h"       /* wiz_NetInfo, NETINFO_STATIC */

#include "net_backend.h"
#include "wifi_backend.h"
#include "net_sock_ops.h"
#include "net_config.h"
#include "upnp_client.h"

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

/* The IGD is told where to send eventing notifications, so the session needs
 * this interface's address as text. On Ethernet it is the static identity
 * above; on Wi-Fi it is a DHCP lease, so it is read off the netif once the link
 * is up. Resolving it here rather than in upnp_client.c keeps that file free of
 * esp_netif and thus reusable on either backend. */
static const char *eth_local_ip(void)
{
    return NET_IP_ADDR_STR;
}

#if UPNP_OVER_WIFI
static const char *wifi_local_ip(void)
{
    static char ip_str[16] = "0.0.0.0";

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (netif != NULL && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
        esp_ip4addr_ntoa(&info.ip, ip_str, sizeof(ip_str));
    }
    return ip_str;
}
#endif

void app_main(void)
{
    /* Ethernet (WIZnet chip) first: it initializes esp_netif + the default event
     * loop that Wi-Fi then reuses, and applies g_net_info to the chip. */
    wiznet_net_init(&g_net_info);
    if (WIFI_CONFIGURED) {
        wifi_net_init(WIFI_SSID, WIFI_PASS);
    }

#if UPNP_OVER_WIFI
    upnp_client_start("wifi", &net_wifi_ops, wifi_net_is_up, wifi_local_ip);
#else
    upnp_client_start("eth", &net_eth_ops, wiznet_net_is_up, eth_local_ip);
#endif
}
