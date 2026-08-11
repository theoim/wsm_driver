/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Backend-neutral SNTP client. One 48-byte NTP request out, one back, and the
 * transmit timestamp is converted to a wall clock. The BSD socket calls go
 * through a vtable (sntp_ops_t) so the Ethernet (WIZnet chip) and Wi-Fi paths
 * reuse one copy - see sntp_client.h for why ioLibrary's SNTP_run() could not.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"          /* esp_ip4addr_aton (server address parse) */

#include "sntp_client.h"
#include "net_config.h"         /* SNTP_SERVER_PORT, SNTP_TIMEOUT_MS, ... */

static const char *TAG = "sntp";

/* Standard lwIP BSD socket vtable (Ethernet). With WSM_DRIVER_SOCKET_WRAP=1 these
 * lwip_* symbols are --wrap-redirected to the WIZnet chip; with =0 they are
 * software LwIP over esp_eth. Correct either way, so no #if here. */
const sntp_ops_t sntp_lwip_ops = {
    .socket = lwip_socket,   .bind = lwip_bind,
    .listen = lwip_listen,   .accept = lwip_accept,
    .connect = lwip_connect, .recv = lwip_recv,
    .send = lwip_send,       .recvfrom = lwip_recvfrom,
    .sendto = lwip_sendto,   .setsockopt = lwip_setsockopt,
    .close = lwip_close,
};

/* ---- NTP wire format (RFC 4330) ------------------------------------------
 * The packet is 48 bytes. We only fill byte 0 and only read the transmit
 * timestamp, which is what a simple unicast client needs.
 */
#define NTP_PACKET_SIZE     48
#define NTP_XMIT_TS_OFFSET  40      /* transmit timestamp: seconds since 1900 */
#define NTP_MODE_CLIENT     3
#define NTP_MODE_SERVER     4
#define NTP_VERSION         3
#define NTP_LI_ALARM        3       /* server clock not synchronized */

/* Seconds between the NTP epoch (1900-01-01) and the UNIX epoch (1970-01-01). */
#define NTP_TO_UNIX_EPOCH   2208988800UL

static uint8_t ntp_li(uint8_t b0)   { return (uint8_t)((b0 >> 6) & 0x03); }
static uint8_t ntp_mode(uint8_t b0) { return (uint8_t)(b0 & 0x07); }

/* One request/response exchange. Returns 0 and fills *out_utc on success. */
static int sntp_query(const char *tag, const sntp_ops_t *ops,
                      const char *server_ip, uint16_t local_port,
                      time_t *out_utc)
{
    int rc = -1;

    int s = ops->socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        ESP_LOGE(TAG, "[%s] socket() failed: errno %d", tag, errno);
        return -1;
    }

    /* Bind before use: on the TOE this is what actually opens the chip's UDP
     * hardware socket (wiztoe_bind), so it is not optional there. */
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(local_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (ops->bind(s, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG, "[%s] bind to port %u failed: errno %d", tag, (unsigned)local_port, errno);
        goto out;
    }

    struct timeval tv = {
        .tv_sec  = SNTP_TIMEOUT_MS / 1000,
        .tv_usec = (SNTP_TIMEOUT_MS % 1000) * 1000,
    };
    ops->setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t pkt[NTP_PACKET_SIZE];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = (uint8_t)((NTP_VERSION << 3) | NTP_MODE_CLIENT);   /* LI=0, VN=3, Mode=3 */

    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(SNTP_SERVER_PORT),
    };
    dst.sin_addr.s_addr = esp_ip4addr_aton(server_ip);

    if (ops->sendto(s, pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        ESP_LOGE(TAG, "[%s] sendto %s:%d failed: errno %d",
                 tag, server_ip, SNTP_SERVER_PORT, errno);
        goto out;
    }

    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    int n = ops->recvfrom(s, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &fl);
    if (n < 0) {
        ESP_LOGW(TAG, "[%s] no reply within %d ms (errno %d)", tag, SNTP_TIMEOUT_MS, errno);
        goto out;
    }
    if (n < NTP_PACKET_SIZE) {
        ESP_LOGW(TAG, "[%s] short reply: %d bytes", tag, n);
        goto out;
    }
    if (ntp_mode(pkt[0]) != NTP_MODE_SERVER) {
        ESP_LOGW(TAG, "[%s] not a server reply (mode %u)", tag, (unsigned)ntp_mode(pkt[0]));
        goto out;
    }
    if (ntp_li(pkt[0]) == NTP_LI_ALARM) {
        ESP_LOGW(TAG, "[%s] server clock is not synchronized", tag);
        goto out;
    }

    uint32_t secs = ((uint32_t)pkt[NTP_XMIT_TS_OFFSET + 0] << 24) |
                    ((uint32_t)pkt[NTP_XMIT_TS_OFFSET + 1] << 16) |
                    ((uint32_t)pkt[NTP_XMIT_TS_OFFSET + 2] << 8)  |
                    ((uint32_t)pkt[NTP_XMIT_TS_OFFSET + 3]);
    if (secs < NTP_TO_UNIX_EPOCH) {          /* 0 = unset; pre-1970 = nonsense */
        ESP_LOGW(TAG, "[%s] bogus transmit timestamp", tag);
        goto out;
    }

    *out_utc = (time_t)(secs - NTP_TO_UNIX_EPOCH);
    rc = 0;

out:
    ops->close(s);
    return rc;
}

/* --------------------------------------------------------------------------
 * Task launcher: Ethernet and Wi-Fi are both started this way (same level).
 * ------------------------------------------------------------------------ */
typedef struct {
    const char       *name;
    const sntp_ops_t *ops;
    const char       *server_ip;
    uint16_t          local_port;
    bool            (*is_up)(void);
} sntp_ctx_t;

static void sntp_task(void *arg)
{
    sntp_ctx_t *c = (sntp_ctx_t *)arg;
    time_t utc = 0;
    bool ok = false;

    /* Wait for the link first. The request is a single datagram with no
     * retransmit inside the exchange, so sending it before the PHY is up just
     * burns one attempt. */
    ESP_LOGI(TAG, "[%s] waiting for link...", c->name);
    while (!c->is_up()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    for (int attempt = 1; attempt <= SNTP_RETRY_COUNT; attempt++) {
        ESP_LOGI(TAG, "[%s] querying %s (attempt %d/%d)",
                 c->name, c->server_ip, attempt, SNTP_RETRY_COUNT);
        if (sntp_query(c->name, c->ops, c->server_ip, c->local_port, &utc) == 0) {
            ok = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (ok) {
        time_t local = utc + (time_t)SNTP_TZ_OFFSET_MIN * 60;
        struct tm tm;
        gmtime_r(&local, &tm);      /* gmtime on an already-offset value */
        ESP_LOGI(TAG, "[%s] %04d-%02d-%02d %02d:%02d:%02d (UTC%+d:%02d)",
                 c->name, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec,
                 SNTP_TZ_OFFSET_MIN / 60, abs(SNTP_TZ_OFFSET_MIN % 60));
    } else {
        ESP_LOGE(TAG, "[%s] SNTP failed after %d attempts", c->name, SNTP_RETRY_COUNT);
    }

    free(c);
    while (1) {                     /* nothing to do, same as the original */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void sntp_client_start(const char *name, const sntp_ops_t *ops,
                       const char *server_ip, uint16_t local_port,
                       bool (*is_up)(void))
{
    sntp_ctx_t *c = malloc(sizeof(*c));
    if (c == NULL) {
        ESP_LOGE(TAG, "[%s] out of memory", name);
        return;
    }
    c->name       = name;
    c->ops        = ops;
    c->server_ip  = server_ip;
    c->local_port = local_port;
    c->is_up      = is_up;

    if (xTaskCreate(sntp_task, name, 4096, c, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "[%s] xTaskCreate failed", name);
        free(c);
    }
}
