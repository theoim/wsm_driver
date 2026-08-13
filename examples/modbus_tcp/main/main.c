/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/*
 * Modbus TCP server (slave) on the WIZnet WSM driver (W5500 / W6300).
 *
 * app_main only orchestrates: bring the interfaces up, start a server on each,
 * and return. The protocol lives in the backend-neutral mb_core.c and the
 * session in mb_server.c, both of which take a socket vtable -- net_eth_ops for
 * the plain lwIP BSD entry points that the wsm_driver component redirects to
 * the WIZnet hardware sockets at link time via -Wl,--wrap (see wiztoe_wrap.c),
 * and net_wifi_ops for the software LwIP behind the Wi-Fi netif.
 *
 * The Modbus implementation is written for this example rather than ported.
 * Modbus TCP is small enough to be worth reading in full: the serial line's
 * CRC-16, inter-frame timer and ASCII transcoding -- most of the bulk of a
 * general Modbus library -- do not exist over TCP, where framing is the 7-byte
 * MBAP header and the transport already guarantees ordering and integrity.
 *
 * Any standard master is the test rig:
 *
 *     mbpoll -m tcp -a 1 -r 1 -c 10 192.168.11.2
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
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "wizchip_conf.h"       /* wiz_NetInfo, NETINFO_STATIC */

#include "net_backend.h"
#include "wifi_backend.h"
#include "net_sock_ops.h"

#include "app_config.h"
#include "app_control.h"
#include "mb_server.h"
#include "mb_store.h"
#include "net_config.h"
#include "web_server.h"

static const char *TAG = "main";

/* Network identity — wsm_driver style (wiz_NetInfo). The MAC and DNS come from
 * net_config.h and are not settable at runtime; the address, mask, gateway and
 * Modbus port come from NVS when one has been saved, and from net_config.h
 * otherwise. See app_config.h for why the split falls there. */
static wiz_NetInfo g_net_info = {
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
    /* NVS first: the stored address decides what the chip is brought up on, so
     * it has to be read before wiznet_net_init() rather than applied after. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    app_config_t cfg;
    bool stored = app_config_load(&cfg);
    ESP_LOGI(TAG, "%s configuration: %u.%u.%u.%u, Modbus port %u, web port %u",
             stored ? "stored" : "factory",
             cfg.ip[0], cfg.ip[1], cfg.ip[2], cfg.ip[3],
             cfg.modbus_port, WEB_PORT);

    memcpy(g_net_info.ip, cfg.ip, 4);
    memcpy(g_net_info.sn, cfg.mask, 4);
    memcpy(g_net_info.gw, cfg.gateway, 4);

    /* One data model behind both the Modbus server and the web UI, so a
     * register a master writes is the register the browser shows. */
    mb_store_t *store = mb_store_create();
    if (store == NULL) {
        ESP_LOGE(TAG, "out of memory for the data model");
        return;
    }

    /* Ethernet (WIZnet chip) first: it initializes esp_netif + the default event
     * loop that Wi-Fi then reuses, and applies g_net_info to the chip. */
    wiznet_net_init(&g_net_info);
    if (WIFI_CONFIGURED) {
        wifi_net_init(WIFI_SSID, WIFI_PASS);
    }

    mb_server_t *eth = mb_server_start("eth", &net_eth_ops, cfg.modbus_port,
                                       store, wiznet_net_is_up);
    web_server_t *web = web_server_start(&net_eth_ops, WEB_PORT, store,
                                         wiznet_net_is_up);

    /* The control task owns both servers from here: an address change stops and
     * restarts them, and two owners of one handle is how a task ends up writing
     * to a socket someone else already closed. */
    app_control_start(&net_eth_ops, store, eth, web, &cfg);

    /*
     * Wi-Fi keeps its own data model and is not reconfigurable from the page.
     *
     * It is here to show the same server running on two stacks, and sharing one
     * store across both would mean a browser on Ethernet showing registers a
     * Wi-Fi master wrote -- interesting, but it makes the store a synchronising
     * point between two interfaces and that is a different example. The web UI
     * reports the Ethernet side, which is the one it can configure.
     */
    if (WIFI_CONFIGURED) {
        mb_store_t *wifi_store = mb_store_create();
        if (wifi_store != NULL) {
            mb_server_start("wifi", &net_wifi_ops, WIFI_MB_PORT, wifi_store,
                            wifi_net_is_up);
        }
    }
}
