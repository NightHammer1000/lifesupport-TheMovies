#include "log.h"

FILE *g_logfile = NULL;

void log_init(void) {
    if (!g_logfile) {
        g_logfile = fopen("movies_fix.log", "a");
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
