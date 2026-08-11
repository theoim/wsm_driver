/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Wi-Fi side of the network install check.
 *
 * The Ethernet check (link_check.c) reads the WIZnet chip's PHY registers. Wi-Fi
 * has no such registers, so the equivalent information comes from esp_wifi and
 * esp_netif: are we associated, to what, how strong, and what address did we
 * get. No socket vtable is involved here -- this example makes no socket calls.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"

#include "link_check.h"
#include "net_config.h"     /* LINK_CHECK_INTERVAL_MS, WIFI_LINK_CHECK_MAX_RETRY */

static const char *TAG = "link_check";

typedef struct {
    const char *name;
    bool      (*is_up)(void);
} wifi_link_ctx_t;

/* Human-readable auth mode, so the log says something useful when an AP is
 * open or still on WEP. */
static const char *authmode_str(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3-PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3-PSK";
    default:                        return "other";
    }
}

static void wifi_link_check_task(void *arg)
{
    wifi_link_ctx_t *c = (wifi_link_ctx_t *)arg;
    bool up = false;

    ESP_LOGI(TAG, "[%s] waiting for association...", c->name);
    for (int i = 0; i < WIFI_LINK_CHECK_MAX_RETRY; i++) {
        if (c->is_up()) {
            up = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(LINK_CHECK_INTERVAL_MS));
    }

    if (!up) {
        ESP_LOGE(TAG, "[%s] Link failed of Wi-Fi STA.", c->name);
        ESP_LOGE(TAG, "[%s] Please check WIFI_SSID / WIFI_PASS in net_config.h "
                      "and that the AP is in range.", c->name);
        goto done;
    }

    ESP_LOGI(TAG, "[%s] Link OK of Wi-Fi STA.", c->name);

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        ESP_LOGI(TAG, "[%s] SSID \"%s\", channel %u, %s",
                 c->name, (const char *)ap.ssid, (unsigned)ap.primary,
                 authmode_str(ap.authmode));
        /* RSSI is the closest Wi-Fi analogue to "link quality" on the PHY side:
         * roughly -50 excellent, -70 usable, below -80 expect retries. */
        ESP_LOGI(TAG, "[%s] RSSI %d dBm of the Wi-Fi link.", c->name, ap.rssi);
    } else {
        ESP_LOGW(TAG, "[%s] associated but AP info unavailable", c->name);
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        ESP_LOGI(TAG, "[%s] Try ping the ip:" IPSTR ".", c->name, IP2STR(&ip.ip));
    } else {
        ESP_LOGW(TAG, "[%s] no IPv4 address on the STA netif", c->name);
    }

done:
    free(c);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void wifi_link_check_start(const char *name, bool (*is_up)(void))
{
    wifi_link_ctx_t *c = malloc(sizeof(*c));
    if (c == NULL) {
        ESP_LOGE(TAG, "[%s] out of memory", name);
        return;
    }
    c->name  = name;
    c->is_up = is_up;

    if (xTaskCreate(wifi_link_check_task, name, 4096, c, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "[%s] xTaskCreate failed", name);
        free(c);
    }
}
