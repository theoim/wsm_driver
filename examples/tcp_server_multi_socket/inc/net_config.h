/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * TCP server multi-socket example configuration.
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

/* ---- multi-socket server config ----
 *
 * ONE PORT PER SOCKET, as in the original WIZnet-PICO-C example: listener i
 * binds PORT_BASE + i. The WIZnet chip cannot have several hardware sockets
 * listening on the SAME port, so the original spreads them across consecutive
 * ports and this port stays a per-socket value. The Wi-Fi side uses its own
 * base so the two never clash when they share one LwIP stack (SOCKET_WRAP=0).
 *
 * MULTI_SOCKET_COUNT is 8 to match the chip's 8 hardware sockets (the original
 * round-robins over _WIZCHIP_SOCK_NUM_). Each listener costs one task on both
 * interfaces; lower this if you are short on RAM.
 */
#define MULTI_SOCKET_PORT_BASE       5000   /* Ethernet: 5000..5000+COUNT-1 */
#define WIFI_MULTI_SOCKET_PORT_BASE  5100   /* Wi-Fi:    5100..5100+COUNT-1 */
#define MULTI_SOCKET_COUNT           8
#define MULTI_SOCKET_BUF_SIZE        2048

#endif /* NET_CONFIG_H */
