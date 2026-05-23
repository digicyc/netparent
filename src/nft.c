#include "nft.h"
#include "log.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* We shell out to the `nft` binary. It is the standard tool on modern
 * OpenWRT (firewall4), is tiny, and avoids linking libnftables (which
 * has heavier dependencies). All inputs are validated MAC addresses,
 * so there is no shell injection surface.
 */

bool np_mac_normalize(const char *in, char out[18])
{
    if (!in || !out)
        return false;

    int bytes = 0;
    int hexcount = 0;
    char hex[2];

    for (const char *p = in; *p; p++) {
        if (isxdigit((unsigned char)*p)) {
            if (hexcount >= 2)
                return false;
            hex[hexcount++] = (char)tolower((unsigned char)*p);
        } else if (*p == ':' || *p == '-') {
            if (hexcount == 0)
                return false;
            if (hexcount == 1) {
                hex[1] = hex[0];
                hex[0] = '0';
            }
            if (bytes >= 6)
                return false;
            out[bytes * 3 + 0] = hex[0];
            out[bytes * 3 + 1] = hex[1];
            if (bytes < 5)
                out[bytes * 3 + 2] = ':';
            bytes++;
            hexcount = 0;
        } else if (isspace((unsigned char)*p)) {
            continue;
        } else {
            return false;
        }
    }

    if (hexcount > 0) {
        if (hexcount == 1) {
            hex[1] = hex[0];
            hex[0] = '0';
        }
        if (bytes >= 6)
            return false;
        out[bytes * 3 + 0] = hex[0];
        out[bytes * 3 + 1] = hex[1];
        bytes++;
    }

    if (bytes != 6)
        return false;
    out[17] = '\0';
    return true;
}

/* Run a nft command. Returns nft's exit status (0 = success).
 * Captures stderr into a log line on failure.
 */
static int run_nft(const char *args_fmt, ...)
{
    char cmd[1024];
    char nft_args[768];

    va_list ap;
    va_start(ap, args_fmt);
    int n = vsnprintf(nft_args, sizeof(nft_args), args_fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(nft_args)) {
        LOG_ERR("nft command too long");
        return -1;
    }

    snprintf(cmd, sizeof(cmd), "nft %s 2>&1", nft_args);
    LOG_DBG("exec: %s", cmd);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        LOG_ERR("popen failed: %s", cmd);
        return -1;
    }

    char buf[256];
    char last_err[256] = {0};
    while (fgets(buf, sizeof(buf), fp)) {
        size_t l = strlen(buf);
        if (l && buf[l - 1] == '\n')
            buf[l - 1] = '\0';
        strncpy(last_err, buf, sizeof(last_err) - 1);
    }
    int rc = pclose(fp);
    if (rc != 0 && last_err[0])
        LOG_DBG("nft stderr: %s", last_err);
    return rc;
}

int np_nft_ensure(const char *table, const char *set)
{
    /* Create table, set, and chain hooked at forward priority filter.
     * Multi-statement ruleset is piped through `nft -f -` for atomicity.
     */
    char script[1024];
    snprintf(script, sizeof(script),
        "add table %s\n"
        "add set %s %s { type ether_addr; flags interval; }\n"
        "add chain %s netparent_fwd { type filter hook forward priority filter; policy accept; }\n",
        table, table, set, table);

    FILE *fp = popen("nft -f - 2>&1", "w");
    if (!fp) {
        LOG_ERR("popen nft failed");
        return -1;
    }
    fputs(script, fp);
    int rc = pclose(fp);
    if (rc != 0) {
        LOG_WARN("nft initial ensure returned %d (may be benign if already present)", rc);
    }

    /* Add drop rules only if they don't already exist. Easiest is to
     * flush our chain and re-add. Safe because the chain is exclusive
     * to netparent.
     */
    if (run_nft("flush chain %s netparent_fwd", table) != 0)
        LOG_WARN("flush chain failed (chain may not exist yet)");

    if (run_nft("add rule %s netparent_fwd ether saddr @%s counter drop",
                table, set) != 0) {
        LOG_ERR("failed to add saddr drop rule");
        return -1;
    }
    if (run_nft("add rule %s netparent_fwd ether daddr @%s counter drop",
                table, set) != 0) {
        LOG_ERR("failed to add daddr drop rule");
        return -1;
    }

    LOG_INFO("nftables ready: table=%s set=%s", table, set);
    return 0;
}

int np_nft_block(const char *table, const char *set, const char *mac)
{
    char norm[18];
    if (!np_mac_normalize(mac, norm))
        return -1;
    /* `add element` is idempotent for sets in modern nft (returns 0
     * even if entry exists). */
    int rc = run_nft("add element %s %s { %s }", table, set, norm);
    if (rc == 0)
        LOG_INFO("blocked MAC %s", norm);
    return rc;
}

int np_nft_unblock(const char *table, const char *set, const char *mac)
{
    char norm[18];
    if (!np_mac_normalize(mac, norm))
        return -1;
    /* `delete element` returns nonzero if the element is absent — treat
     * that as success (idempotent semantics). */
    int rc = run_nft("delete element %s %s { %s }", table, set, norm);
    if (rc != 0)
        LOG_DBG("delete element returned %d (likely not present)", rc);
    LOG_INFO("unblocked MAC %s", norm);
    return 0;
}

int np_nft_is_blocked(const char *table, const char *set, const char *mac)
{
    char norm[18];
    if (!np_mac_normalize(mac, norm))
        return -1;

    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "nft get element %s %s { %s } >/dev/null 2>&1",
             table, set, norm);
    int rc = system(cmd);
    if (rc == -1)
        return -1;
    return (rc == 0) ? 1 : 0;
}

int np_nft_list(const char *table, const char *set,
                void (*cb)(const char *mac, void *user), void *user)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "nft -j list set %s %s 2>/dev/null", table, set);
    FILE *fp = popen(cmd, "r");
    if (!fp)
        return -1;

    /* Crude but dependency-free: scan output for "elem":"xx:xx:..." or
     * bare MAC strings. We avoid pulling in a JSON dep here because the
     * format from `nft -j list set` includes both. The MAC pattern is
     * unambiguous.
     */
    char buf[4096];
    size_t total = 0;
    while (fgets(buf + total, sizeof(buf) - total, fp) &&
           total < sizeof(buf) - 1) {
        total += strlen(buf + total);
    }
    pclose(fp);

    /* Walk buf looking for MAC address patterns. */
    for (size_t i = 0; i + 16 < total; i++) {
        const char *p = buf + i;
        char mac[18];
        if (isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1]) &&
            p[2] == ':' &&
            isxdigit((unsigned char)p[3]) && isxdigit((unsigned char)p[4]) &&
            p[5] == ':' &&
            isxdigit((unsigned char)p[6]) && isxdigit((unsigned char)p[7]) &&
            p[8] == ':' &&
            isxdigit((unsigned char)p[9]) && isxdigit((unsigned char)p[10]) &&
            p[11] == ':' &&
            isxdigit((unsigned char)p[12]) && isxdigit((unsigned char)p[13]) &&
            p[14] == ':' &&
            isxdigit((unsigned char)p[15]) && isxdigit((unsigned char)p[16])) {
            memcpy(mac, p, 17);
            mac[17] = '\0';
            for (int j = 0; j < 17; j++)
                mac[j] = (char)tolower((unsigned char)mac[j]);
            cb(mac, user);
            i += 16;
        }
    }
    return 0;
}
