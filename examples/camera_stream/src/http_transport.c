/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * BSD implementation of the camera server's network seam (see the header).
 *
 * The only file in the example that includes lwIP.
 */
#include <errno.h>
#include <string.h>

#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "net_sock_ops.h"
#include "http_transport.h"

static const char *TAG = "http_tx";

static void set_timeout(const net_sock_ops_t *ops, int fd, uint32_t ms)
{
    struct timeval tv = {
        .tv_sec  = ms / 1000,
        .tv_usec = (ms % 1000) * 1000,
    };
    ops->setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static int listen_one(const net_sock_ops_t *ops, uint16_t port)
{
    int fd = ops->socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }

    int opt = 1;
    ops->setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (ops->bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        ops->listen(fd, 1) < 0) {
        ops->close(fd);
        return -1;
    }
    return fd;
}

int http_listen_pool(const void *vops, uint16_t port, int count, int *fds)
{
    const net_sock_ops_t *ops = (const net_sock_ops_t *)vops;
    if (ops == NULL || count < 1) {
        return 0;
    }
    if (count > HTTP_MAX_LISTENERS) {
        count = HTTP_MAX_LISTENERS;
    }

    int opened = 0;
    for (int i = 0; i < count; i++) {
        int fd = listen_one(ops, port);
        if (fd < 0) {
            /* Running out of hardware sockets is the ordinary reason, and it is
             * not fatal: fewer listeners means fewer simultaneous clients, not
             * a broken server. Say so rather than failing the whole start. */
            ESP_LOGW(TAG, "listener %d of %d on port %u failed: errno %d",
                     i + 1, count, port, errno);
            break;
        }
        fds[opened++] = fd;
    }
    return opened;
}

int http_accept(const void *vops, int listen_fd, uint32_t timeout_ms)
{
    const net_sock_ops_t *ops = (const net_sock_ops_t *)vops;
    set_timeout(ops, listen_fd, timeout_ms);

    struct sockaddr_in peer;
    socklen_t sl = sizeof(peer);
    int fd = ops->accept(listen_fd, (struct sockaddr *)&peer, &sl);
    if (fd < 0) {
        /* Only a receive timeout means "nobody connected". Anything else is a
         * broken listening socket, and reporting it as a timeout would leave
         * the caller polling a dead socket forever. The TOE wrapper is explicit
         * about the difference -- EWOULDBLOCK on timeout, EINVAL on failure. */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        ESP_LOGE(TAG, "accept failed: errno %d", errno);
        return -1;
    }
    return fd;
}

int http_recv(const void *vops, int fd, void *buf, size_t size,
              uint32_t timeout_ms)
{
    const net_sock_ops_t *ops = (const net_sock_ops_t *)vops;
    set_timeout(ops, fd, timeout_ms);

    int n = ops->recv(fd, buf, size, 0);
    if (n > 0) {
        return n;
    }
    if (n == 0) {
        return -1;                      /* peer closed */
    }
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
}

int http_set_send_timeout(const void *vops, int fd, uint32_t timeout_ms)
{
    const net_sock_ops_t *ops = (const net_sock_ops_t *)vops;
    struct timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    return ops->setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

int http_send(const void *vops, int fd, const void *buf, size_t len)
{
    const net_sock_ops_t *ops = (const net_sock_ops_t *)vops;
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;

    while (sent < len) {
        int n = ops->send(fd, p + sent, len - sent, 0);
        if (n <= 0) {
            /* Warning rather than error: a browser that navigates away mid
             * frame does exactly this, and on a stream that is routine. */
            ESP_LOGW(TAG, "send stopped after %u of %u bytes: errno %d",
                     (unsigned)sent, (unsigned)len, errno);
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

void http_close(const void *vops, int fd)
{
    const net_sock_ops_t *ops = (const net_sock_ops_t *)vops;
    if (ops != NULL && fd >= 0) {
        ops->close(fd);
    }
}
