/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * TCP client over SSL example configuration.
 *
 * Follows wsm_driver's config conventions:
 *   - SPI / pin wiring is NOT configured here. It comes from the component
 *     Kconfig (menuconfig -> Component config -> WIZnet WSM Driver) and is
 *     applied by net_backend_toe.c via wsm_driver_spi_config_t.
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

/* ---- Wi-Fi STA config (fill in your AP credentials) ---- */
#define WIFI_SSID             "your-ssid"
#define WIFI_PASS             "your-password"

/* ---- TLS client config ----
 *
 * Point this at your TLS server. Given as an IPv4 literal, as in the original
 * example: this client does no name resolution, so DNS is not a second thing
 * that can fail while bringing a board up.
 *
 * Certificate verification is disabled (see ssl_client.c) to keep the demo
 * dependency-free, same as the original.
 */
#define SSL_TARGET_IP         "192.168.11.4"
#define SSL_TARGET_PORT       443

#define SSL_RECV_TIMEOUT_MS   (1000 * 10)
#define SSL_RETRY_DELAY_MS    (1000 * 5)   /* wait before reconnecting */
#define SSL_BUF_SIZE          2048
#define SSL_HELLO_MSG         " W5x00 TCP over SSL test\n"

#endif /* NET_CONFIG_H */
