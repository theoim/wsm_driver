/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * BSD implementation of the WebSocket transport seam (see ws_transport.h).
 *
 * The only file in the example that includes lwIP. Sockets are reached through
 * the component's net_sock_ops_t vtable, so the server runs on the WIZnet
 * hardware sockets or on the software LwIP the Wi-Fi netif is attached to,
 * chosen by which vtable main.c hands over.
 */
#include <errno.h>
#include <string.h>

#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "net_sock_ops.h"
#include "ws_transport.h"

static const char *TAG = "ws_tx";

/* No module-level vtable here on purpose -- see the header. Two servers run
 * concurrently on two stacks, so the vtable belongs to the call, not the file. */

static void set_timeout(const net_sock_ops_t *ops, int fd, uint32_t ms)
{
    struct timeval tv = {
        .tv_sec  = ms / 1000,
        .tv_usec = (ms % 1000) * 1000,
    };
    ops->setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

int ws_transport_listen(const void *vops, uint16_t port)
{
    const net_sock_ops_t *s_ops = (const net_sock_ops_t *)vops;
    if (s_ops == NULL) {
        ESP_LOGE(TAG, "listen(%u) with no socket vtable", port);
        return -1;
    }

    int fd = s_ops->socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return -1;
    }

    int opt = 1;
    s_ops->setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (s_ops->bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind(%u) failed: errno %d", port, errno);
        s_ops->close(fd);
        return -1;
    }
    if (s_ops->listen(fd, 1) < 0) {
        ESP_LOGE(TAG, "listen(%u) failed: errno %d", port, errno);
        s_ops->close(fd);
        return -1;
    }
    return fd;
}

int ws_transport_accept(const void *vops, int listen_fd, uint32_t timeout_ms)
{
    const net_sock_ops_t *s_ops = (const net_sock_ops_t *)vops;
    set_timeout(s_ops, listen_fd, timeout_ms);

    struct sockaddr_in peer;
    socklen_t sl = sizeof(peer);
    int fd = s_ops->accept(listen_fd, (struct sockaddr *)&peer, &sl);
    if (fd < 0) {
        /* Only a receive timeout means "nobody connected". Anything else is a
         * broken listening socket, and reporting it as a timeout would leave
         * the caller polling a dead socket forever. The TOE wrapper is explicit
         * about the difference -- EWOULDBLOCK on timeout, EINVAL on failure
         * (wiztoe_wrap.c) -- so there is a real distinction to preserve. */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        ESP_LOGE(TAG, "accept failed: errno %d", errno);
        return -1;
    }
    ESP_LOGI(TAG, "client connected from %s", inet_ntoa(peer.sin_addr));

    /* 0 is the caller's "nobody yet" value, so an accepted fd must never be 0.
     * It cannot be: lwIP descriptors start at LWIP_SOCKET_OFFSET, and the TOE
     * wrapper adds that same offset to its 0..7 hardware socket numbers. */
    return fd;
}

int ws_transport_recv(const void *vops, int fd, void *buf, size_t size,
                      uint32_t timeout_ms)
{
    const net_sock_ops_t *s_ops = (const net_sock_ops_t *)vops;
    set_timeout(s_ops, fd, timeout_ms);

    int n = s_ops->recv(fd, buf, size, 0);
    if (n > 0) {
        return n;
    }
    if (n == 0) {
        return -1;                      /* peer closed */
    }
    /* A receive timeout is not a failure: the caller polls. Anything else is. */
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
}

int ws_transport_send(const void *vops, int fd, const void *buf, size_t len)
{
    const net_sock_ops_t *s_ops = (const net_sock_ops_t *)vops;
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;

    while (sent < len) {
        int n = s_ops->send(fd, p + sent, len - sent, 0);
        if (n <= 0) {
            /* Warning rather than error: the ordinary cause is a client that
             * went away mid-response -- a browser reloading before the page
             * finished arriving does exactly this -- and that is the peer's
             * business, not a fault here. The caller decides whether losing
             * this particular write matters. */
            ESP_LOGW(TAG, "send stopped after %u of %u bytes: errno %d",
                     (unsigned)sent, (unsigned)len, errno);
            return -1;
        }
        sent += n;
    }
    return 0;
}

void ws_transport_close(const void *vops, int fd)
{
    const net_sock_ops_t *s_ops = (const net_sock_ops_t *)vops;
    if (s_ops != NULL && fd >= 0) {
        s_ops->close(fd);
    }
}
