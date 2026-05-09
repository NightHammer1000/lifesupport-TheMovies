#include "log.h"

FILE *g_logfile = NULL;

void log_init(void) {
    if (!g_logfile) {
        /* Truncate on each ASI load — every game run starts a fresh log so
           old runs don't pollute the diagnostic context. */
        g_logfile = fopen("movies_fix.log", "w");
    }
}

void log_close(void) {
    if (g_logfile) {
        fclose(g_logfile);
        g_logfile = NULL;
    }
}

void proxy_log(const char *fmt, ...) {
    if (!g_logfile) return;
    va_list args;
    va_start(args, fmt);
    fprintf(g_logfile, "[%08lu] ", (unsigned long)GetTickCount());
    vfprintf(g_logfile, fmt, args);
    fputc('\n', g_logfile);
    fflush(g_logfile);
    va_end(args);
}
