/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * PPPoE session bring-up. Same logic and same messages as the original
 * WIZnet-PICO-C example; only the SPI/chip bring-up moved out (it now comes
 * from wiznet_net_init(), see main.c) and the credentials moved to
 * net_config.h.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "wizchip_conf.h"

/* The vendored PPPoE.c carries its own W5500 / W6300 branches (MR+PSID+PTIMER
 * vs NETMR2+PSIDR+PTMR), so both chips are supported. Other WIZnet parts are
 * not: W5100S needs a different PPPoE.c path and is untested here. */
#if (_WIZCHIP_ != W5500) && (_WIZCHIP_ != W6300)
#error "The pppoe example supports W5500 and W6300 only."
#endif

/* PPPoE is negotiated by driving the chip's own registers through ioLibrary, so
 * this example only works when the TOE backend owns the chip. Under the ETH
 * backend esp_eth owns it instead (MACRAW + software LwIP) and never registers
 * ioLibrary's SPI callbacks, so the first WIZCHIP_READ() falls through to
 * ioLibrary's default parallel-bus path and writes to address 0 -- a
 * StoreProhibited panic a couple of seconds after boot. There is no ETH-backend
 * equivalent to fall back to, so fail here instead of at runtime.
 * Fix: set CONFIG_ESP_WIZ_TOE_BACKEND_TOE=y (menuconfig -> Component config ->
 * WIZnet TOE Component -> Network backend), or delete sdkconfig so this
 * example's sdkconfig.defaults applies. */
#if !defined(CONFIG_ESP_WIZ_TOE_BACKEND_TOE)
#error "The pppoe example requires the TOE backend (CONFIG_ESP_WIZ_TOE_BACKEND_TOE)."
#endif

/* PPPoE.h does `#include "socket.h"`, meaning ioLibrary's Ethernet/socket.h.
 * That only resolves correctly because this file is compiled with -iquote
 * pointing at ioLibrary's Ethernet/ -- ESP-IDF's lwIP port also ships a
 * socket.h on the -I path. See the note in main/CMakeLists.txt. */
#include "PPPoE.h"

#include "pppoe_session.h"
#include "net_config.h"     /* PPPOE_ID, PPPOE_PW, PPPOE_DATA_BUF_SIZE */

static const char *TAG = "pppoe";

/* Globals the vendored PPPoE.c links against, by these exact names. */
uint8_t  gDATABUF[PPPOE_DATA_BUF_SIZE];
uint8_t  pppoe_id[] = PPPOE_ID;
uint8_t  pppoe_id_len = sizeof(PPPOE_ID) - 1;   /* no NUL, as PPPoE.c expects */
uint8_t  pppoe_pw[] = PPPOE_PW;
uint8_t  pppoe_pw_len = sizeof(PPPOE_PW) - 1;
uint8_t  pppoe_ip[4];
uint16_t pppoe_retry_count = 0;

typedef struct {
    const char *name;
    bool      (*is_up)(void);
} pppoe_ctx_t;

static void pppoe_task(void *arg)
{
    pppoe_ctx_t *c = (pppoe_ctx_t *)arg;
    int32_t ret = 0;
    uint8_t str[15];

    ESP_LOGI(TAG, "[%s] waiting for chip init...", c->name);
    while (!c->is_up()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "[%s] wiznet chip PPPOE example.", c->name);

    while (1) {
        ret = ppp_start(gDATABUF);
        if (ret == PPP_SUCCESS || pppoe_retry_count > PPP_MAX_RETRY_COUNT) {
            break;      /* connected, or gave up after too many retries */
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (ret == PPP_SUCCESS) {
        ESP_LOGI(TAG, "[%s] <<<< PPPoE Success >>>>", c->name);
        ESP_LOGI(TAG, "[%s] Assigned IP address : %d.%d.%d.%d", c->name,
                 pppoe_ip[0], pppoe_ip[1], pppoe_ip[2], pppoe_ip[3]);
        ESP_LOGI(TAG, "[%s] AFTER PPPoE, Net Configuration Information", c->name);

        getSHAR(str);
        ESP_LOGI(TAG, "[%s] MAC address  : %x:%x:%x:%x:%x:%x", c->name,
                 str[0], str[1], str[2], str[3], str[4], str[5]);
        getSUBR(str);
        ESP_LOGI(TAG, "[%s] SUBNET MASK  : %d.%d.%d.%d", c->name,
                 str[0], str[1], str[2], str[3]);
        getGAR(str);
        ESP_LOGI(TAG, "[%s] G/W IP ADDRESS : %d.%d.%d.%d", c->name,
                 str[0], str[1], str[2], str[3]);
        getSIPR(str);
        ESP_LOGI(TAG, "[%s] SOURCE IP ADDRESS : %d.%d.%d.%d", c->name,
                 str[0], str[1], str[2], str[3]);
    } else {
        ESP_LOGE(TAG, "[%s] <<<< PPPoE Failed >>>>", c->name);
    }

    free(c);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void pppoe_session_start(const char *name, bool (*is_up)(void))
{
    pppoe_ctx_t *c = malloc(sizeof(*c));
    if (c == NULL) {
        ESP_LOGE(TAG, "[%s] out of memory", name);
        return;
    }
    c->name  = name;
    c->is_up = is_up;

    if (xTaskCreate(pppoe_task, name, 8192, c, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "[%s] xTaskCreate failed", name);
        free(c);
    }
}
