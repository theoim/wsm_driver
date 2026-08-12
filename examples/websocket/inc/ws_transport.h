/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Network seam for the WebSocket server.
 *
 * ws_core.c speaks RFC 6455 and knows nothing about sockets, lwIP, or the
 * WIZnet chip; only ws_transport.c includes lwIP. Same arrangement as the tftp
 * and upnp examples, and for the same reason: the protocol code stays testable
 * and backend-neutral, and the one file that has to know about the backend is
 * small enough to read in a sitting.
 *
 * `ops` is a const void * rather than a net_sock_ops_t *: that type is an
 * anonymous typedef with no struct tag, so it cannot be forward declared, and
 * naming it here would drag the component's headers -- and lwIP behind them --
 * into ws_core.c.
 *
 * On accept() the two backends disagree in a way worth knowing about. LwIP
 * returns a NEW descriptor and leaves the listener open. The WIZnet TOE has no
 * separate accepted socket: the listening hardware socket BECOMES the
 * connection, so accept() hands back the same descriptor it was given. The
 * component absorbs the difference on the way out -- closing an accepted
 * listener reopens and re-listens it rather than tearing it down (see
 * wiztoe_close) -- so the ordinary accept / serve / close loop works unchanged
 * on both. What does NOT carry over is serving several clients from one
 * listening socket: on the TOE that needs one hardware socket per client, which
 * is what examples/tcp_server_multi_socket demonstrates. This example serves one
 * connection at a time.
 */
#ifndef WS_TRANSPORT_H
#define WS_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

/* Select the socket vtable every later call uses. Pass &net_eth_ops for the
 * WIZnet hardware sockets or &net_wifi_ops for the software LwIP stack. */
void ws_transport_bind(const void *ops);

/* Open a listening socket on `port`. Returns the fd, or -1. */
int ws_transport_listen(uint16_t port);

/*
 * Wait for a client. Returns a connected fd, 0 if nobody arrived before the
 * timeout, or -1 on error. The returned fd may or may not equal listen_fd --
 * see the note above; callers must not assume either way.
 */
int ws_transport_accept(int listen_fd, uint32_t timeout_ms);

/*
 * Read up to `size` bytes. Returns the count, 0 on timeout, or -1 if the peer
 * closed or the socket failed.
 */
int ws_transport_recv(int fd, void *buf, size_t size, uint32_t timeout_ms);

/* Write all of `len`. Returns 0 on success, -1 on failure. */
int ws_transport_send(int fd, const void *buf, size_t len);

void ws_transport_close(int fd);

#endif /* WS_TRANSPORT_H */
