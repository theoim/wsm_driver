/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * NetBIOS example configuration.
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

/* ---- NetBIOS responder config ----
 * The names the board answers to. NetBIOS names are case-insensitive, at most
 * 15 characters, and must be UNIQUE on the segment — hence a separate name per
 * interface, so a host that can see both does not get two answers for one name.
 * (This is the same reason the loopback/http examples give each interface its
 * own port.) */
#define NETBIOS_NAME          "WIZNET"        /* Ethernet (WIZnet chip) */
#define WIFI_NETBIOS_NAME     "WIZNETWIFI"    /* Wi-Fi STA */

#define NETBIOS_PORT          137             /* NetBIOS name service (UDP) */
#define NETBIOS_BUF_SIZE      512             /* max NetBIOS datagram handled */

/* Bounds how long recvfrom() waits for a query, so a task on a quiet network
 * loops instead of looking wedged. Nothing else depends on the period. */
#define NETBIOS_RECV_TIMEOUT_MS (5 * 1000)

/* TTL (seconds) advertised in the name response — how long the querying host
 * may cache the answer. */
#define NETBIOS_NAME_TTL      10

#endif /* NET_CONFIG_H */
