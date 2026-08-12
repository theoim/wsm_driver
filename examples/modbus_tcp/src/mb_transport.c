/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * BSD implementation of the Modbus TCP transport seam (see mb_transport.h).
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
#include "mb_transport.h"

static const char *TAG = "mb_tx";

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

int mb_transport_listen(const void *vops, uint16_t port)
{
    const net_sock_ops_t *ops = (const net_sock_ops_t *)vops;
    if (ops == NULL) {
        ESP_LOGE(TAG, "listen(%u) with no socket vtable", port);
        return -1;
    }

    int fd = ops->socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return -1;
    }

    int opt = 1;
    ops->setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (ops->bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind(%u) failed: errno %d", port, errno);
        ops->close(fd);
        return -1;
    }
    if (ops->listen(fd, 1) < 0) {
        ESP_LOGE(TAG, "listen(%u) failed: errno %d", port, errno);
        ops->close(fd);
        return -1;
    }
    return fd;
}

int mb_transport_accept(const void *vops, int listen_fd, uint32_t timeout_ms)
{
    const net_sock_ops_t *ops = (const net_sock_ops_t *)vops;
    set_timeout(ops, listen_fd, timeout_ms);

    struct sockaddr_in peer;
    socklen_t sl = sizeof(peer);
    int fd = ops->accept(listen_fd, (struct sockaddr *)&peer, &sl);
    if (fd < 0) {
        return 0;                       /* nobody connected in time */
    }
    ESP_LOGI(TAG, "master connected from %s", inet_ntoa(peer.sin_addr));
    return fd;
}

int mb_transport_recv_exact(const void *vops, int fd, void *buf, size_t size,
                            uint32_t timeout_ms)
{
    const net_sock_ops_t *ops = (const net_sock_ops_t *)vops;
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;

    set_timeout(ops, fd, timeout_ms);

    while (got < size) {
        int n = ops->recv(fd, p + got, size - got, 0);
        if (n > 0) {
            got += (size_t)n;
            continue;
        }
        if (n == 0) {
            return -1;                  /* peer closed */
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* Nothing yet is only benign before the first byte. Once part of an
             * ADU has arrived the rest is already in flight, so a timeout here
             * means the frame will never complete and the stream is no longer
             * on a frame boundary -- the connection has to go. */
            return (got == 0) ? 0 : -1;
        }
        return -1;
    }
    return (int)size;
}

int mb_transport_send(const void *vops, int fd, const void *buf, size_t len)
{
    const net_sock_ops_t *ops = (const net_sock_ops_t *)vops;
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;

    while (sent < len) {
        int n = ops->send(fd, p + sent, len - sent, 0);
        if (n <= 0) {
            ESP_LOGW(TAG, "send stopped after %u of %u bytes: errno %d",
                     (unsigned)sent, (unsigned)len, errno);
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

void mb_transport_close(const void *vops, int fd)
{
    const net_sock_ops_t *ops = (const net_sock_ops_t *)vops;
    if (ops != NULL && fd >= 0) {
        ops->close(fd);
    }
}
