#ifndef NETPARENT_HANDLER_H
#define NETPARENT_HANDLER_H

#include "config.h"
#include "mqtt.h"
#include "leases.h"

struct np_handler {
    const struct np_config *cfg;
    struct np_mqtt *mqtt;
    struct np_leases *leases;  /* may be NULL */
};

/* Process an incoming MQTT command.
 * Topic must look like "netparent/<device_id>/cmd/<action>".
 * Payload is JSON, e.g. {"mac":"aa:bb:cc:dd:ee:ff","req_id":"abc"}.
 *
 * Supported actions:
 *   block    — add MAC to blocked set
 *   unblock  — remove MAC from blocked set
 *   status   — report whether a MAC is currently blocked
 *   list     — list all blocked MACs
 *   ping     — health check, replies on response/<req_id>
 *
 * Always publishes a response to "netparent/<device_id>/response/<req_id>"
 * when req_id is provided.
 */
void np_handle_message(struct np_handler *h,
                       const char *topic,
                       const void *payload, int paylen);

#endif
