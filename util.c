#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "util.h"

FILE *g_log = NULL;

static void make_parent_dirs(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

void log_open(const char *path) {
    make_parent_dirs(path);
    g_log = fopen(path, "w");
    if (!g_log)
        fprintf(stderr, "Warning: cannot open log file '%s': %s\n", path, strerror(errno));
}

void log_close(void) {
    if (g_log) { fclose(g_log); g_log = NULL; }
}

void logprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    if (g_log) { va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap); fflush(g_log); }
}

void logerrorf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    if (g_log) { va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap); fflush(g_log); }
}
