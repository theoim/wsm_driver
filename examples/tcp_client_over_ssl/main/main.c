/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * WIZnet TOE (W5500 / W6300) + Wi-Fi TCP-over-SSL client demo.
 *
 * app_main brings up BOTH interfaces and starts a TLS client on each as its OWN
 * task, at the same level (see ssl_client_start):
 *   - Ethernet (WIZnet chip) -> SSL_TARGET_IP:SSL_TARGET_PORT
 *   - Wi-Fi STA              -> the same server
 * Both use ephemeral local ports, so the two sessions never collide.
 *
 * The TLS logic lives in the backend-neutral client ssl_client.c; each interface
 * supplies a socket vtable that mbedTLS's BIO is wired to. Ethernet uses the
 * plain lwIP BSD entry points, which the wsm_driver component redirects to the
 * WIZnet hardware sockets at link time via -Wl,--wrap (see wiztoe_wrap.c). The
 * Wi-Fi path's --wrap awareness is isolated to wifi_ssl_client.c.
 *
 * Certificate verification is disabled (VERIFY_NONE) to keep the demo
 * dependency-free, same as the original. Point SSL_TARGET_IP at your TLS server.
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
#include "ssl_client.h"

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

    /* Start both clients as sibling tasks; each waits for its own link. Same
     * call shape - only the label, vtable and readiness predicate differ.
     * Ethernet uses the standard lwIP vtable (--wrap-redirected to the WIZnet
     * chip); Wi-Fi uses its own (__real_lwip_* to bypass the wrap). */
    ssl_client_start("eth",  &ssl_lwip_ops, SSL_TARGET_IP, SSL_TARGET_PORT,
                     wiznet_net_is_up);
    ssl_client_start("wifi", &wifi_ssl_ops, SSL_TARGET_IP, SSL_TARGET_PORT,
                     wifi_net_is_up);
}
