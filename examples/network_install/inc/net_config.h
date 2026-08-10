/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Network install example configuration.
 *
 * Follows esp_wiz_toe's config conventions:
 *   - SPI / pin wiring is NOT configured here. It comes from the component
 *     Kconfig (menuconfig -> Component config -> WIZnet TOE Component) and is
 *     applied by net_backend_toe.c via esp_wiz_toe_spi_config_t.
 *   - The network identity is expressed as wiz_NetInfo fields (byte arrays);
 *     main.c assembles a wiz_NetInfo from these macros and hands it to
 *     wiznet_net_init(), which applies it with wizchip_setnetinfo().
 *
 * Wi-Fi is checked too, but differently from the socket-based examples: those
 * swap a socket vtable to run one engine on both interfaces, while this one has
 * no sockets at all. The Ethernet side reads the WIZnet chip's PHY registers;
 * the Wi-Fi side reports association state, IP and RSSI. Same purpose -- "is
 * this link actually usable?" -- reached two different ways.
 */
#ifndef NET_CONFIG_H
#define NET_CONFIG_H

/* ---- static network identity (esp_wiz_toe style: wiz_NetInfo byte arrays) ---- */
#define NET_MAC_ADDR          {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}  /* WIZnet OUI */
#define NET_IP_ADDR           {192, 168, 11, 2}
#define NET_SUBNET_MASK       {255, 255, 255, 0}
#define NET_GATEWAY           {192, 168, 11, 1}
#define NET_DNS_ADDR          {8, 8, 8, 8}

/* ---- Wi-Fi STA config (fill in your AP credentials) ---- */
#define WIFI_SSID             "your-ssid"
#define WIFI_PASS             "your-password"

/* ---- link check ----
 * Poll the PHY every LINK_CHECK_INTERVAL_MS and give up after
 * LINK_CHECK_MAX_RETRY consecutive "link off" reads, as in the original.
 *
 * The Wi-Fi side polls at the same interval but needs a longer budget: an
 * association plus DHCP lease takes seconds, where a PHY link is up in
 * milliseconds once the cable is in.
 */
#define LINK_CHECK_INTERVAL_MS  500
#define LINK_CHECK_MAX_RETRY    10
#define WIFI_LINK_CHECK_MAX_RETRY 40   /* 40 * 500 ms = 20 s */

#endif /* NET_CONFIG_H */
