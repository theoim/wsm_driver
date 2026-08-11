/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * UPnP IGD client example configuration.
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

/* The same address as a string. The SUBSCRIBE callback header has to carry it,
 * and the XML builder takes it as text so it never has to read it back off the
 * chip -- that is what kept the original MakeXML.c tied to getSIPR(). */
#define NET_IP_ADDR_STR       "192.168.11.2"

/* ---- Wi-Fi STA config (fill in your AP credentials) ----
 * Leave WIFI_SSID empty to run Ethernet-only; main.c then skips Wi-Fi entirely. */
#define WIFI_SSID             ""
#define WIFI_PASS             ""

/* Set to 1 to drive the session over Wi-Fi instead of Ethernet. Like the tftp
 * example this runs one interface at a time: upnp_core.c keeps the discovered
 * IGD in globals, so two concurrent sessions would share it. */
#define UPNP_OVER_WIFI        0

/* ---- port mapping the example asks the router for ----
 * Leave UPNP_MAP_INT_IP empty to map to whichever interface is running the
 * session. Pinning it to NET_IP_ADDR_STR would point a Wi-Fi run at the
 * Ethernet address, which is not where the traffic would arrive. */
#define UPNP_MAP_PROTOCOL     "TCP"
#define UPNP_MAP_EXT_PORT     8000
#define UPNP_MAP_INT_IP       ""
#define UPNP_MAP_INT_PORT     8000
#define UPNP_MAP_DESCRIPTION  "wsm_driver"

/* Delete the mapping again at the end of the run. Leave at 1 so repeated runs
 * start from a clean router table; set to 0 to inspect the mapping in the
 * router's admin page afterwards. */
#define UPNP_DELETE_AFTER_ADD 1

/* Seconds to keep listening for eventing notifications after subscribing.
 * 0 skips the eventing step entirely. */
#define UPNP_EVENT_LISTEN_SEC 10

#endif /* NET_CONFIG_H */
