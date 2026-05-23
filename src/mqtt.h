#ifndef NETPARENT_MQTT_H
#define NETPARENT_MQTT_H

#include "config.h"

struct np_mqtt;

/* Callback fired for every received command message.
 *   topic:    null-terminated topic string
 *   payload:  message payload (NOT null-terminated)
 *   paylen:   payload byte length
 *   user:     opaque pointer passed at create-time
 */
typedef void (*np_mqtt_msg_cb)(const char *topic,
                               const void *payload, int paylen,
                               void *user);

/* Allocate and configure an MQTT client from cfg. Does not connect yet. */
struct np_mqtt *np_mqtt_create(const struct np_config *cfg,
                               np_mqtt_msg_cb on_message,
                               void *user);

/* Register an optional periodic tick callback invoked from inside the
 * run loop (roughly every loop iteration, ~500ms). Pass NULL to clear. */
typedef void (*np_mqtt_tick_cb)(void *user);
void np_mqtt_set_tick(struct np_mqtt *m, np_mqtt_tick_cb cb, void *user);

/* Connect, subscribe to command topics, and run the network loop until
 * np_mqtt_stop() is called or a fatal error occurs.
 * Returns 0 on clean shutdown, -1 on fatal error.
 */
int np_mqtt_run(struct np_mqtt *m);

/* Signal the run loop to exit (signal-safe). */
void np_mqtt_stop(struct np_mqtt *m);

/* Publish a retained status message. */
int np_mqtt_publish_status(struct np_mqtt *m, const char *json_payload);

/* Publish a response to a command. */
int np_mqtt_publish_response(struct np_mqtt *m,
                             const char *suffix,
                             const char *json_payload);

/* Publish the retained devices snapshot to netparent/<id>/devices. */
int np_mqtt_publish_devices(struct np_mqtt *m, const char *json_payload);

/* Publish a non-retained event to netparent/<id>/event/<kind>. */
int np_mqtt_publish_event(struct np_mqtt *m,
                          const char *kind,
                          const char *json_payload);

void np_mqtt_destroy(struct np_mqtt *m);

#endif
