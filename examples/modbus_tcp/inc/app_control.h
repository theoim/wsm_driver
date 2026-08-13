/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Deferred reconfiguration.
 *
 * Applying a new IP means tearing down the sockets the request arrived on. Doing
 * that inside the HTTP handler would kill the connection before the response
 * left the device, so the browser would see a failed request and the user would
 * not learn the new address -- exactly when they need it most.
 *
 * So the handler validates, saves to NVS, answers, and posts the work here. A
 * separate task picks it up a moment later, by which time the response has been
 * sent and the socket closed. The user reads the new address off the page they
 * are still looking at, then reconnects.
 */
#ifndef APP_CONTROL_H
#define APP_CONTROL_H

#include <stdbool.h>

#include "app_config.h"
#include "mb_server.h"
#include "mb_store.h"
#include "web_server.h"

/*
 * Start the control task.
 *   ops     - socket vtable the Modbus server runs on (Ethernet)
 *   store   - the shared data model, handed to each restarted server
 *   server  - the running Modbus server; the task takes ownership of the handle
 *   current - the configuration in force, kept up to date as changes apply
 */
bool app_control_start(const void *ops, mb_store_t *store,
                       mb_server_t *server, web_server_t *web,
                       const app_config_t *current);

/*
 * Request that `cfg` be applied. Returns immediately. Only the fields that
 * actually differ are acted on: an unchanged IP does not disturb the link, and
 * an unchanged port does not restart the Modbus server.
 */
bool app_control_apply(const app_config_t *cfg);

/* What is running right now, which is not always what was last requested --
 * an apply can fail. */
void app_control_current(app_config_t *out);

/*
 * True when NVS holds a configuration the running device is not using: an apply
 * was requested and either has not finished or could not be carried out.
 *
 * Surfaced on the dashboard because the alternative is a device that says 502
 * while its saved settings say 1502, and only reveals the difference at the next
 * reboot -- in a cabinet, months later.
 */
bool app_control_pending(void);

#endif /* APP_CONTROL_H */
