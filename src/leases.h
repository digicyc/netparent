#ifndef NETPARENT_LEASES_H
#define NETPARENT_LEASES_H

#include "config.h"
#include "mqtt.h"

struct np_leases;

/* Allocate a leases watcher. `path` is the dnsmasq leases file
 * (typically /tmp/dhcp.leases on OpenWRT). */
struct np_leases *np_leases_create(const char *path,
                                   const struct np_config *cfg,
                                   struct np_mqtt *mqtt);

void np_leases_destroy(struct np_leases *l);

/* Poll the leases file for changes. Safe to call frequently; it
 * cheaply stats the file first and only re-reads when mtime/size
 * changes. Republishes the devices snapshot on any change. */
void np_leases_poll(struct np_leases *l);

/* Force a republish of the current devices snapshot. Use this after
 * a block/unblock so the cached snapshot stays accurate. */
void np_leases_republish(struct np_leases *l);

#endif
