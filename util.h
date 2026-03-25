#ifndef UTIL_H
#define UTIL_H

#include <stdio.h>

extern FILE *g_log;

void log_open(const char *path);
void log_close(void);
void logprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logerrorf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
