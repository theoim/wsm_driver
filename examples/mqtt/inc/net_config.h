/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * MQTT client example configuration.
 *
 * Follows wsm_driver's config conventions:
 *   - SPI / pin wiring is NOT configured here. It comes from the component
 *     Kconfig (menuconfig -> Component config -> WIZnet WSM Driver) and is
 *     applied by the component's TOE backend via wsm_driver_spi_config_t.
 *   - The network identity is expressed as wiz_NetInfo fields (byte arrays);
 *     main.c assembles a wiz_NetInfo from these macros and hands it to
 *     wiznet_net_init(), which applies it with wizchip_setnetinfo().
 *   - What the client does (publish / subscribe / both) is a menuconfig choice:
 *     MQTT Example Configuration -> MQTT mode.
 */
#ifndef NET_CONFIG_H
#define NET_CONFIG_H

/* ---- static network identity (wsm_driver style: wiz_NetInfo byte arrays) ---- */
#define NET_MAC_ADDR            {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}  /* WIZnet OUI */
#define NET_IP_ADDR             {192, 168, 11, 2}
#define NET_SUBNET_MASK         {255, 255, 255, 0}
#define NET_GATEWAY             {192, 168, 11, 1}
#define NET_DNS_ADDR            {8, 8, 8, 8}

/* ---- Wi-Fi STA config (fill in your AP credentials) ----
 * Leave WIFI_SSID empty to run Ethernet-only; main.c then skips Wi-Fi entirely. */
#define WIFI_SSID               "your-ssid"
#define WIFI_PASS               "your-password"

/* ---- broker ---- */
#define MQTT_BROKER_IP          "192.168.11.100"   /* the PC running mosquitto */
#define MQTT_BROKER_PORT        1883

/* ---- session ----
 * One client ID per CONNECTION: a broker closes the older session when a second
 * client connects with the same ID, so Ethernet and Wi-Fi must not share one. */
#define MQTT_CLIENT_ID_ETH      "esp32s3-wiz-toe-eth"
#define MQTT_CLIENT_ID_WIFI     "esp32s3-wiz-toe-wifi"
/* Leave MQTT_USERNAME empty for an anonymous broker; a password without a
 * username is not allowed by MQTT 3.1.1 and is dropped. */
#define MQTT_USERNAME           "wiznet"
#define MQTT_PASSWORD           "0123456789"
#define MQTT_KEEP_ALIVE_S       60

/* ---- what the client publishes / subscribes to ----
 * Which of these are actually used follows the menuconfig MQTT mode.
 *
 * One topic per INTERFACE, not one per example: sharing them would make the two
 * publishes indistinguishable at the broker and would deliver every inbound
 * message to both clients. The common prefix is kept so a single wildcard still
 * covers both — subscribe to "publish_topic/#" to watch them together, or to
 * "publish_topic/eth" to watch one. */
#define MQTT_PUBLISH_TOPIC_ETH    "publish_topic/eth"
#define MQTT_PUBLISH_TOPIC_WIFI   "publish_topic/wifi"
#define MQTT_PUBLISH_PAYLOAD_ETH  "Hello, World! from eth"
#define MQTT_PUBLISH_PAYLOAD_WIFI "Hello, World! from wifi"
#define MQTT_PUBLISH_PERIOD_MS    (10 * 1000)
#define MQTT_SUBSCRIBE_TOPIC_ETH  "subscribe_topic/eth"
#define MQTT_SUBSCRIBE_TOPIC_WIFI "subscribe_topic/wifi"

/* ---- engine tuning ---- */
/* SO_RCVTIMEO on the broker socket: how long recv() waits before the engine
 * gets a turn to publish, ping, or notice a dead link. Also the granularity of
 * MQTT_PUBLISH_PERIOD_MS. */
#define MQTT_POLL_MS            1000
/* How long to wait for CONNACK / SUBACK, and for the rest of a packet whose
 * fixed header has already arrived. */
#define MQTT_ACK_TIMEOUT_MS     (5 * 1000)
/* Delay before rebuilding a dropped connection. */
#define MQTT_RECONNECT_MS       (5 * 1000)
/* Per-task TX and RX buffers: the largest packet the client can build or
 * accept. Bigger incoming publishes are dropped with a warning. */
#define MQTT_BUF_SIZE           2048

#endif /* NET_CONFIG_H */
