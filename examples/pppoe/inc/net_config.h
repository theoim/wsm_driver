/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * PPPoE example configuration.
 *
 * Follows wsm_driver's config conventions:
 *   - SPI / pin wiring is NOT configured here. It comes from the component
 *     Kconfig (menuconfig -> Component config -> WIZnet WSM Driver) and is
 *     applied by net_backend_toe.c via wsm_driver_spi_config_t.
 *   - The network identity is expressed as wiz_NetInfo fields (byte arrays);
 *     main.c assembles a wiz_NetInfo from these macros and hands it to
 *     wiznet_net_init(), which applies it with wizchip_setnetinfo().
 *
 * There is no Wi-Fi section here, unlike the socket-based examples: PPPoE is
 * negotiated by the WIZnet chip through its own registers, and Wi-Fi has no
 * equivalent.
 */
#ifndef NET_CONFIG_H
#define NET_CONFIG_H

/* ---- static network identity (wsm_driver style: wiz_NetInfo byte arrays) ----
 * Used only for the pre-PPPoE bring-up; PPPoE replaces the IP/gateway with what
 * the peer assigns. */
#define NET_MAC_ADDR          {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}  /* WIZnet OUI */
#define NET_IP_ADDR           {192, 168, 11, 2}
#define NET_SUBNET_MASK       {255, 255, 255, 0}
#define NET_GATEWAY           {192, 168, 11, 1}
#define NET_DNS_ADDR          {8, 8, 8, 8}

/* ---- PPPoE credentials (set these to what your ISP / test server expects) ----
 * The lengths PPPoE.c uses are derived with sizeof()-1, so just edit the
 * strings; do not pad them to a fixed width. */
#define PPPOE_ID              "W5100S"
#define PPPOE_PW              "WIZnet"

/* Buffer handed to ppp_start(); referenced by the vendored PPPoE.c as gDATABUF. */
#define PPPOE_DATA_BUF_SIZE   2048

#endif /* NET_CONFIG_H */
