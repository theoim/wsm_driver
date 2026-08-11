/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * DHCP & DNS example configuration.
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

/* ---- static network identity (wsm_driver style: wiz_NetInfo byte arrays) ----
 * main.c sets .dhcp = NETINFO_DHCP, so ip/sn/gw/dns below are only the values
 * the chip carries until the first lease arrives; the DHCP server overrides
 * them. The MAC is NOT leased and is always taken from here. */
#define NET_MAC_ADDR          {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}  /* WIZnet OUI */
#define NET_IP_ADDR           {192, 168, 11, 2}
#define NET_SUBNET_MASK       {255, 255, 255, 0}
#define NET_GATEWAY           {192, 168, 11, 1}
#define NET_DNS_ADDR          {8, 8, 8, 8}

/* ---- Wi-Fi STA config (fill in your AP credentials) ---- */
#define WIFI_SSID             "your-ssid"
#define WIFI_PASS             "your-password"

/* ---- DHCP / DNS application config (shared by the engine + both vtables) ---- */
#define DHCP_DNS_DOMAIN       "www.wiznet.io"   /* hostname resolved on each interface */
#define DHCP_DNS_RETRY_COUNT  5                 /* DHCP rounds tolerated before giving up */
#define DNS_RETRY_COUNT       5                 /* DNS  timeouts tolerated before giving up */
#define DHCP_DNS_BUF_SIZE     (1024 * 2)        /* shared DHCP/DNS message buffer */

/* ---- socket-based DHCP client timing (dhcp_dns.c) ----
 * The client speaks RFC 2131 over ops->sock, so it owns its own retransmission
 * schedule; there is no ioLibrary 1-second tick any more. One "round" is
 * DHCP_XMIT_TRIES transmits with a linear backoff (1x, 2x, 3x ... the interval);
 * when a round expires the engine counts it against DHCP_DNS_RETRY_COUNT. */
#define DHCP_XMIT_TRIES       4                 /* transmits before a round is declared failed */
#define DHCP_XMIT_INTERVAL_MS 2000              /* base retransmit spacing (x1, x2, x3, ...) */
#define DHCP_RECV_TIMEOUT_MS  500               /* SO_RCVTIMEO while waiting for OFFER/ACK */
#define DHCP_STEP_PACKETS     4                 /* datagrams handled per poll (DISCOVER..ACK) */
#define DHCP_DEFAULT_RENEW_S  1800              /* T1 when the server sends no lease time */
#define DNS_RECV_TIMEOUT_MS   3000              /* SO_RCVTIMEO while waiting for the A record */

/* NOTE: the WIZnet hardware sockets are no longer pinned here. dhcp_dns.c calls
 * ops->sock->socket(), which under the TOE --wrap allocates a free hardware
 * socket via wiztoe_socket() — the same way the loopback example does. */

#endif /* NET_CONFIG_H */
