/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * TFTP client example configuration.
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

/* ---- TFTP client config ----
 * Point these at a TFTP server on your PC (tftpd64 on Windows, tftpd-hpa on
 * Linux) with the file present in its served directory. */
#define TFTP_SERVER_IP        "192.168.11.4"
#define TFTP_FILE_NAME        "tftp_test_file.txt"

/* Bytes of the first block echoed to the log so a transfer is visibly a
 * transfer, not just a success code. */
#define TFTP_PREVIEW_BYTES    64

#endif /* NET_CONFIG_H */
