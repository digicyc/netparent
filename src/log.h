#ifndef NETPARENT_LOG_H
#define NETPARENT_LOG_H

#include <stdarg.h>

enum np_log_level {
    NP_LOG_ERR  = 0,
    NP_LOG_WARN = 1,
    NP_LOG_INFO = 2,
    NP_LOG_DBG  = 3,
};

void np_log_init(const char *ident, int use_syslog, enum np_log_level level);
void np_log_close(void);
void np_log(enum np_log_level level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define LOG_ERR(...)  np_log(NP_LOG_ERR,  __VA_ARGS__)
#define LOG_WARN(...) np_log(NP_LOG_WARN, __VA_ARGS__)
#define LOG_INFO(...) np_log(NP_LOG_INFO, __VA_ARGS__)
#define LOG_DBG(...)  np_log(NP_LOG_DBG,  __VA_ARGS__)

#endif
