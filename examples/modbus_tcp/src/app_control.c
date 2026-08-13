/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Deferred reconfiguration (see app_control.h).
 *
 * This is the only file in the example that talks to the WIZnet chip directly.
 * It does so through ioLibrary's wizchip_setnetinfo(), which the component
 * exposes publicly, rather than through a new wsm_driver API: the component is
 * shared with every other example and with the team's own code, and a runtime
 * network-apply entry point is not something to add to it on the strength of
 * one example. If this turns out to be generally useful it can move down later;
 * moving it back up would be harder.
 *
 * What it deliberately does NOT do is call wiznet_net_init() again. That would
 * re-run the SPI bring-up and the netif setup underneath live sockets. Changing
 * the address is a register write on the chip -- the sockets are what have to be
 * rebuilt around it, and this file rebuilds exactly those.
 */
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "net_backend.h"        /* wiznet_net_is_up */
#include "wizchip_conf.h"       /* wiz_NetInfo, wizchip_setnetinfo */

#include "app_control.h"
#include "net_config.h"

static const char *TAG = "control";

/* Long enough for a master mid-request to finish or time out, short enough that
 * a wedged session does not hold a port change forever. */
#define STOP_TIMEOUT_MS   3000

/* The response to the request that caused this has to reach the browser first.
 * It is already written to the socket by the time the message is posted, but
 * the socket has not necessarily drained, and on the TOE the close is what
 * flushes it. */
#define SETTLE_MS         400

typedef struct {
    const void   *ops;
    mb_store_t   *store;
    mb_server_t  *server;
    web_server_t *web;
    app_config_t  current;
    QueueHandle_t queue;
    volatile bool pending;
} control_t;

static control_t s_ctl;

static bool same_ip(const uint8_t a[4], const uint8_t b[4])
{
    return memcmp(a, b, 4) == 0;
}

static void apply_network(const app_config_t *cfg)
{
    wiz_NetInfo ni;
    memset(&ni, 0, sizeof(ni));

    /* Read the current identity back rather than rebuilding it: the MAC and DNS
     * are not ours to change, and taking them from the chip means this stays
     * correct if something else ever sets them. */
    wizchip_getnetinfo(&ni);

    memcpy(ni.ip, cfg->ip, 4);
    memcpy(ni.sn, cfg->mask, 4);
    memcpy(ni.gw, cfg->gateway, 4);
    ni.dhcp = NETINFO_STATIC;

    wizchip_setnetinfo(&ni);

    ESP_LOGI(TAG, "address now %u.%u.%u.%u mask %u.%u.%u.%u gw %u.%u.%u.%u",
             cfg->ip[0], cfg->ip[1], cfg->ip[2], cfg->ip[3],
             cfg->mask[0], cfg->mask[1], cfg->mask[2], cfg->mask[3],
             cfg->gateway[0], cfg->gateway[1], cfg->gateway[2], cfg->gateway[3]);
}

/*
 * Apply one configuration change.
 *
 * Order matters. The Modbus server comes down first, because its listening
 * socket is bound to the old address and a socket outliving the address it was
 * opened on is the one state the chip handles badly. Then the address changes,
 * then the server comes back on whichever port is now configured.
 */
static void apply(control_t *ctl, const app_config_t *cfg)
{
    bool ip_changed   = !same_ip(ctl->current.ip, cfg->ip) ||
                        !same_ip(ctl->current.mask, cfg->mask) ||
                        !same_ip(ctl->current.gateway, cfg->gateway);
    bool port_changed = ctl->current.modbus_port != cfg->modbus_port;

    if (!ip_changed && !port_changed) {
        ESP_LOGI(TAG, "nothing to apply");
        ctl->pending = false;
        return;
    }

    /* Let the HTTP response get out before the sockets underneath it move. */
    vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));

    /*
     * Order: web down, Modbus down, address changed, both back up.
     *
     * The web server only has to come down for an address change, but it has to
     * come down FIRST when it does -- its listening sockets were opened before
     * the address moved, and leaving them open across the change is the one
     * arrangement the chip is not asked to handle anywhere else in these
     * examples. The port-only case leaves the page serving throughout, which is
     * also what a user watching it expects.
     */
    if (ip_changed) {
        if (!web_server_stop(ctl->web, STOP_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "web server would not stop; configuration not applied");
            return;
        }
        ctl->web = NULL;
    }

    if (!mb_server_stop(ctl->server, STOP_TIMEOUT_MS)) {
        /*
         * The handle is leaked deliberately by mb_server_stop in this case, so
         * the old task keeps its own memory. Starting a second server on top
         * would leave two tasks fighting over one hardware socket.
         *
         * A master that stopped reading mid-response is the way this happens:
         * the send has no deadline on the TOE backend, so the task is stuck
         * inside it. NVS already holds the new settings, so the change lands at
         * the next reboot -- app_control_pending() stays true until then and
         * the dashboard says so rather than letting the two quietly disagree.
         */
        ESP_LOGE(TAG, "Modbus server would not stop; configuration saved but "
                 "not applied -- it will take effect at the next reboot");
        if (ip_changed && ctl->web == NULL) {
            ctl->web = web_server_start(ctl->ops, WEB_PORT, ctl->store,
                                        wiznet_net_is_up);
        }
        return;
    }
    ctl->server = NULL;

    if (ip_changed) {
        apply_network(cfg);
    }

    ctl->server = mb_server_start("eth", ctl->ops, cfg->modbus_port,
                                  ctl->store, wiznet_net_is_up);
    if (ip_changed) {
        ctl->web = web_server_start(ctl->ops, WEB_PORT, ctl->store,
                                    wiznet_net_is_up);
    }

    if (ctl->server == NULL) {
        ESP_LOGE(TAG, "Modbus server did not restart on port %u",
                 cfg->modbus_port);
        return;
    }

    ctl->current = *cfg;
    ctl->pending = false;
    ESP_LOGI(TAG, "configuration applied; Modbus on port %u",
             cfg->modbus_port);
}

static void control_task(void *arg)
{
    control_t *ctl = (control_t *)arg;

    for (;;) {
        app_config_t cfg;
        if (xQueueReceive(ctl->queue, &cfg, portMAX_DELAY) == pdTRUE) {
            apply(ctl, &cfg);
        }
    }
}

bool app_control_start(const void *ops, mb_store_t *store,
                       mb_server_t *server, web_server_t *web,
                       const app_config_t *current)
{
    s_ctl.ops = ops;
    s_ctl.store = store;
    s_ctl.server = server;
    s_ctl.web = web;
    s_ctl.current = *current;

    /* Depth one: a second change arriving before the first has been applied is
     * a user clicking twice, and the newer settings are the ones they meant.
     * Anything deeper would queue up a series of reconfigurations to walk
     * through, each one bouncing the link. */
    s_ctl.queue = xQueueCreate(1, sizeof(app_config_t));
    if (s_ctl.queue == NULL) {
        ESP_LOGE(TAG, "no memory for the control queue");
        return false;
    }

    if (xTaskCreate(control_task, "mb_control", 4096, &s_ctl, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return false;
    }
    return true;
}

bool app_control_apply(const app_config_t *cfg)
{
    if (s_ctl.queue == NULL) {
        return false;
    }
    s_ctl.pending = true;
    /* Overwrite rather than block: the HTTP task must not wait here. */
    return xQueueOverwrite(s_ctl.queue, cfg) == pdTRUE;
}

void app_control_current(app_config_t *out)
{
    *out = s_ctl.current;
}

bool app_control_pending(void)
{
    return s_ctl.pending;
}
