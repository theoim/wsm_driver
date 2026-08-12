/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Network seam for the camera streaming server.
 *
 * Same arrangement as the tftp, websocket and modbus_tcp examples: only this
 * file's implementation includes lwIP, and it reaches sockets through the
 * component's net_sock_ops_t vtable, so the server runs on the WIZnet hardware
 * sockets or on the software LwIP behind the Wi-Fi netif.
 *
 * `ops` is a const void * rather than a net_sock_ops_t *: that type is an
 * anonymous typedef with no struct tag, so it cannot be forward declared, and
 * naming it here would drag the component's headers -- and lwIP behind them --
 * into the rest of the example.
 *
 * Every call takes its `ops` explicitly, because two servers run at once and
 * do not start together: the Wi-Fi task blocks until DHCP finishes, which can
 * be minutes after the Ethernet server is already serving. A module-level
 * "current vtable" would let whichever task selected last own the calls of
 * both, handing one stack's socket numbers to the other's descriptor space.
 *
 * ---- Why this example listens more than once -----------------------------
 *
 * The other examples serve one connection at a time. This one cannot: the page
 * holds /stream open indefinitely while polling /api/status once a second, so
 * two connections have to be live at the same instant or the charts never
 * update.
 *
 * On LwIP that is free -- accept() returns a NEW descriptor and the listener
 * stays open, so one listening socket feeds any number of connections. On the
 * TOE it is not: the listening hardware socket BECOMES the connection, so a
 * single listener can hold exactly one client and there is nothing left
 * listening while it does. Serving N clients there means N listening hardware
 * sockets on the same port, which is what examples/tcp_server_multi_socket
 * demonstrates and what http_listen_pool() sets up here.
 *
 * The count therefore follows the backend rather than being a tuning knob --
 * see CAM_ETH_LISTENERS in cam_server.c.
 */
#ifndef HTTP_TRANSPORT_H
#define HTTP_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

/* Most listening sockets one interface may hold. The WIZnet chips have eight
 * hardware sockets in total and the example never needs the rest. */
#define HTTP_MAX_LISTENERS 4

/* Open `count` listening sockets on `port`, all with SO_REUSEADDR. Writes the
 * descriptors to fds[] and returns how many came up; 0 means none did. */
int http_listen_pool(const void *ops, uint16_t port, int count, int *fds);

/*
 * Wait for a client on one listener. Returns a connected fd, 0 if nobody
 * arrived before the timeout, or -1 if the listening socket itself failed.
 *
 * The returned fd may or may not equal listen_fd -- on the TOE it always does,
 * on LwIP it never does -- so a caller must not assume either way. What it can
 * assume is that 0 is never a valid descriptor: LwIP numbers its own from
 * LWIP_SOCKET_OFFSET and the TOE wrapper adds that same offset to its 0..7
 * hardware socket numbers.
 */
int http_accept(const void *ops, int listen_fd, uint32_t timeout_ms);

/*
 * Read up to `size` bytes. Returns the count, 0 on timeout, or -1 if the peer
 * closed or the socket failed.
 */
int http_recv(const void *ops, int fd, void *buf, size_t size,
              uint32_t timeout_ms);

/* Write all of `len`. Returns 0 on success, -1 on failure. */
int http_send(const void *ops, int fd, const void *buf, size_t len);

void http_close(const void *ops, int fd);

#endif /* HTTP_TRANSPORT_H */
