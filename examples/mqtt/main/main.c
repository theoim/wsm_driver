/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/*
 * MQTT client on the WIZnet TOE (W5500 / W6300), ported from
 * WIZnet-PICO-C examples/mqtt (publish / subscribe / publish_subscribe).
 *
 * app_main only orchestrates: bring the interfaces up, start an MQTT client on
 * each, and return. The client logic lives in the backend-neutral engine
 * mqtt.c, which takes a socket vtable, so both interfaces run the very same
 * code at the same level:
 *   - Ethernet (WIZnet chip) as MQTT_CLIENT_ID_ETH  (vtable: net_eth_ops)
 *   - Wi-Fi STA              as MQTT_CLIENT_ID_WIFI (vtable: net_wifi_ops)
 *
 * Which stack actually carries the traffic is decided by the LINKER, from
 * `Component config -> WIZnet WSM Driver -> Network backend`:
 *   - TOE (hardware TCP/IP): net_eth_ops' plain lwip_* calls are redirected to
 *     the chip's hardware sockets by -Wl,--wrap, i.e. every call in mqtt.c ends
 *     up in __wrap_lwip_* (see wiztoe_wrap.c);
 *   - esp_eth MACRAW + software LwIP: there is no wrap, so the same calls end up
 *     in lwip_* and run over the software stack.
 * mqtt.c contains no #if for this — only the vtable it is handed differs, and
 * net_wifi_ops bypasses the wrap so Wi-Fi always reaches the real LwIP.
 *
 * The ioLibrary MQTT client (Internet/MQTT: MQTTClient.c, mqtt_interface.c) is
 * NOT used: its Network struct drives the chip's socket registers directly
 * through ioLibrary's own socket()/send()/recv(), so --wrap has nothing to
 * intercept and one source could not serve both backends.
 *
 * Wi-Fi is optional: leave WIFI_SSID empty in net_config.h to run Ethernet-only.
 *
 * Config conventions follow wsm_driver:
 *   - SPI / pins  -> component Kconfig, applied by the TOE backend.
 *   - network id  -> the wiz_NetInfo below (byte arrays from net_config.h),
 *                    applied by wiznet_net_init() -> wizchip_setnetinfo().
 *   - broker /
 *     topics      -> inc/net_config.h.
 *   - MQTT mode   -> menuconfig: MQTT Example Configuration -> MQTT mode.
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
#include "mqtt.h"

/* MQTT mode (menuconfig): which halves of the client are wired up. */
#if defined(CONFIG_EXAMPLE_MQTT_PUBLISH) || defined(CONFIG_EXAMPLE_MQTT_PUBLISH_SUBSCRIBE)
#define DO_PUBLISH 1
#endif
#if defined(CONFIG_EXAMPLE_MQTT_SUBSCRIBE) || defined(CONFIG_EXAMPLE_MQTT_PUBLISH_SUBSCRIBE)
#define DO_SUBSCRIBE 1
#endif

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

    /* A field left out here stays NULL/0, which is how the engine is told not to
     * do that half of the job — so the MQTT mode is expressed once, as presence
     * or absence of a topic, and mqtt.c needs no #if at all. */
    mqtt_config_t cfg = {
        .broker_ip     = MQTT_BROKER_IP,
        .broker_port   = MQTT_BROKER_PORT,
        .client_id     = MQTT_CLIENT_ID_ETH,
        .username      = MQTT_USERNAME,
        .password      = MQTT_PASSWORD,
        .keepalive_s   = MQTT_KEEP_ALIVE_S,
#ifdef DO_PUBLISH
        .pub_topic     = MQTT_PUBLISH_TOPIC_ETH,
        .pub_payload   = MQTT_PUBLISH_PAYLOAD_ETH,
        .pub_period_ms = MQTT_PUBLISH_PERIOD_MS,
#endif
#ifdef DO_SUBSCRIBE
        .sub_topic     = MQTT_SUBSCRIBE_TOPIC_ETH,
#endif
    };

    /* Start both clients as sibling tasks; each waits for its own link. Same
     * call shape — only the label, vtable, identity and readiness predicate
     * differ. mqtt_client_start() copies the config, so one local serves both. */
    mqtt_client_start("eth", &net_eth_ops, &cfg, wiznet_net_is_up);
    if (WIFI_CONFIGURED) {
        /* Everything that identifies the client on the broker is swapped, not
         * just the ID: a shared client ID makes the broker kick the older
         * session, and shared topics would make the two publishes
         * indistinguishable and deliver every inbound message to both. The
         * #ifdefs match the ones above so a topic left out by the MQTT mode
         * stays NULL here too — that NULL is what the engine reads as "skip
         * this half". */
        cfg.client_id = MQTT_CLIENT_ID_WIFI;
#ifdef DO_PUBLISH
        cfg.pub_topic   = MQTT_PUBLISH_TOPIC_WIFI;
        cfg.pub_payload = MQTT_PUBLISH_PAYLOAD_WIFI;
#endif
#ifdef DO_SUBSCRIBE
        cfg.sub_topic   = MQTT_SUBSCRIBE_TOPIC_WIFI;
#endif
        mqtt_client_start("wifi", &net_wifi_ops, &cfg, wifi_net_is_up);
    }
}
