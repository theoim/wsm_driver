/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Ethernet side of the network install check.
 *
 * The check differs by backend, because "who owns the chip" differs:
 *
 *   TOE backend - net_backend_toe.c registers ioLibrary's SPI callbacks, so the
 *     chip's PHY registers are readable directly (wizphy_getphylink /
 *     wizphy_getphyconf). Same logic and same messages as the original
 *     WIZnet-PICO-C example; only the SPI/chip bring-up moved out.
 *
 *   ETH backend - esp_eth owns the chip through its own SPI driver and
 *     ioLibrary's callback table is never populated. Calling wizphy_* here
 *     dereferences that empty table and panics (StoreProhibited at 0x0), so the
 *     link state comes from the driver instead, via wiznet_net_is_up() which
 *     tracks ETHERNET_EVENT_CONNECTED.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "wizchip_conf.h"

#include "link_check.h"
#include "net_config.h"     /* LINK_CHECK_INTERVAL_MS, LINK_CHECK_MAX_RETRY */

static const char *TAG = "link_check";

typedef struct {
    const char   *name;
    wiz_NetInfo   net_info;      /* copied: the caller's may be const/static */
    bool        (*is_up)(void);
} link_ctx_t;

static void print_ping_hint(const link_ctx_t *c)
{
    ESP_LOGI(TAG, "[%s] Try ping the ip:%d.%d.%d.%d.", c->name,
             c->net_info.ip[0], c->net_info.ip[1],
             c->net_info.ip[2], c->net_info.ip[3]);
}

static void print_cable_hint(const link_ctx_t *c)
{
    ESP_LOGE(TAG, "[%s] Please check whether the network cable is loose or disconnected.",
             c->name);
}

#if defined(CONFIG_ESP_WIZ_TOE_BACKEND_TOE)
/* ---- TOE backend: read the chip's PHY registers through ioLibrary ---- */
static void link_check_run(link_ctx_t *c)
{
    uint8_t link_status;
    uint16_t count = 0;

    ESP_LOGI(TAG, "[%s] waiting for chip init...", c->name);
    while (!c->is_up()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    do {
        link_status = wizphy_getphylink();
        ESP_LOGI(TAG, "[%s] PHY link: %u", c->name, (unsigned)link_status);

        if (link_status == PHY_LINK_OFF) {
            count++;
            if (count > LINK_CHECK_MAX_RETRY) {
                ESP_LOGE(TAG, "[%s] Link failed of Internal PHY.", c->name);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(LINK_CHECK_INTERVAL_MS));
    } while (link_status == PHY_LINK_OFF);

    if (link_status == PHY_LINK_ON) {
        wiz_PhyConf phyconf;
        wizphy_getphystat(&phyconf);   /* negotiated link, not the advertised config */

        ESP_LOGI(TAG, "[%s] Link OK of Internal PHY.", c->name);
        /* Kept exactly as the original WIZnet-PICO-C example prints it, including
         * the inverted-looking ternary (PHY_SPEED_10 -> "100"). Do not "fix" this
         * without checking the original first -- see README. */
        ESP_LOGI(TAG, "[%s] the %d Mbtis speed of Internal PHY.", c->name,
                 phyconf.speed == PHY_SPEED_100 ? 100 : 10);
        ESP_LOGI(TAG, "[%s] The %s Duplex Mode of the Internal PHY.", c->name,
                 phyconf.duplex == PHY_DUPLEX_FULL ? "Full-Duplex" : "Half-Duplex");
        print_ping_hint(c);
    } else {
        print_cable_hint(c);
    }
}

#else
/* ---- ETH backend: ask the esp_eth driver, never ioLibrary ----
 * wiznet_net_is_up() is false until ETHERNET_EVENT_CONNECTED, so polling it IS
 * the link check here. Speed/duplex are not printed: esp_eth knows them, but
 * net_backend_eth.c keeps its esp_eth_handle_t private, and the driver already
 * logs "Ethernet link up" plus the address on its own. */
static void link_check_run(link_ctx_t *c)
{
    bool up = false;

    ESP_LOGI(TAG, "[%s] waiting for Ethernet link...", c->name);
    for (int i = 0; i <= LINK_CHECK_MAX_RETRY; i++) {
        if (c->is_up()) {
            up = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(LINK_CHECK_INTERVAL_MS));
    }

    if (up) {
        ESP_LOGI(TAG, "[%s] Link OK of Internal PHY.", c->name);
        print_ping_hint(c);
    } else {
        ESP_LOGE(TAG, "[%s] Link failed of Internal PHY.", c->name);
        print_cable_hint(c);
    }
}
#endif /* CONFIG_ESP_WIZ_TOE_BACKEND_TOE */

static void link_check_task(void *arg)
{
    link_ctx_t *c = (link_ctx_t *)arg;

    link_check_run(c);

    free(c);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void link_check_start(const char *name, const wiz_NetInfo *net_info,
                      bool (*is_up)(void))
{
    link_ctx_t *c = malloc(sizeof(*c));
    if (c == NULL) {
        ESP_LOGE(TAG, "[%s] out of memory", name);
        return;
    }
    c->name     = name;
    c->net_info = *net_info;
    c->is_up    = is_up;

    if (xTaskCreate(link_check_task, name, 4096, c, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "[%s] xTaskCreate failed", name);
        free(c);
    }
}
