/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Wi-Fi side of the DHCP + DNS example: the socket vtable the engine runs on,
 * plus the esp_netif hooks it cannot express as socket calls. The same esp_netif
 * helpers serve Ethernet under the ETH backend (eth_dhcp_dns.c), so they take
 * the netif key as a parameter.
 *
 * This file is the ONE place aware of the WIZnet --wrap. With
 * WSM_DRIVER_SOCKET_WRAP=1 the plain lwip_* symbols are redirected to the
 * chip's hardware sockets, so Wi-Fi must bind to the linker's __real_lwip_*
 * (the un-wrapped originals) to reach the REAL software LwIP stack its netif is
 * attached to. With =0 there is no --wrap and Wi-Fi and Ethernet share one LwIP
 * stack, so the vtable is just the plain lwip_* — identical to dhcp_lwip_ops.
 */
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"

#include "netif_dhcp_dns.h"

static const char *TAG = "netif_dhcp_dns";

/* ---- Wi-Fi socket vtable ---- */

#if defined(WSM_DRIVER_SOCKET_WRAP) && (WSM_DRIVER_SOCKET_WRAP)
/* Un-wrapped LwIP entry points provided by the linker because these symbols are
 * listed in --wrap (see the wsm_driver component CMakeLists, which also defines
 * WSM_DRIVER_SOCKET_WRAP PUBLIC). Calling __real_* bypasses the chip. */
extern int     __real_lwip_socket(int domain, int type, int protocol);
extern int     __real_lwip_bind(int s, const struct sockaddr *name, socklen_t namelen);
extern ssize_t __real_lwip_sendto(int s, const void *data, size_t size, int flags,
                                  const struct sockaddr *to, socklen_t tolen);
extern ssize_t __real_lwip_recvfrom(int s, void *mem, size_t len, int flags,
                                    struct sockaddr *from, socklen_t *fromlen);
extern int     __real_lwip_setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen);
extern int     __real_lwip_close(int s);

const dhcp_sock_ops_t dhcp_wifi_sock_ops = {
    .socket = __real_lwip_socket,   .bind = __real_lwip_bind,
    .sendto = __real_lwip_sendto,   .recvfrom = __real_lwip_recvfrom,
    .setsockopt = __real_lwip_setsockopt,
    .close = __real_lwip_close,
};
#else /* SOCKET_WRAP=0: no --wrap; plain lwIP (shared stack with Ethernet). */
const dhcp_sock_ops_t dhcp_wifi_sock_ops = {
    .socket = lwip_socket,   .bind = lwip_bind,
    .sendto = lwip_sendto,   .recvfrom = lwip_recvfrom,
    .setsockopt = lwip_setsockopt,
    .close = lwip_close,
};
#endif

/* ---- esp_netif hooks, shared with the ETH backend ---- */

void netif_dhcp_prepare(const char *ifkey, uint8_t mac[6], char *ifname, size_t ifname_len)
{
    memset(mac, 0, 6);
    if (ifname_len > 0) {
        ifname[0] = '\0';
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(ifkey);
    if (netif == NULL) {
        ESP_LOGE(TAG, "netif \"%s\" not found", ifkey);
        return;
    }

    /* The stack ships its own DHCP client; ours replaces it. Stopping it frees
     * UDP port 68 and keeps two clients from fighting over the same lease.
     * (Under the ETH backend the component already stopped it and set a static
     * identity — this call is then a harmless no-op.) */
    esp_err_t err = esp_netif_dhcpc_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "dhcpc_stop(\"%s\"): %s", ifkey, esp_err_to_name(err));
    }

    if (esp_netif_get_mac(netif, mac) != ESP_OK) {
        ESP_LOGE(TAG, "cannot read the MAC of \"%s\"", ifkey);
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

void netif_apply_lease(const char *ifkey, const dhcp_dns_netinfo_t *info)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(ifkey);
    if (netif == NULL) {
        return;
    }

    esp_netif_ip_info_t ip = {0};
    ip.ip.addr      = ESP_IP4TOADDR(info->ip[0], info->ip[1], info->ip[2], info->ip[3]);
    ip.netmask.addr = ESP_IP4TOADDR(info->sn[0], info->sn[1], info->sn[2], info->sn[3]);
    ip.gw.addr      = ESP_IP4TOADDR(info->gw[0], info->gw[1], info->gw[2], info->gw[3]);
    if (esp_netif_set_ip_info(netif, &ip) != ESP_OK) {
        ESP_LOGE(TAG, "cannot install the lease on \"%s\"", ifkey);
        return;
    }

    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr =
        ESP_IP4TOADDR(info->dns[0], info->dns[1], info->dns[2], info->dns[3]);
    if (dns.ip.u_addr.ip4.addr != 0) {
        esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    }
}

/* ---- Wi-Fi vtable: the helpers above bound to the STA netif ---- */

static void wifi_prepare(uint8_t mac[6], char *ifname, size_t ifname_len)
{
    netif_dhcp_prepare(NETIF_KEY_WIFI, mac, ifname, ifname_len);
}

static void wifi_apply_lease(const dhcp_dns_netinfo_t *info)
{
    netif_apply_lease(NETIF_KEY_WIFI, info);
}

const dhcp_dns_ops_t dhcp_dns_wifi_ops = {
    .sock        = &dhcp_wifi_sock_ops,
    .prepare     = wifi_prepare,
    .apply_lease = wifi_apply_lease,
};
