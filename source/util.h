/*
 * util.h – naplózás, végzetes hiba, időzítés, elérési utak
 *
 * Copyright (C) 2026 Levi (Switch port), MIT license.
 */
#pragma once

#include <stdint.h>

// Az SD-kártyán: a játék könyvtára (a libPirates.so és az Android-adatmappa ide kerül).
#define DATA_PATH  "sdmc:/switch/backstab/"
#define SO_PATH    DATA_PATH "libPirates.so"
#define GAME_FILES DATA_PATH "com.gameloft.android.ANMP.GloftSDHM/files/"
#define LOG_PATH   DATA_PATH "log.txt"

void log_init(void);
void platform_log_write(const char *s);
void log_printf(const char *level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define log_info(...)  log_printf("I", __VA_ARGS__)
#define log_error(...) log_printf("E", __VA_ARGS__)
#ifdef DEBUG
#define log_debug(...) log_printf("D", __VA_ARGS__)
#else
#define log_debug(...) do {} while (0)
#endif

// Hibaüzenet a Switch hibaablakában, aztán kilépés.
void fatal_error(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

uint64_t time_ms(void);
int file_exists(const char *path);
// A szülőkönyvtárak létrehozása (mkdir -p a fájl könyvtárára).
void mkpath(const char *path);
