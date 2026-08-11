/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * HTTP server example configuration.
 *
 * Follows wsm_driver's config conventions:
 *   - SPI / pin wiring is NOT configured here. It comes from the component
 *     Kconfig (menuconfig -> Component config -> WIZnet WSM Driver) and is
 *     applied by the component's TOE backend via wsm_driver_spi_config_t.
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
#define WIFI_SSID             "your-ssid"
#define WIFI_PASS             "your-password"

/* ---- HTTP server config ---- */
#define HTTP_PORT             80      /* Ethernet */
/* Wi-Fi listens on a different port: with SOCKET_WRAP=0 both interfaces share one
 * LwIP stack, where the same port would clash on bind. */
#define WIFI_HTTP_PORT        8080

/* Bounds how long recv() waits for a request line, and therefore also how long
 * accept() blocks — a task that never sees a client still loops instead of
 * wedging. */
#define HTTP_RECV_TIMEOUT_MS  (10 * 1000)

/* One buffer per server task: holds the request headers while they are parsed,
 * then the response header before the body is streamed out. */
#define HTTP_BUF_SIZE         2048

#endif /* NET_CONFIG_H */
