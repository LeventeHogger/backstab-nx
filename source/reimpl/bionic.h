/*
 * bionic.h – az Android libc (bionic, armeabi-v7a) ABI-jának pótlása newlib fölött
 *
 * A játék bionic-struktúrákkal és -konstansokkal hívja a libc-t; ahol ezek eltérnek a newlibtől
 * (FILE-tömb, 32 bites time_t és off_t, struct stat, open-flagek, 4 bájtos pthread-típusok, setjmp),
 * itt fordítunk. Ahol az ABI azonos, a dynlib.c egyenesen a newlib-függvényt köti be.
 *
 * Copyright (C) 2026 Levi (Switch port), MIT license.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

// A bionic.c belső állapotának előkészítése (stack-guard, időalap). A resolve_imports() hívja,
// még a játék init_array-ja előtt.
void bionic_init(void);

// --- stdio ---------------------------------------------------------------------------------
// bionic: sizeof(FILE) == 84, a __sF[3] tömb stdin/stdout/stderr. A tömbbe mutató FILE*-ot minden
// wrapper a valódi newlib-folyamra fordítja; a többi FILE* a mi fopen-ünkből jön (newlib FILE,
// a _p/_r/_flags mezők ugyanott vannak, mint bionicban – a játék inline getc/ferror-ja ezt olvassa).
#define BIONIC_FILE_SIZE 84
extern uint8_t bionic_sF[3 * BIONIC_FILE_SIZE];
FILE *bionic_fix_fp(FILE *fp);

typedef int32_t bionic_fpos_t;   // bionicban fpos_t = off_t = 32 bites long

FILE *bionic_fopen(const char *path, const char *mode);
FILE *bionic_freopen(const char *path, const char *mode, FILE *fp);
int bionic_fclose(FILE *fp);
int bionic_fflush(FILE *fp);
int bionic_ferror(FILE *fp);
int bionic_setvbuf(FILE *fp, char *buf, int mode, size_t size);
size_t bionic_fread(void *ptr, size_t size, size_t n, FILE *fp);
size_t bionic_fwrite(const void *ptr, size_t size, size_t n, FILE *fp);
int bionic_fseek(FILE *fp, long off, int whence);
long bionic_ftell(FILE *fp);
int bionic_fgetpos(FILE *fp, bionic_fpos_t *pos);
int bionic_fsetpos(FILE *fp, const bionic_fpos_t *pos);
char *bionic_fgets(char *s, int n, FILE *fp);
int bionic_getc(FILE *fp);
int bionic_ungetc(int c, FILE *fp);
int bionic_srget(FILE *fp);
int bionic_fputc(int c, FILE *fp);
int bionic_putc(int c, FILE *fp);
int bionic_fputs(const char *s, FILE *fp);
int bionic_fprintf(FILE *fp, const char *fmt, ...);
int bionic_fscanf(FILE *fp, const char *fmt, ...);
FILE *bionic_tmpfile(void);
char *bionic_tmpnam(char *s);

// --- elérési utak és fájlleírók ----------------------------------------------------------------
// Android-út → SD-kártya: /sdcard/Android/data/<x> → DATA_PATH<x>, /data/data/<x> → DATA_PATH<x>,
// a többi /sdcard/<x> és a relatív utak → GAME_FILES<x>; az "eszköz:/" kezdetűek változatlanok.
const char *bionic_path(const char *path, char *buf, size_t size);

int bionic_open(const char *path, int flags, ...);
int32_t bionic_lseek(int fd, int32_t off, int whence);
int bionic_fcntl(int fd, int cmd, ...);
int bionic_remove(const char *path);
int bionic_rename(const char *from, const char *to);
int bionic_unlink(const char *path);
int bionic_chdir(const char *path);

// bionic struct stat (armeabi-v7a, 104 bájt). A mezőnevek nem st_atime/…, mert a newlib
// <sys/stat.h> ezeket makróként definiálja (st_atim.tv_sec).
struct stat_bionic {
    unsigned long long st_dev;        // 0
    unsigned char __pad0[4];          // 8
    unsigned long __st_ino;           // 12
    unsigned int st_mode;             // 16
    unsigned int st_nlink;            // 20
    unsigned long st_uid;             // 24
    unsigned long st_gid;             // 28
    unsigned long long st_rdev;       // 32
    unsigned char __pad3[4];          // 40 (+4 igazítás)
    long long st_size;                // 48
    unsigned long st_blksize;         // 56 (+4 igazítás)
    unsigned long long st_blocks;     // 64
    unsigned long st_atime_sec;       // 72
    unsigned long st_atime_nsec;
    unsigned long st_mtime_sec;       // 80
    unsigned long st_mtime_nsec;
    unsigned long st_ctime_sec;       // 88
    unsigned long st_ctime_nsec;
    unsigned long long st_ino;        // 96
};
int bionic_fstat(int fd, struct stat_bionic *st);

// --- idő (32 bites time_t) ---------------------------------------------------------------------
typedef int32_t time32_t;
struct timeval32 { int32_t tv_sec; int32_t tv_usec; };
struct timespec32 { int32_t tv_sec; int32_t tv_nsec; };
// bionic struct tm: a newlib 9 mezője + tm_gmtoff, tm_zone (44 bájt)
struct tm_bionic {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};

time32_t bionic_time(time32_t *t);
int bionic_gettimeofday(struct timeval32 *tv, void *tz);
struct tm_bionic *bionic_localtime(const time32_t *t);
struct tm_bionic *bionic_gmtime(const time32_t *t);
time32_t bionic_mktime(struct tm_bionic *tm);
double bionic_difftime(time32_t a, time32_t b);
size_t bionic_strftime(char *s, size_t max, const char *fmt, const struct tm_bionic *tm);
long bionic_clock(void);
int bionic_nanosleep(const struct timespec32 *req, struct timespec32 *rem);

// --- pthread (4 bájtos mutex, 24 bájtos attr) --------------------------------------------------
// A játék 4 bájtos mutex-helyén vagy egy bionic statikus inicializáló (0 / 0x4000 / 0x8000) áll,
// vagy a lustán lefoglalt newlib-mutexünk címe.
typedef struct { volatile uintptr_t value; } bionic_mutex_t;
typedef long bionic_mutexattr_t;
typedef struct {
    uint32_t flags;           // PTHREAD_ATTR_FLAG_DETACHED = 1
    void *stack_base;
    size_t stack_size;
    size_t guard_size;
    int32_t sched_policy;
    int32_t sched_priority;
} bionic_pthread_attr_t;
typedef uintptr_t bionic_pthread_t;

int bionic_pthread_mutex_init(bionic_mutex_t *m, const bionic_mutexattr_t *attr);
int bionic_pthread_mutex_destroy(bionic_mutex_t *m);
int bionic_pthread_mutex_lock(bionic_mutex_t *m);
int bionic_pthread_mutex_trylock(bionic_mutex_t *m);
int bionic_pthread_mutex_unlock(bionic_mutex_t *m);
int bionic_pthread_create(bionic_pthread_t *thread, const bionic_pthread_attr_t *attr,
                          void *(*start)(void *), void *arg);

// --- setjmp/longjmp (setjmp.S; a bionic jmp_buf 64 long = 256 bájt) --------------------------
int bionic_setjmp(long *env);
void bionic_longjmp(long *env, int val) __attribute__((noreturn));

// --- memória -----------------------------------------------------------------------------------
void *bionic_mmap(void *addr, size_t len, int prot, int flags, int fd, int32_t off);
int bionic_munmap(void *addr, size_t len);

// --- hálózat (offline port: minden hívás tisztán hibát ad) -------------------------------------
int bionic_socket(int domain, int type, int protocol);
int bionic_bind(int fd, const void *addr, unsigned len);
int bionic_setsockopt(int fd, int level, int name, const void *val, unsigned len);
int bionic_recvfrom(int fd, void *buf, size_t len, int flags, void *addr, unsigned *alen);
int bionic_sendto(int fd, const void *buf, size_t len, int flags, const void *addr, unsigned alen);
int bionic_select(int nfds, void *rd, void *wr, void *ex, struct timeval32 *timeout);
int bionic_getaddrinfo(const char *node, const char *service, const void *hints, void **res);
void bionic_freeaddrinfo(void *res);
int bionic_gethostname(char *name, size_t len);

// --- egyéb -------------------------------------------------------------------------------------
int bionic_android_log_print(int prio, const char *tag, const char *fmt, ...);
char *bionic_getenv(const char *name);
int bionic_system(const char *cmd);
long bionic_sysconf(int name);
char *bionic_setlocale(int category, const char *locale);
void bionic_abort(void) __attribute__((noreturn));
void bionic_exit(int code) __attribute__((noreturn));
int bionic_aeabi_atexit(void *obj, void (*dtor)(void *), void *dso);
void bionic_cxa_pure_virtual(void) __attribute__((noreturn));
void bionic_stack_chk_fail(void) __attribute__((noreturn));
extern uintptr_t bionic_stack_chk_guard;
extern int bionic_dso_handle;

// ctype_bionic.c – bionic ctype-táblák; a játék a mutatókat olvassa (extern const char *_ctype_)
extern const char *BIONIC_ctype_;
extern const short *BIONIC_tolower_tab_;
extern const short *BIONIC_toupper_tab_;
