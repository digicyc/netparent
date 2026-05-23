#ifndef NETPARENT_CONFIG_H
#define NETPARENT_CONFIG_H

#include <stdbool.h>

struct np_config {
    /* Identity */
    char *device_id;        /* used in topic prefix: netparent/<device_id>/... */

    /* Broker */
    char *broker_host;
    int   broker_port;      /* default 8883 */
    int   keepalive;        /* seconds, default 30 */

    /* TLS */
    bool  tls_enabled;      /* default true */
    char *tls_ca_file;
    char *tls_cert_file;    /* optional client cert */
    char *tls_key_file;     /* optional client key */
    bool  tls_insecure;     /* skip hostname verification (NOT recommended) */

    /* Auth (optional, in addition to TLS) */
    char *username;
    char *password;

    /* nftables */
    char *nft_table;        /* default "inet netparent" */
    char *nft_set;          /* default "blocked_macs" */

    /* Logging */
    int   log_level;        /* 0..3 */
    bool  log_syslog;       /* default true */
};

/* Load configuration from UCI ("netparent.main").
 * Falls back to sane defaults for any missing options.
 * Returns 0 on success, -1 on fatal error.
 */
int np_config_load(struct np_config *cfg);

/* Free strings owned by cfg (does not free cfg itself). */
void np_config_free(struct np_config *cfg);

#endif
