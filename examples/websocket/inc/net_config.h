/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * WebSocket echo server example configuration.
 *
 * Follows wsm_driver's config conventions:
 *   - SPI / pin wiring is NOT configured here. It comes from the component
 *     Kconfig (menuconfig -> Component config -> WIZnet WSM Driver -> Board)
 *     and is applied by the component's TOE backend.
 *   - The network identity is expressed as wiz_NetInfo fields (byte arrays);
 *     main.c assembles a wiz_NetInfo from these macros and hands it to
 *     wiznet_net_init(), which applies it with wizchip_setnetinfo().
 */
#ifndef NET_CONFIG_H
#define NET_CONFIG_H

/* ---- static network identity (wsm_driver style: wiz_NetInfo byte arrays) ---- */
#define NET_MAC_ADDR          {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}  /* WIZnet OUI */
#define NET_IP_ADDR           {192, 168, 11, 2}
#define NET_SUBNET_MASK       {255, 255, 255, 0}
#define NET_GATEWAY           {192, 168, 11, 1}
#define NET_DNS_ADDR          {8, 8, 8, 8}

/* ---- Wi-Fi STA config (fill in your AP credentials) ----
 * Leave WIFI_SSID empty to run Ethernet-only; main.c then skips Wi-Fi entirely. */
#define WIFI_SSID             ""
#define WIFI_PASS             ""

/* ---- WebSocket server config ----
 * Port 80 lets a browser reach it as ws://192.168.11.2/ with no port suffix.
 * The two interfaces bind different ports because with SOCKET_WRAP=0 (esp_eth
 * backend) they share one LwIP stack, where the same port would clash on bind. */
#define WS_PORT               80      /* Ethernet */
#define WIFI_WS_PORT          81      /* Wi-Fi    */

/* Largest message the server will assemble, fragments included. A browser can
 * send more than the chip's 2 KB socket buffer holds, so this is about what the
 * application is willing to buffer, not about the transport. */
#define WS_MAX_MESSAGE_SIZE   2048

#endif /* NET_CONFIG_H */
