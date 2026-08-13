/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Runtime configuration: what the web UI can change and NVS remembers.
 *
 * Deliberately small. IP, mask, gateway and the Modbus port are the four things
 * a technician standing in front of an installed device needs to change without
 * a toolchain. MAC, DNS and the unit id stay where they are -- the MAC is the
 * device's identity and the other two are not worth a settings page nobody will
 * read.
 *
 * net_config.h keeps its role as the factory defaults. NVS holds the deltas,
 * and anything missing, corrupt or from an older layout falls back to the
 * header rather than to zeros: a device that boots on 0.0.0.0 after a bad write
 * is a device that needs a cable, and the whole point of this page is not
 * needing one.
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t  ip[4];
    uint8_t  mask[4];
    uint8_t  gateway[4];
    uint16_t modbus_port;
} app_config_t;

/*
 * Load the stored configuration, or the factory defaults if there is none.
 * Always leaves `out` usable. Returns true when the values came from NVS, which
 * is worth logging: "running on defaults" and "running on your settings" look
 * identical from the outside and are very different when something is wrong.
 */
bool app_config_load(app_config_t *out);

/* Write to NVS. Returns 0 on success. */
int app_config_save(const app_config_t *cfg);

/* Forget the stored configuration; the next boot uses the factory defaults. */
int app_config_reset(void);

/*
 * Check a candidate configuration.
 *
 * On failure, writes a short reason into `reason` -- the browser shows it, and
 * "invalid configuration" tells a user nothing about which of the four fields
 * they got wrong. Returns true when the configuration is usable.
 */
bool app_config_validate(const app_config_t *cfg, char *reason, size_t size);

/* Parse "192.168.11.2" into four octets. Returns false on anything else,
 * including trailing characters and octets over 255. */
bool app_config_parse_ip(const char *text, uint8_t out[4]);

#endif /* APP_CONFIG_H */
