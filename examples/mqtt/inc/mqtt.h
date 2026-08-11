/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Backend-neutral MQTT 3.1.1 client. The socket entry points are injected via
 * the component's net_sock_ops_t vtable, so the same client drives either stack:
 *   - Ethernet: net_eth_ops (plain lwip_*). With WSM_DRIVER_SOCKET_WRAP=1 these
 *     are redirected to the WIZnet hardware sockets by -Wl,--wrap (so every call
 *     lands in __wrap_lwip_*); with =0 they are the software LwIP over esp_eth
 *     (so every call lands in lwip_*). Correct either way, no #if needed.
 *   - Wi-Fi: net_wifi_ops, which bypasses that wrap to reach the real LwIP.
 *
 * Nothing here calls the ioLibrary MQTT client (Internet/MQTT: MQTTClient.c and
 * mqtt_interface.c). Those talk to the chip's socket registers directly through
 * ioLibrary's own socket()/send()/recv(), so --wrap has nothing to intercept and
 * one source could not serve both backends. src/mqtt.c speaks the MQTT 3.1.1
 * wire format over ops->recv() / ops->send() instead.
 */
#ifndef MQTT_H
#define MQTT_H

#include <stdbool.h>
#include <stdint.h>

#include "net_sock_ops.h"   /* net_sock_ops_t, net_eth_ops, net_wifi_ops */

/*
 * One client's session parameters. The struct is copied into the task, but the
 * strings are NOT: they must stay valid for the lifetime of the client (string
 * literals from net_config.h in this example).
 */
typedef struct {
    const char *broker_ip;      /* dotted-quad, e.g. "192.168.11.100"          */
    uint16_t    broker_port;    /* 1883 for plain MQTT                          */

    const char *client_id;      /* must be unique per connection to a broker    */
    const char *username;       /* NULL or "" -> connect anonymously            */
    const char *password;       /* ignored unless a username is set (MQTT-3.1.2-22) */
    uint16_t    keepalive_s;    /* 0 disables PINGREQ and the silence check     */

    const char *pub_topic;      /* NULL -> this client never publishes          */
    const char *pub_payload;
    uint32_t    pub_period_ms;

    const char *sub_topic;      /* NULL -> this client never subscribes         */
} mqtt_config_t;

/*
 * Spawn a FreeRTOS task that waits until is_up() reports the interface ready,
 * then keeps a broker session alive forever: connect -> CONNECT/CONNACK ->
 * SUBSCRIBE -> publish + serve arriving messages, reconnecting when the link
 * drops. Arriving messages are logged as "<topic> : <payload>".
 *   name   - short label; also the task name and log prefix (e.g. "eth")
 *   ops    - socket vtable for this interface
 *   cfg    - session parameters, copied (see the note above about the strings)
 *   is_up  - predicate the task polls for link readiness
 */
void mqtt_client_start(const char *name, const net_sock_ops_t *ops,
                       const mqtt_config_t *cfg, bool (*is_up)(void));

#endif /* MQTT_H */
