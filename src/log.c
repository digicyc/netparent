#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

static int g_use_syslog = 0;
static enum np_log_level g_level = NP_LOG_INFO;

static int to_syslog_prio(enum np_log_level l)
{
    switch (l) {
    case NP_LOG_ERR:  return LOG_ERR;
    case NP_LOG_WARN: return LOG_WARNING;
    case NP_LOG_INFO: return LOG_INFO;
    case NP_LOG_DBG:  return LOG_DEBUG;
    }
    return LOG_INFO;
}

static const char *level_tag(enum np_log_level l)
{
    switch (l) {
    case NP_LOG_ERR:  return "ERROR";
    case NP_LOG_WARN: return "WARN ";
    case NP_LOG_INFO: return "INFO ";
    case NP_LOG_DBG:  return "DEBUG";
    }
    return "?????";
}

void np_log_init(const char *ident, int use_syslog, enum np_log_level level)
{
    g_use_syslog = use_syslog;
    g_level = level;
    if (g_use_syslog)
        openlog(ident ? ident : "netparent", LOG_PID | LOG_CONS, LOG_DAEMON);
}

void np_log_close(void)
{
    if (g_use_syslog)
        closelog();
}

void np_log(enum np_log_level level, const char *fmt, ...)
{
    if (level > g_level)
        return;

    va_list ap;
    va_start(ap, fmt);

    if (g_use_syslog) {
        vsyslog(to_syslog_prio(level), fmt, ap);
    } else {
        char ts[32];
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
        fprintf(stderr, "%s [%s] ", ts, level_tag(level));
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
    }

    va_end(ap);
}
