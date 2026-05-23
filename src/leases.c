#include "leases.h"
#include "log.h"
#include "nft.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_DEVICES   256
#define HOSTNAME_LEN  64
#define IP_LEN        46  /* room for IPv6 too */

struct dev {
    char mac[18];
    char ip[IP_LEN];
    char hostname[HOSTNAME_LEN];
    int  blocked;
    long lease_expires;
    long last_seen;
};

struct np_leases {
    char *path;
    const struct np_config *cfg;
    struct np_mqtt *mqtt;

    struct dev list[MAX_DEVICES];
    int        count;

    /* Snapshot of last published state for diffing. */
    struct dev prev[MAX_DEVICES];
    int        prev_count;

    time_t last_mtime;
    off_t  last_size;
    int    first_run;
};

static int devcmp(const void *a, const void *b)
{
    return strcmp(((const struct dev *)a)->mac,
                  ((const struct dev *)b)->mac);
}

static void str_lower(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z')
            *s = (char)(*s + 32);
}

struct np_leases *np_leases_create(const char *path,
                                   const struct np_config *cfg,
                                   struct np_mqtt *mqtt)
{
    struct np_leases *l = calloc(1, sizeof(*l));
    if (!l)
        return NULL;
    l->path = strdup(path ? path : "/tmp/dhcp.leases");
    l->cfg = cfg;
    l->mqtt = mqtt;
    l->first_run = 1;
    return l;
}

void np_leases_destroy(struct np_leases *l)
{
    if (!l)
        return;
    free(l->path);
    free(l);
}

/* Parse a single dnsmasq lease line into `out`. Returns 0 on success.
 * Format: "<expiry> <mac> <ip> <hostname> <client_id>"
 */
static int parse_lease_line(char *line, struct dev *out)
{
    long expiry;
    char mac[64], ip[IP_LEN], host[HOSTNAME_LEN];

    int n = sscanf(line, "%ld %63s %45s %63s", &expiry, mac, ip, host);
    if (n < 3)
        return -1;

    memset(out, 0, sizeof(*out));
    char norm[18];
    if (!np_mac_normalize(mac, norm))
        return -1;
    memcpy(out->mac, norm, 18);

    strncpy(out->ip, ip, IP_LEN - 1);
    if (n >= 4 && strcmp(host, "*") != 0)
        strncpy(out->hostname, host, HOSTNAME_LEN - 1);

    out->lease_expires = expiry;
    out->last_seen = (long)time(NULL);
    return 0;
}

static void add_or_merge(struct dev *list, int *count,
                         const struct dev *d)
{
    for (int i = 0; i < *count; i++) {
        if (strcmp(list[i].mac, d->mac) == 0) {
            /* Merge: prefer the entry that has more info. */
            if (d->ip[0])           strcpy(list[i].ip, d->ip);
            if (d->hostname[0])     strcpy(list[i].hostname, d->hostname);
            if (d->lease_expires)   list[i].lease_expires = d->lease_expires;
            if (d->last_seen)       list[i].last_seen = d->last_seen;
            if (d->blocked)         list[i].blocked = 1;
            return;
        }
    }
    if (*count >= MAX_DEVICES) {
        LOG_WARN("device list full (%d), dropping %s", MAX_DEVICES, d->mac);
        return;
    }
    list[(*count)++] = *d;
}

static void add_blocked_cb(const char *mac, void *user)
{
    struct np_leases *l = user;
    struct dev d = {0};
    strncpy(d.mac, mac, 17);
    str_lower(d.mac);
    d.blocked = 1;
    add_or_merge(l->list, &l->count, &d);
}

static int rebuild(struct np_leases *l)
{
    l->count = 0;

    /* 1. Read leases file. */
    FILE *fp = fopen(l->path, "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            struct dev d;
            if (parse_lease_line(line, &d) == 0)
                add_or_merge(l->list, &l->count, &d);
        }
        fclose(fp);
    } else {
        LOG_DBG("leases file %s not readable yet", l->path);
    }

    /* 2. Merge in currently blocked MACs from nftables. */
    np_nft_list(l->cfg->nft_table, l->cfg->nft_set, add_blocked_cb, l);

    /* 3. Mark blocked flag on leases that are in the set. */
    for (int i = 0; i < l->count; i++) {
        if (!l->list[i].blocked)
            l->list[i].blocked =
                np_nft_is_blocked(l->cfg->nft_table, l->cfg->nft_set,
                                  l->list[i].mac) > 0;
    }

    qsort(l->list, (size_t)l->count, sizeof(l->list[0]), devcmp);
    return 0;
}

static cJSON *dev_to_json(const struct dev *d)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "mac",  d->mac);
    cJSON_AddStringToObject(o, "ip",   d->ip);
    cJSON_AddStringToObject(o, "hostname", d->hostname);
    cJSON_AddBoolToObject(o,   "blocked", d->blocked);
    cJSON_AddNumberToObject(o, "lease_expires", (double)d->lease_expires);
    cJSON_AddNumberToObject(o, "last_seen", (double)d->last_seen);
    return o;
}

static int devs_differ(const struct dev *a, const struct dev *b)
{
    if (strcmp(a->mac, b->mac) != 0) return 1;
    if (strcmp(a->ip,  b->ip)  != 0) return 1;
    if (strcmp(a->hostname, b->hostname) != 0) return 1;
    if (a->blocked != b->blocked) return 1;
    if (a->lease_expires != b->lease_expires) return 1;
    return 0;
}

static void emit_event(struct np_leases *l, const char *event,
                       const struct dev *d, const char *mac_only)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", event);
    if (d)
        cJSON_AddItemToObject(o, "device", dev_to_json(d));
    if (mac_only)
        cJSON_AddStringToObject(o, "mac", mac_only);

    char *s = cJSON_PrintUnformatted(o);
    if (s) {
        char topic[128];
        snprintf(topic, sizeof(topic),
                 "netparent/%s/event/device", l->cfg->device_id);
        /* event/device is non-retained, handler.c uses publish_response
         * helper which always targets response/<req_id>; for events we
         * call the lower-level mosq publish through a small inline. */
        /* We don't have a generic publish in mqtt.h, so reuse the
         * response helper with a magic suffix... cleaner: extend mqtt
         * API. For now, log + skip if MQTT not ready. */
        np_mqtt_publish_event(l->mqtt, "device", s);
        free(s);
    }
    cJSON_Delete(o);
}

static void diff_and_emit(struct np_leases *l)
{
    if (l->first_run) {
        l->first_run = 0;
        memcpy(l->prev, l->list, sizeof(l->prev[0]) * (size_t)l->count);
        l->prev_count = l->count;
        return;
    }

    int i = 0, j = 0;
    while (i < l->prev_count && j < l->count) {
        int c = strcmp(l->prev[i].mac, l->list[j].mac);
        if (c == 0) {
            if (devs_differ(&l->prev[i], &l->list[j]))
                emit_event(l, "changed", &l->list[j], NULL);
            i++; j++;
        } else if (c < 0) {
            emit_event(l, "removed", NULL, l->prev[i].mac);
            i++;
        } else {
            emit_event(l, "added", &l->list[j], NULL);
            j++;
        }
    }
    while (i < l->prev_count) {
        emit_event(l, "removed", NULL, l->prev[i].mac);
        i++;
    }
    while (j < l->count) {
        emit_event(l, "added", &l->list[j], NULL);
        j++;
    }

    memcpy(l->prev, l->list, sizeof(l->prev[0]) * (size_t)l->count);
    l->prev_count = l->count;
}

static void publish_snapshot(struct np_leases *l)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "device_id", l->cfg->device_id);
    cJSON_AddNumberToObject(o, "updated_at", (double)time(NULL));
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < l->count; i++)
        cJSON_AddItemToArray(arr, dev_to_json(&l->list[i]));
    cJSON_AddItemToObject(o, "devices", arr);

    char *s = cJSON_PrintUnformatted(o);
    if (s) {
        np_mqtt_publish_devices(l->mqtt, s);
        LOG_DBG("published devices snapshot (%d entries)", l->count);
        free(s);
    }
    cJSON_Delete(o);
}

void np_leases_republish(struct np_leases *l)
{
    if (!l) return;
    rebuild(l);
    diff_and_emit(l);
    publish_snapshot(l);
}

void np_leases_poll(struct np_leases *l)
{
    if (!l) return;

    struct stat st;
    int has_stat = (stat(l->path, &st) == 0);
    int changed = 0;
    if (has_stat) {
        if (st.st_mtime != l->last_mtime || st.st_size != l->last_size) {
            l->last_mtime = st.st_mtime;
            l->last_size  = st.st_size;
            changed = 1;
        }
    }
    if (changed || l->first_run)
        np_leases_republish(l);
}
