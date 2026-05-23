#include "config.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <uci.h>

#define CFG_PACKAGE "netparent"
#define CFG_SECTION "main"

static char *xstrdup(const char *s)
{
    if (!s)
        return NULL;
    char *r = strdup(s);
    if (!r) {
        LOG_ERR("out of memory");
        abort();
    }
    return r;
}

static void set_str(char **dst, const char *src)
{
    free(*dst);
    *dst = xstrdup(src);
}

static const char *uci_get(struct uci_section *s, const char *name)
{
    struct uci_option *o = uci_lookup_option(s->package->ctx, s, name);
    if (!o || o->type != UCI_TYPE_STRING)
        return NULL;
    return o->v.string;
}

static int uci_get_int(struct uci_section *s, const char *name, int defval)
{
    const char *v = uci_get(s, name);
    if (!v || !*v)
        return defval;
    return (int)strtol(v, NULL, 10);
}

static bool uci_get_bool(struct uci_section *s, const char *name, bool defval)
{
    const char *v = uci_get(s, name);
    if (!v || !*v)
        return defval;
    if (!strcmp(v, "1") || !strcasecmp(v, "true") ||
        !strcasecmp(v, "yes") || !strcasecmp(v, "on"))
        return true;
    if (!strcmp(v, "0") || !strcasecmp(v, "false") ||
        !strcasecmp(v, "no") || !strcasecmp(v, "off"))
        return false;
    return defval;
}

static void apply_defaults(struct np_config *cfg)
{
    if (!cfg->device_id)    set_str(&cfg->device_id, "router-1");
    if (!cfg->broker_host)  set_str(&cfg->broker_host, "127.0.0.1");
    if (cfg->broker_port <= 0) cfg->broker_port = cfg->tls_enabled ? 8883 : 1883;
    if (cfg->keepalive <= 0)   cfg->keepalive = 30;
    if (!cfg->nft_table)    set_str(&cfg->nft_table, "inet netparent");
    if (!cfg->nft_set)      set_str(&cfg->nft_set, "blocked_macs");
    if (cfg->log_level < 0) cfg->log_level = NP_LOG_INFO;
}

int np_config_load(struct np_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->tls_enabled = true;
    cfg->log_syslog = true;
    cfg->log_level = -1;

    struct uci_context *ctx = uci_alloc_context();
    if (!ctx) {
        LOG_ERR("uci_alloc_context failed");
        return -1;
    }

    struct uci_package *pkg = NULL;
    if (uci_load(ctx, CFG_PACKAGE, &pkg) != UCI_OK || !pkg) {
        LOG_WARN("UCI package '%s' not found; using defaults", CFG_PACKAGE);
        uci_free_context(ctx);
        apply_defaults(cfg);
        return 0;
    }

    struct uci_element *e;
    struct uci_section *main_sec = NULL;
    uci_foreach_element(&pkg->sections, e) {
        struct uci_section *s = uci_to_section(e);
        if (!strcmp(s->e.name, CFG_SECTION) ||
            !strcmp(s->type, CFG_SECTION)) {
            main_sec = s;
            break;
        }
    }

    if (!main_sec) {
        LOG_WARN("UCI section '%s.%s' not found; using defaults",
                 CFG_PACKAGE, CFG_SECTION);
        uci_unload(ctx, pkg);
        uci_free_context(ctx);
        apply_defaults(cfg);
        return 0;
    }

    set_str(&cfg->device_id,     uci_get(main_sec, "device_id"));
    set_str(&cfg->broker_host,   uci_get(main_sec, "broker_host"));
    cfg->broker_port = uci_get_int(main_sec, "broker_port", 0);
    cfg->keepalive   = uci_get_int(main_sec, "keepalive", 0);

    cfg->tls_enabled = uci_get_bool(main_sec, "tls_enabled", true);
    set_str(&cfg->tls_ca_file,   uci_get(main_sec, "tls_ca_file"));
    set_str(&cfg->tls_cert_file, uci_get(main_sec, "tls_cert_file"));
    set_str(&cfg->tls_key_file,  uci_get(main_sec, "tls_key_file"));
    cfg->tls_insecure = uci_get_bool(main_sec, "tls_insecure", false);

    set_str(&cfg->username, uci_get(main_sec, "username"));
    set_str(&cfg->password, uci_get(main_sec, "password"));

    set_str(&cfg->nft_table, uci_get(main_sec, "nft_table"));
    set_str(&cfg->nft_set,   uci_get(main_sec, "nft_set"));

    cfg->log_level  = uci_get_int(main_sec, "log_level", -1);
    cfg->log_syslog = uci_get_bool(main_sec, "log_syslog", true);

    uci_unload(ctx, pkg);
    uci_free_context(ctx);
    apply_defaults(cfg);

    if (cfg->tls_enabled && !cfg->tls_ca_file) {
        LOG_ERR("tls_enabled=1 but tls_ca_file not set");
        return -1;
    }
    return 0;
}

void np_config_free(struct np_config *cfg)
{
    if (!cfg)
        return;
    free(cfg->device_id);
    free(cfg->broker_host);
    free(cfg->tls_ca_file);
    free(cfg->tls_cert_file);
    free(cfg->tls_key_file);
    free(cfg->username);
    free(cfg->password);
    free(cfg->nft_table);
    free(cfg->nft_set);
    memset(cfg, 0, sizeof(*cfg));
}
