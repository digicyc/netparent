#include "config.h"
#include "log.h"
#include "mqtt.h"
#include "nft.h"
#include "handler.h"
#include "leases.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct np_mqtt *g_mqtt = NULL;

static void on_signal(int sig)
{
    (void)sig;
    if (g_mqtt)
        np_mqtt_stop(g_mqtt);
}

static void on_message_trampoline(const char *topic,
                                  const void *payload, int paylen,
                                  void *user)
{
    np_handle_message((struct np_handler *)user, topic, payload, paylen);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s [-f]\n"
        "  -f    foreground mode, log to stderr instead of syslog\n"
        "  -h    show this help\n",
        argv0);
}

int main(int argc, char **argv)
{
    int foreground = 0;
    int opt;
    while ((opt = getopt(argc, argv, "fh")) != -1) {
        switch (opt) {
        case 'f': foreground = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }

    struct np_config cfg;
    if (np_config_load(&cfg) != 0) {
        fprintf(stderr, "failed to load configuration\n");
        return 1;
    }

    np_log_init("netparent",
                foreground ? 0 : (cfg.log_syslog ? 1 : 0),
                (enum np_log_level)cfg.log_level);

    LOG_INFO("netparent starting (device_id=%s, broker=%s:%d)",
             cfg.device_id, cfg.broker_host, cfg.broker_port);

    if (np_nft_ensure(cfg.nft_table, cfg.nft_set) != 0) {
        LOG_ERR("nftables setup failed; refusing to start");
        np_config_free(&cfg);
        return 1;
    }

    struct np_handler handler = { .cfg = &cfg };

    struct np_mqtt *m = np_mqtt_create(&cfg, on_message_trampoline, &handler);
    if (!m) {
        LOG_ERR("failed to create MQTT client");
        np_config_free(&cfg);
        return 1;
    }
    handler.mqtt = m;
    g_mqtt = m;

    struct np_leases *leases = np_leases_create("/tmp/dhcp.leases", &cfg, m);
    if (!leases)
        LOG_WARN("could not create leases watcher; continuing without it");
    handler.leases = leases;
    np_mqtt_set_tick(m, (np_mqtt_tick_cb)np_leases_poll, leases);

    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int rc = np_mqtt_run(m);

    LOG_INFO("netparent shutting down");
    np_leases_destroy(leases);
    np_mqtt_destroy(m);
    np_config_free(&cfg);
    np_log_close();
    return rc == 0 ? 0 : 1;
}
