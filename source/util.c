/*
 * util.c – naplózás, végzetes hiba, időzítés, elérési utak
 *
 * Copyright (C) 2026 Levi (Switch port), MIT license.
 */
#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>

static FILE *g_log = NULL;
static Mutex g_log_mutex = 0;

void log_init(void) {
    mkpath(LOG_PATH);
    g_log = fopen(LOG_PATH, "w");
}

void platform_log_write(const char *s) {
    size_t len = strlen(s);
    svcOutputDebugString(s, len);
    mutexLock(&g_log_mutex);
    if (g_log) {
        fputs(s, g_log);
        if (len == 0 || s[len - 1] != '\n')
            fputc('\n', g_log);
        fflush(g_log);
    }
    mutexUnlock(&g_log_mutex);
}

void log_printf(const char *level, const char *fmt, ...) {
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "[%s %6u] ", level, (unsigned)(time_ms() % 1000000));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    platform_log_write(buf);
}

void fatal_error(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    log_printf("F", "%s", msg);

    ErrorApplicationConfig c;
    if (R_SUCCEEDED(errorApplicationCreate(&c, "Backstab HD", msg))) {
        errorApplicationShow(&c);
    }
    exit(1);
}

uint64_t time_ms(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}

PerfStats perf;

uint64_t perf_tick(void) {
    return armGetSystemTick();
}

uint32_t perf_ms(uint64_t ticks) {
    return (uint32_t)(armTicksToNs(ticks) / 1000000ULL);
}

int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

void mkpath(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    // a "sdmc:/" előtag utántól keressük a perjeleket
    char *start = strstr(tmp, ":/");
    char *p = start ? start + 2 : tmp + 1;
    for (; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
}
