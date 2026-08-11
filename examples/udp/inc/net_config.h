/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * UDP example configuration.
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
#define WIFI_SSID             ""
#define WIFI_PASS             ""

/* ---- UDP echo application config ---- */
#define UDP_ECHO_PORT         5000    /* server role, Ethernet: bind here        */
#define UDP_ECHO_CLIENT_PORT  50000   /* client role, Ethernet: bind here
                                         (matches the ioLibrary any_port)        */
/* Wi-Fi binds different ports. With SOCKET_WRAP=0 both interfaces share one LwIP
 * stack, so identical ports would clash on bind; keeping them apart makes the
 * example correct for either backend. */
#define WIFI_UDP_ECHO_PORT        5001
#define WIFI_UDP_ECHO_CLIENT_PORT 50001

#define UDP_ECHO_BUF_SIZE     2048

#endif /* NET_CONFIG_H */
