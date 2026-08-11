/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Backend-neutral TLS client. Same demo as the original WIZnet-PICO-C example
 * (connect, handshake with verification disabled, send a hello line, print what
 * comes back), but the transport goes through a vtable (ssl_ops_t) so the
 * Ethernet (WIZnet chip) and Wi-Fi paths reuse one copy.
 *
 * Two differences from the original, both forced by running on two interfaces
 * at once:
 *   - The mbedTLS context and config are per task, not file-scope globals. Two
 *     concurrent sessions cannot share one mbedtls_ssl_context.
 *   - The BIO is wired to ops->send/recv on a BSD fd instead of ioLibrary
 *     send()/recv() on a hardware socket number, and the "is there data?"
 *     getsockopt(SO_RECVBUF) poll is gone: the BIO blocks in recv() with
 *     SO_RCVTIMEO instead, which is what mbedtls_ssl_conf_read_timeout expects.
 *
 * mbedTLS is provided by ESP-IDF. Certificate verification is disabled
 * (VERIFY_NONE) to keep the demo dependency-free, same as the original.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"          /* esp_ip4addr_aton (target address parse) */

#include "mbedtls/ssl.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"   /* MBEDTLS_ERR_NET_* */

#include "ssl_client.h"
#include "net_config.h"         /* SSL_RECV_TIMEOUT_MS, SSL_BUF_SIZE, ... */

static const char *TAG = "ssl_client";

/* Standard lwIP BSD socket vtable (Ethernet). With WSM_DRIVER_SOCKET_WRAP=1 these
 * lwip_* symbols are --wrap-redirected to the WIZnet chip; with =0 they are
 * software LwIP over esp_eth. Correct either way, so no #if here. */
const ssl_ops_t ssl_lwip_ops = {
    .socket = lwip_socket,   .bind = lwip_bind,
    .listen = lwip_listen,   .accept = lwip_accept,
    .connect = lwip_connect, .recv = lwip_recv,
    .send = lwip_send,       .recvfrom = lwip_recvfrom,
    .sendto = lwip_sendto,   .setsockopt = lwip_setsockopt,
    .close = lwip_close,
};

/* --------------------------------------------------------------------------
 * mbedTLS BIO -> socket vtable glue. ctx carries both the vtable and the fd,
 * because the fd alone does not say which stack it belongs to.
 * ------------------------------------------------------------------------ */
typedef struct {
    const ssl_ops_t *ops;
    int              fd;
} bio_ctx_t;

static bool errno_is_timeout(void)
{
    return (errno == EWOULDBLOCK) || (errno == EAGAIN);
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    bio_ctx_t *b = (bio_ctx_t *)ctx;
    int n = b->ops->send(b->fd, buf, len, 0);
    if (n < 0) {
        return errno_is_timeout() ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    bio_ctx_t *b = (bio_ctx_t *)ctx;
    int n = b->ops->recv(b->fd, buf, len, 0);
    if (n < 0) {
        return errno_is_timeout() ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return n;                    /* 0 = peer closed, which mbedTLS reads as EOF */
}

/* mbedtls_ssl_conf_read_timeout() drives this one. The TOE has no select(), so
 * the timeout is applied with SO_RCVTIMEO and the recv blocks for that long;
 * the wrap maps its own timeout to EWOULDBLOCK (see wiztoe_wrap.c). */
static int bio_recv_timeout(void *ctx, unsigned char *buf, size_t len, uint32_t timeout_ms)
{
    bio_ctx_t *b = (bio_ctx_t *)ctx;

    struct timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    b->ops->setsockopt(b->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int n = b->ops->recv(b->fd, buf, len, 0);
    if (n < 0) {
        return errno_is_timeout() ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return n;
}

/* --------------------------------------------------------------------------
 * One session: TCP connect -> handshake -> hello -> echo/print until it drops
 * ------------------------------------------------------------------------ */
typedef struct {
    const char      *name;
    const ssl_ops_t *ops;
    const char      *target_ip;
    uint16_t         target_port;
    bool           (*is_up)(void);
} ssl_ctx_t;

static void ssl_session(const ssl_ctx_t *c, uint8_t *buf)
{
    const ssl_ops_t *ops = c->ops;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  conf;
    bio_ctx_t bio;
    int ret;

    int s = ops->socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        ESP_LOGE(TAG, "[%s] socket() failed: errno %d", c->name, errno);
        return;
    }

    int one = 1;
    ops->setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(c->target_port),
    };
    dst.sin_addr.s_addr = esp_ip4addr_aton(c->target_ip);

    /* The original example opened its W6300 socket with SF_FORCE_ARP and gave up
     * on the first failed connect (vTaskDelete). That flag is not reachable from
     * the BSD API, and it turns out not to be needed: the tcp_client example
     * connects on the same chip with open flags 0 and simply retries from its
     * SOCK_CLOSED state. This client retries the same way (see ssl_task), so a
     * lost first SYN costs one SSL_RETRY_DELAY_MS, not the session. */
    ESP_LOGI(TAG, "[%s] connecting to %s:%u", c->name, c->target_ip, (unsigned)c->target_port);
    if (ops->connect(s, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        ESP_LOGW(TAG, "[%s] connect failed: errno %d", c->name, errno);
        ops->close(s);
        return;
    }
    ESP_LOGI(TAG, "[%s] TCP connected, starting TLS handshake", c->name);

    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);

    if ((ret = mbedtls_ssl_config_defaults(&conf,
                                           MBEDTLS_SSL_IS_CLIENT,
                                           MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        ESP_LOGE(TAG, "[%s] mbedtls_ssl_config_defaults returned -0x%x", c->name, (unsigned)-ret);
        goto cleanup;
    }

    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_read_timeout(&conf, SSL_RECV_TIMEOUT_MS);

    if ((ret = mbedtls_ssl_setup(&ssl, &conf)) != 0) {
        ESP_LOGE(TAG, "[%s] mbedtls_ssl_setup returned -0x%x", c->name, (unsigned)-ret);
        goto cleanup;
    }

    bio.ops = ops;
    bio.fd  = s;
    mbedtls_ssl_set_bio(&ssl, &bio, bio_send, bio_recv, bio_recv_timeout);

    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            ESP_LOGE(TAG, "[%s] mbedtls_ssl_handshake returned -0x%x", c->name, (unsigned)-ret);
            goto cleanup;
        }
    }
    ESP_LOGI(TAG, "[%s] TLS ok [ Ciphersuite: %s ]", c->name, mbedtls_ssl_get_ciphersuite(&ssl));

    /* Say hello, then print whatever the server sends back. */
    const char *hello = SSL_HELLO_MSG;
    size_t hello_len = strlen(hello);
    size_t off = 0;
    while (off < hello_len) {
        ret = mbedtls_ssl_write(&ssl, (const unsigned char *)hello + off, hello_len - off);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (ret <= 0) {
            ESP_LOGE(TAG, "[%s] mbedtls_ssl_write returned -0x%x", c->name, (unsigned)-ret);
            goto close_notify;
        }
        off += (size_t)ret;
    }

    while (1) {
        ret = mbedtls_ssl_read(&ssl, buf, SSL_BUF_SIZE - 1);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
            ret == MBEDTLS_ERR_SSL_TIMEOUT) {
            continue;                       /* idle link, keep waiting */
        }
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            ESP_LOGI(TAG, "[%s] server closed the session", c->name);
            break;
        }
        if (ret <= 0) {
            ESP_LOGW(TAG, "[%s] mbedtls_ssl_read returned -0x%x", c->name, (unsigned)-ret);
            break;
        }
        buf[ret] = 0x00;
        ESP_LOGI(TAG, "[%s] %s", c->name, (char *)buf);
    }

close_notify:
    mbedtls_ssl_close_notify(&ssl);

cleanup:
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    ops->close(s);
}

static void ssl_task(void *arg)
{
    ssl_ctx_t *c = (ssl_ctx_t *)arg;

    uint8_t *buf = malloc(SSL_BUF_SIZE);
    if (buf == NULL) {
        ESP_LOGE(TAG, "[%s] out of memory for %d-byte buffer", c->name, SSL_BUF_SIZE);
        free(c);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "[%s] waiting for link...", c->name);
    while (!c->is_up()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    while (1) {
        ssl_session(c, buf);
        vTaskDelay(pdMS_TO_TICKS(SSL_RETRY_DELAY_MS));
    }
}

void ssl_client_start(const char *name, const ssl_ops_t *ops,
                      const char *target_ip, uint16_t target_port,
                      bool (*is_up)(void))
{
    ssl_ctx_t *c = malloc(sizeof(*c));
    if (c == NULL) {
        ESP_LOGE(TAG, "[%s] out of memory", name);
        return;
    }
    c->name        = name;
    c->ops         = ops;
    c->target_ip   = target_ip;
    c->target_port = target_port;
    c->is_up       = is_up;

    /* TLS handshakes are stack-hungry; the original example used 16 kB for its
     * single session and each interface now gets its own task. */
    if (xTaskCreate(ssl_task, name, 16384, c, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "[%s] xTaskCreate failed", name);
        free(c);
    }
}
