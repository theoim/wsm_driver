/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Wi-Fi side of the NetBIOS example: the esp_netif hooks the engine cannot
 * express as socket calls, bound to the STA netif. The same helpers serve
 * Ethernet under the ETH backend (eth_netbios.c), so they take the netif key as
 * a parameter.
 *
 * The socket vtable itself is NOT here — it is the component's net_wifi_ops,
 * which is the one place aware of the WIZnet --wrap: with
 * WSM_DRIVER_SOCKET_WRAP=1 it binds __real_lwip_* so Wi-Fi reaches the real
 * software LwIP its netif sits on, and with =0 it is the plain lwip_* both
 * interfaces share. Nothing in this example needs an #if for that.
 */
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"

#include "netbios.h"
#include "netif_netbios.h"

static const char *TAG = "netif_netbios";

/* ---- esp_netif hooks, shared with the ETH backend ---- */

bool netif_netbios_get_ip(const char *ifkey, uint8_t ip[4])
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(ifkey);
    if (netif == NULL) {
        return false;
    }

    esp_netif_ip_info_t info = {0};
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) {
        return false;
    }

    /* esp_ip4_addr_t holds the address in network order, which is the order the
     * NetBIOS answer carries it in. */
    memcpy(ip, &info.ip.addr, 4);
    return true;
}

void netif_netbios_get_ifname(const char *ifkey, char *ifname, size_t ifname_len)
{
    if (ifname_len > 0) {
        ifname[0] = '\0';
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(ifkey);
    if (netif == NULL) {
        ESP_LOGE(TAG, "netif \"%s\" not found", ifkey);
        return;
    }

    char impl[8] = {0};
    if (esp_netif_get_netif_impl_name(netif, impl) == ESP_OK && ifname_len > 0) {
        size_t n = strlen(impl);
        if (n > ifname_len - 1) {
            n = ifname_len - 1;
        }
        memcpy(ifname, impl, n);
        ifname[n] = '\0';
    }
}

/* ---- Wi-Fi vtable: the helpers above bound to the STA netif ---- */

static bool wifi_get_ip(uint8_t ip[4])
{
    return netif_netbios_get_ip(NETIF_KEY_WIFI, ip);
}

static void wifi_get_ifname(char *ifname, size_t ifname_len)
{
    netif_netbios_get_ifname(NETIF_KEY_WIFI, ifname, ifname_len);
}

const netbios_ops_t netbios_wifi_ops = {
    /* Bypasses the WIZnet --wrap when it is active, so Wi-Fi always reaches the
     * software LwIP stack. Provided by the component (net_sock_ops.h). */
    .sock        = &net_wifi_ops,
    .get_ip      = wifi_get_ip,
    .get_ifname  = wifi_get_ifname,
};
