/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Wi-Fi (STA) bring-up, running ALONGSIDE the W5500 network backend (either
 * TOE or esp_eth). This is the "port" layer: it only brings the interface up;
 * what runs on top (echo server, socket vtable, ...) lives in the example.
 *
 * The example owns the credentials (its net_config.h) and passes them in — the
 * component hardcodes no SSID/password. When CONFIG_WSM_DRIVER_SOCKET_WRAP is
 * enabled (TOE), Wi-Fi traffic must reach the software LwIP via __real_lwip_*
 * to bypass the W5500 --wrap (see the example's wifi_loopback.c).
 */
#ifndef WIFI_BACKEND_H
#define WIFI_BACKEND_H

#include <stdbool.h>

/* Bring up Wi-Fi in STA mode and start connecting to `ssid`/`pass` (async).
 * Safe to call after wiznet_net_init() — tolerates esp_netif/event-loop already
 * being initialized. The strings are copied into the driver during the call. */
void wifi_net_init(const char *ssid, const char *pass);

/* True once the STA has an IPv4 address (IP_EVENT_STA_GOT_IP). */
bool wifi_net_is_up(void);

#endif /* WIFI_BACKEND_H */
