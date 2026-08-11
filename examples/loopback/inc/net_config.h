/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Loopback example configuration.
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

/* ---- loopback application config (shared by loopback.c + wifi_loopback.c) ---- */
#define LOOPBACK_PORT         5000              /* Ethernet (W5500) echo port */
#define WIFI_LOOPBACK_PORT    5001              /* Wi-Fi echo port — kept != Ethernet so the
                                                   shared LwIP stack has no bind clash */
#define LOOPBACK_TARGET_IP    "192.168.11.100"  /* TCP-client mode destination */
#define LOOPBACK_TARGET_PORT  5000
#define LOOPBACK_BUF_SIZE     2048

#endif /* NET_CONFIG_H */
