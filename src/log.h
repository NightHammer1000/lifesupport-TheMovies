#ifndef PROXY_LOG_H
#define PROXY_LOG_H

#include <stdio.h>
#include <stdarg.h>
#include <windows.h>

extern FILE *g_logfile;

void log_init(void);
void log_close(void);

__attribute__((format(printf, 1, 2)))
void proxy_log(const char *fmt, ...);

#endif /* PROXY_LOG_H */
