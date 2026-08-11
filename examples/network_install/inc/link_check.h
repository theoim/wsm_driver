/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * PHY link / cable sanity check for first bring-up.
 *
 * Both interfaces are checked, but NOT through a shared socket vtable the way
 * the socket-based examples do it -- this example makes no socket calls at all.
 * Ethernet reads the WIZnet chip's PHY registers (wizphy_getphylink /
 * wizphy_getphyconf), which sits below the socket layer; Wi-Fi has no such
 * registers, so its check asks esp_wifi for the association instead. Two
 * engines, one question: "is this link usable?"
 */
#ifndef LINK_CHECK_H
#define LINK_CHECK_H

#include <stdbool.h>

#include "wizchip_conf.h"   /* wiz_NetInfo */

/*
 * Spawn a task that waits until is_up() reports the chip initialized, then
 * polls the PHY until the link comes up (or LINK_CHECK_MAX_RETRY expires) and
 * prints the negotiated speed / duplex plus the IP to ping.
 *
 *   name      - short label; also the task name and log tag (e.g. "eth")
 *   net_info  - the identity applied to the chip, used to print the ping target
 *   is_up     - predicate the task polls for chip-init readiness
 */
void link_check_start(const char *name, const wiz_NetInfo *net_info,
                      bool (*is_up)(void));

/*
 * Wi-Fi counterpart (wifi_link_check.c). Waits for the STA to associate and get
 * an IP, then prints the SSID, channel, RSSI and the address to ping. Gives up
 * after WIFI_LINK_CHECK_MAX_RETRY polls so bad credentials do not hang the log.
 *
 *   name  - short label; also the task name and log tag (e.g. "wifi")
 *   is_up - predicate the task polls for association + IP readiness
 */
void wifi_link_check_start(const char *name, bool (*is_up)(void));

#endif /* LINK_CHECK_H */
