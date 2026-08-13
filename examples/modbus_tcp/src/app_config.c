/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Runtime configuration in NVS (see app_config.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "net_config.h"

static const char *TAG = "cfg";

#define NVS_NAMESPACE  "modbus"
#define NVS_KEY        "netcfg"

/*
 * Stored with a magic and a version rather than as a bare struct.
 *
 * The magic catches a blob of the right size that happens to be there for some
 * other reason; the version catches this example growing a fifth field later.
 * Both failures land in the same place -- fall back to the factory defaults --
 * but silently loading a struct that used to mean something else would put the
 * device on an address nobody chose.
 */
#define CFG_MAGIC    0x57534D42u        /* "WSMB" */
#define CFG_VERSION  1

typedef struct {
    uint32_t     magic;
    uint16_t     version;
    app_config_t cfg;
} stored_config_t;

static void factory_defaults(app_config_t *out)
{
    const uint8_t ip[4]   = NET_IP_ADDR;
    const uint8_t mask[4] = NET_SUBNET_MASK;
    const uint8_t gw[4]   = NET_GATEWAY;

    memcpy(out->ip, ip, 4);
    memcpy(out->mask, mask, 4);
    memcpy(out->gateway, gw, 4);
    out->modbus_port = MB_PORT;
}

bool app_config_load(app_config_t *out)
{
    factory_defaults(out);

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGI(TAG, "no stored configuration; using factory defaults");
        return false;
    }

    stored_config_t stored;
    size_t size = sizeof(stored);
    esp_err_t err = nvs_get_blob(handle, NVS_KEY, &stored, &size);
    nvs_close(handle);

    if (err != ESP_OK || size != sizeof(stored)) {
        ESP_LOGI(TAG, "no stored configuration; using factory defaults");
        return false;
    }
    if (stored.magic != CFG_MAGIC || stored.version != CFG_VERSION) {
        ESP_LOGW(TAG, "stored configuration is magic 0x%08X version %u, "
                 "expected 0x%08X version %u -- using factory defaults",
                 (unsigned)stored.magic, stored.version,
                 (unsigned)CFG_MAGIC, CFG_VERSION);
        return false;
    }

    /* Validate what came back. A blob that passed the magic can still hold a
     * broken mask if it was written by a version with a weaker check. */
    char reason[64];
    if (!app_config_validate(&stored.cfg, reason, sizeof(reason))) {
        ESP_LOGW(TAG, "stored configuration rejected (%s) -- factory defaults",
                 reason);
        return false;
    }

    *out = stored.cfg;
    ESP_LOGI(TAG, "loaded stored configuration: %u.%u.%u.%u port %u",
             out->ip[0], out->ip[1], out->ip[2], out->ip[3], out->modbus_port);
    return true;
}

int app_config_save(const app_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return -1;
    }

    stored_config_t stored = {
        .magic = CFG_MAGIC,
        .version = CFG_VERSION,
        .cfg = *cfg,
    };

    err = nvs_set_blob(handle, NVS_KEY, &stored, sizeof(stored));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving configuration failed: %s", esp_err_to_name(err));
        return -1;
    }
    return 0;
}

int app_config_reset(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return -1;
    }
    esp_err_t err = nvs_erase_key(handle, NVS_KEY);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return (err == ESP_OK) ? 0 : -1;
}

bool app_config_parse_ip(const char *text, uint8_t out[4])
{
    unsigned a, b, c, d;
    char trailing;

    /* The %c catches "1.2.3.4.5" and "1.2.3.4 " -- sscanf alone stops at the
     * fourth number and would call both of those valid. */
    int matched = sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &trailing);
    if (matched != 4) {
        return false;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255) {
        return false;
    }

    out[0] = (uint8_t)a;
    out[1] = (uint8_t)b;
    out[2] = (uint8_t)c;
    out[3] = (uint8_t)d;
    return true;
}

static uint32_t as_u32(const uint8_t v[4])
{
    return ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) |
           ((uint32_t)v[2] << 8) | v[3];
}

bool app_config_validate(const app_config_t *cfg, char *reason, size_t size)
{
    uint32_t ip   = as_u32(cfg->ip);
    uint32_t mask = as_u32(cfg->mask);
    uint32_t gw   = as_u32(cfg->gateway);

    if (ip == 0 || cfg->ip[0] == 0) {
        snprintf(reason, size, "IP address cannot start with 0");
        return false;
    }
    if (cfg->ip[0] == 127) {
        snprintf(reason, size, "127.x is the loopback range");
        return false;
    }
    if (cfg->ip[0] >= 224) {
        snprintf(reason, size, "%u.x is multicast or reserved", cfg->ip[0]);
        return false;
    }

    /*
     * A mask has to be a run of ones followed by a run of zeros, so the host
     * part -- the inverted mask -- has to be a run of low ones. Adding one to
     * such a run carries all the way and leaves a single bit above it, so
     * host & (host + 1) is zero exactly when the mask is contiguous:
     *
     *     255.255.255.0  host 0x000000FF  +1 0x00000100  &  0
     *     255.255.0.255  host 0x0000FF00  +1 0x0000FF01  &  0xFF00
     */
    uint32_t host = ~mask;
    if (mask == 0 || (host & (host + 1)) != 0) {
        snprintf(reason, size, "subnet mask %u.%u.%u.%u is not contiguous",
                 cfg->mask[0], cfg->mask[1], cfg->mask[2], cfg->mask[3]);
        return false;
    }

    if ((ip & ~mask) == 0) {
        snprintf(reason, size, "IP is the network address of its own subnet");
        return false;
    }
    if ((ip & ~mask) == (~mask & 0xFFFFFFFFu)) {
        snprintf(reason, size, "IP is the broadcast address of its own subnet");
        return false;
    }

    /* A zero gateway means "none", which is legitimate on an isolated segment.
     * A gateway outside the subnet is not: nothing could ever reach it, and the
     * device would look configured while being unroutable. */
    if (gw != 0 && (gw & mask) != (ip & mask)) {
        snprintf(reason, size, "gateway %u.%u.%u.%u is outside the subnet",
                 cfg->gateway[0], cfg->gateway[1], cfg->gateway[2],
                 cfg->gateway[3]);
        return false;
    }
    if (gw == ip) {
        snprintf(reason, size, "gateway is the device's own address");
        return false;
    }

    if (cfg->modbus_port == 0) {
        snprintf(reason, size, "port must be 1..65535");
        return false;
    }
    if (cfg->modbus_port == WEB_PORT) {
        snprintf(reason, size, "port %u is the web UI's own port", WEB_PORT);
        return false;
    }

    reason[0] = '\0';
    return true;
}
