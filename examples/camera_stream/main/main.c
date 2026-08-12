/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/*
 * MJPEG camera streaming on the WIZnet WSM driver (W5500 / W6300).
 *
 * app_main only orchestrates: bring the camera up, bring the interfaces up,
 * start a server on each, and return. The sensor lives in cam_source.c and the
 * server in cam_server.c, both of which take a socket vtable -- net_eth_ops for
 * the plain lwIP BSD entry points that the wsm_driver component redirects to
 * the WIZnet hardware sockets at link time via -Wl,--wrap (see wiztoe_wrap.c),
 * and net_wifi_ops for the software LwIP behind the Wi-Fi netif.
 *
 * A browser is the whole test rig: open http://192.168.11.2 and the device
 * serves the page that streams from it. Fill in WIFI_SSID as well and the same
 * page appears at http://<sta-ip>:81 with an lwIP badge instead of a TOE one --
 * one camera, two stacks, frame rate and link bandwidth plotted side by side.
 *
 * Board: Seeed XIAO ESP32-S3 Sense (OV3660) with a WIZnet module on SPI. The
 * camera pins are fixed by the Sense board and do not overlap the SPI wiring;
 * cam_pins.h has both maps side by side.
 *
 * Config conventions follow wsm_driver:
 *   - SPI / pins  -> component Kconfig (Board), applied by the TOE backend.
 *   - network id  -> the wiz_NetInfo below (byte arrays from net_config.h),
 *                    applied by wiznet_net_init() -> wizchip_setnetinfo().
 *   - ports       -> net_config.h.
 */
#include "esp_log.h"
#include "sdkconfig.h"
#include "wizchip_conf.h"       /* wiz_NetInfo, NETINFO_STATIC */

#include "net_backend.h"
#include "wifi_backend.h"
#include "net_sock_ops.h"

#include "cam_server.h"
#include "cam_source.h"
#include "net_config.h"

static const char *TAG = "main";

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
 * spinning on a connect that can never succeed. */
#define WIFI_CONFIGURED  (WIFI_SSID[0] != '\0')

/*
 * How many listening sockets each interface opens. This is a property of the
 * backend rather than a preference -- see http_transport.h. The Ethernet side
 * needs several only when it is the TOE, where a listening hardware socket
 * becomes the connection and nothing is left listening while a stream is up.
 */
#if defined(WSM_DRIVER_SOCKET_WRAP) && WSM_DRIVER_SOCKET_WRAP
#define ETH_LISTENERS   3
#define ETH_STACK_NAME  "TOE"
#else
#define ETH_LISTENERS   1
#define ETH_STACK_NAME  "lwIP"
#endif

void app_main(void)
{
    /* The camera first: if the sensor is missing there is nothing to serve, and
     * failing here with one clear line beats a page that loads and stays black. */
    if (cam_source_init() != 0) {
        ESP_LOGE(TAG, "camera init failed -- check the Sense board is seated");
        return;
    }

    /* Ethernet (WIZnet chip) next: it initializes esp_netif + the default event
     * loop that Wi-Fi then reuses, and applies g_net_info to the chip. */
    wiznet_net_init(&g_net_info);
    if (WIFI_CONFIGURED) {
        wifi_net_init(WIFI_SSID, WIFI_PASS);
    }

    /* Start both servers as sibling tasks; each waits for its own link. Same
     * call shape — only the label, stack name, vtable, port, listener count and
     * readiness predicate differ. */
    cam_server_start("eth", ETH_STACK_NAME, &net_eth_ops, CAM_PORT,
                     ETH_LISTENERS, wiznet_net_is_up);
    if (WIFI_CONFIGURED) {
        cam_server_start("wifi", "lwIP", &net_wifi_ops, WIFI_CAM_PORT,
                         1, wifi_net_is_up);
    }
}
