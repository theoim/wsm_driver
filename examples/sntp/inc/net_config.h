/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * SNTP example configuration.
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

/* ---- SNTP application config ----
 *
 * The server is given as an IPv4 literal, as in the original example: this
 * client does no name resolution, so that DNS is not a second thing that can
 * fail while bringing a board up. 216.239.35.0 is time.google.com.
 *
 * Each interface binds its own local UDP port. They only actually share a port
 * space when the wrap is off (ETH backend, one LwIP stack for both), but
 * keeping them distinct means the same config works in every combination.
 */
#define SNTP_SERVER_IP        "216.239.35.0"   /* time.google.com */
#define SNTP_SERVER_PORT      123
#define SNTP_LOCAL_PORT       5000             /* Ethernet local UDP port */
#define WIFI_SNTP_LOCAL_PORT  5001             /* Wi-Fi local UDP port */

#define SNTP_TIMEOUT_MS       (1000 * 10)      /* per attempt, as in the original */
#define SNTP_RETRY_COUNT      3

/* Local time offset applied to the UTC the server returns. The original example
 * used ioLibrary's TIMEZONE index 40 (Korea, UTC+9); here it is plain minutes. */
#define SNTP_TZ_OFFSET_MIN    (9 * 60)

#endif /* NET_CONFIG_H */
