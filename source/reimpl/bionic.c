/*
 * bionic.c – az Android libc (bionic, armeabi-v7a) ABI-jának pótlása newlib fölött
 *
 * Csak azok a függvények vannak itt, ahol a bionic és a newlib ABI-ja eltér (típusméret,
 * struktúra-elrendezés, konstansok), vagy ahol a Switchen mást kell tenni (útvonalak, hálózat).
 * A többit a dynlib.c egyenesen a newlibre köti. Az errno newlib-értékű marad (1–34 egyezik a
 * Linuxszal); a játék csak a Lua strerror(errno)-jában nézi, az pedig szintén a newlibé.
 *
 * Részben a backstab-vita reimpl/io.c, pthr.c, mem.c, sys.c alapján:
 * Copyright (C) 2021 Andy Nguyen, (C) 2022 Rinnegatamante, (C) 2022-2023 Volodymyr Atamanenko
 * Copyright (C) 2026 Levi (Switch port), MIT license.
 */
#include "reimpl/bionic.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <malloc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <switch.h>

#include "game.h"
#include "util.h"

#define PATH_BUF 512

// A hívó címe a játékmodulon belüli eltolásként (diagnosztikához).
#define CALLER_OFFSET() ((unsigned)((uintptr_t)__builtin_return_address(0) - so_mod.base))

// ---------------------------------------------------------------------------------------------
// ABI-ellenőrzések: ha a newlib vagy a fordító beállítása elmozdul, itt álljon meg a fordítás.

_Static_assert(sizeof(struct stat_bionic) == 104, "bionic struct stat: 104 bájt");
_Static_assert(offsetof(struct stat_bionic, st_mode) == 16, "st_mode @16");
_Static_assert(offsetof(struct stat_bionic, st_size) == 48, "st_size @48");
_Static_assert(offsetof(struct stat_bionic, st_blocks) == 64, "st_blocks @64");
_Static_assert(offsetof(struct stat_bionic, st_ino) == 96, "st_ino @96");
_Static_assert(sizeof(struct tm_bionic) == 44, "bionic struct tm: 44 bájt");
_Static_assert(sizeof(bionic_pthread_attr_t) == 24, "bionic pthread_attr_t: 24 bájt");
_Static_assert(sizeof(bionic_mutex_t) == 4, "bionic pthread_mutex_t: 4 bájt");
_Static_assert(sizeof(pthread_t) == 4 && sizeof(pthread_key_t) == 4, "pthread_t/key_t: 4 bájt");
_Static_assert(sizeof(wchar_t) == 4 && sizeof(long) == 4, "wchar_t, long: 4 bájt");
// A játék inline getc/ferror-ja: _p @0, _r @4, _flags @12 (short), __SERR 0x40, __SEOF 0x20.
_Static_assert(offsetof(FILE, _p) == 0 && offsetof(FILE, _r) == 4, "FILE._p/_r");
_Static_assert(offsetof(FILE, _flags) == 12 && sizeof(((FILE *)0)->_flags) == 2, "FILE._flags");
_Static_assert(__SERR == 0x40 && __SEOF == 0x20, "__SERR/__SEOF");
// open/fcntl/lseek: a SEEK_* és O_ACCMODE egyezik, a többi flaget fordítjuk
_Static_assert(SEEK_SET == 0 && SEEK_CUR == 1 && SEEK_END == 2, "SEEK_*");
_Static_assert(O_RDONLY == 0 && O_WRONLY == 1 && O_RDWR == 2, "O_ACCMODE");
_Static_assert(_IOFBF == 0 && _IOLBF == 1 && _IONBF == 2, "setvbuf-módok");
_Static_assert(S_IFMT == 0170000 && S_IFREG == 0100000 && S_IFDIR == 0040000, "S_IF*");

// ---------------------------------------------------------------------------------------------
// Előkészítés

uintptr_t bionic_stack_chk_guard = 0;
int bionic_dso_handle = 0;
static uint64_t g_clock_base_ns;

void bionic_init(void) {
    uintptr_t guard = 0;
    randomGet(&guard, sizeof(guard));
    if (guard == 0)
        guard = (uintptr_t)armGetSystemTick() | 1;
    // csak a játék init_array-ja előtt szabad beállítani (futó keretek a régi értéket várnák)
    bionic_stack_chk_guard = guard;
    g_clock_base_ns = armTicksToNs(armGetSystemTick());
}

// ---------------------------------------------------------------------------------------------
// Elérési utak

static int has_prefix(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

const char *bionic_path(const char *path, char *buf, size_t size) {
    if (!path)
        return NULL;

    // "eszköz:/..." (sdmc:, romfs:) – már Switch-út
    const char *colon = strchr(path, ':');
    const char *slash = strchr(path, '/');
    if (colon && (!slash || colon < slash))
        return path;

    const char *p = path;
    if (has_prefix(p, "/mnt/sdcard"))
        p += 4;

    const char *base, *rest;
    if (has_prefix(p, "/sdcard/Android/data")) {
        base = DATA_PATH;
        rest = p + strlen("/sdcard/Android/data");
    } else if (has_prefix(p, "/data/data/")) {
        base = DATA_PATH;
        rest = p + strlen("/data/data/");
    } else if (has_prefix(p, "/sdcard")) {
        base = GAME_FILES;
        rest = p + strlen("/sdcard");
    } else {
        base = GAME_FILES;
        rest = p;
    }
    while (*rest == '/')
        rest++;

    snprintf(buf, size, "%s%s", base, rest);
    log_debug("[io] útvonal: %s -> %s", path, buf);
    return buf;
}

// ---------------------------------------------------------------------------------------------
// stdio

uint8_t bionic_sF[3 * BIONIC_FILE_SIZE] __attribute__((aligned(8)));

FILE *bionic_fix_fp(FILE *fp) {
    uintptr_t a = (uintptr_t)fp, base = (uintptr_t)bionic_sF;
    if (a >= base && a < base + sizeof(bionic_sF)) {
        switch ((a - base) / BIONIC_FILE_SIZE) {
        case 0: return stdin;
        case 1: return stdout;
        default: return stderr;
        }
    }
    return fp;
}

static int mode_creates(const char *mode) {
    return mode && (strchr(mode, 'w') || strchr(mode, 'a'));
}

FILE *bionic_fopen(const char *path, const char *mode) {
    char buf[PATH_BUF];
    const char *p = bionic_path(path, buf, sizeof(buf));
    uint64_t t = perf_tick();
    FILE *f = fopen(p, mode);
    if (!f && errno == ENOENT && mode_creates(mode)) {
        mkpath(p);
        f = fopen(p, mode);
    }
    perf.io_ticks += perf_tick() - t;
    perf.opens++;
    log_debug("[io] fopen(%s, %s): %p", p, mode, (void *)f);
    return f;
}

FILE *bionic_freopen(const char *path, const char *mode, FILE *fp) {
    char buf[PATH_BUF];
    const char *p = path ? bionic_path(path, buf, sizeof(buf)) : NULL;
    FILE *real = bionic_fix_fp(fp);
    FILE *f = freopen(p, mode, real);
    log_debug("[io] freopen(%s, %s): %p", p ? p : "(null)", mode, (void *)f);
    // a játék a saját mutatóját kapja vissza (pl. &__sF[1])
    return (f && f == real) ? fp : f;
}

int bionic_fclose(FILE *fp) {
    return fclose(bionic_fix_fp(fp));
}

int bionic_fflush(FILE *fp) {
    return fflush(fp ? bionic_fix_fp(fp) : NULL);
}

int bionic_ferror(FILE *fp) {
    return ferror(bionic_fix_fp(fp));
}

int bionic_setvbuf(FILE *fp, char *buf, int mode, size_t size) {
    return setvbuf(bionic_fix_fp(fp), buf, mode, size);
}

size_t bionic_fread(void *ptr, size_t size, size_t n, FILE *fp) {
    uint64_t t = perf_tick();
    size_t r = fread(ptr, size, n, bionic_fix_fp(fp));
    perf.io_ticks += perf_tick() - t;
    perf.io_bytes += (uint64_t)r * size;
    perf.reads++;
    return r;
}

size_t bionic_fwrite(const void *ptr, size_t size, size_t n, FILE *fp) {
    return fwrite(ptr, size, n, bionic_fix_fp(fp));
}

int bionic_fseek(FILE *fp, long off, int whence) {
    uint64_t t = perf_tick();
    int r = fseek(bionic_fix_fp(fp), off, whence);
    perf.io_ticks += perf_tick() - t;
    return r;
}

long bionic_ftell(FILE *fp) {
    return ftell(bionic_fix_fp(fp));
}

int bionic_fgetpos(FILE *fp, bionic_fpos_t *pos) {
    fpos_t p;
    int r = fgetpos(bionic_fix_fp(fp), &p);
    if (r == 0) {
        if (p > INT32_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
        *pos = (bionic_fpos_t)p;
    }
    return r;
}

int bionic_fsetpos(FILE *fp, const bionic_fpos_t *pos) {
    fpos_t p = *pos;
    return fsetpos(bionic_fix_fp(fp), &p);
}

char *bionic_fgets(char *s, int n, FILE *fp) {
    return fgets(s, n, bionic_fix_fp(fp));
}

// a newlib getc/putc makró (többször értékelné ki az argumentumot) – a függvényváltozat kell
int bionic_getc(FILE *fp) {
    return fgetc(bionic_fix_fp(fp));
}

int bionic_ungetc(int c, FILE *fp) {
    return ungetc(c, bionic_fix_fp(fp));
}

// A játék inline getc-je (--fp->_r < 0 ? __srget(fp) : *fp->_p++) ezt hívja, ha kiürült a puffer.
int bionic_srget(FILE *fp) {
    return __srget_r(_REENT, bionic_fix_fp(fp));
}

int bionic_fputc(int c, FILE *fp) {
    return fputc(c, bionic_fix_fp(fp));
}

int bionic_putc(int c, FILE *fp) {
    return fputc(c, bionic_fix_fp(fp));
}

int bionic_fputs(const char *s, FILE *fp) {
    return fputs(s, bionic_fix_fp(fp));
}

int bionic_fprintf(FILE *fp, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(bionic_fix_fp(fp), fmt, ap);
    va_end(ap);
    return r;
}

int bionic_fscanf(FILE *fp, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfscanf(bionic_fix_fp(fp), fmt, ap);
    va_end(ap);
    return r;
}

char *bionic_tmpnam(char *s) {
    static char name[PATH_BUF];
    static unsigned counter = 0;
    char *out = s ? s : name;
    // bionic L_tmpnam = 1024, a hívó puffere elég nagy
    snprintf(out, PATH_BUF, DATA_PATH "tmp/tmp%u_%u", (unsigned)(time_ms() % 100000),
             __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED));
    mkpath(out);
    return out;
}

FILE *bionic_tmpfile(void) {
    char name[PATH_BUF];
    bionic_tmpnam(name);
    FILE *f = fopen(name, "w+b");
    log_debug("[io] tmpfile: %s: %p", name, (void *)f);
    return f;
}

// ---------------------------------------------------------------------------------------------
// Fájlleírók: open-flagek (Linux ARM) → newlib

#define LINUX_O_ACCMODE  0x003
#define LINUX_O_CREAT    0x040
#define LINUX_O_EXCL     0x080
#define LINUX_O_TRUNC    0x200
#define LINUX_O_APPEND   0x400
#define LINUX_O_NONBLOCK 0x800

static int oflags_from_linux(int f) {
    int out = f & LINUX_O_ACCMODE;
    if (f & LINUX_O_CREAT)    out |= O_CREAT;
    if (f & LINUX_O_EXCL)     out |= O_EXCL;
    if (f & LINUX_O_TRUNC)    out |= O_TRUNC;
    if (f & LINUX_O_APPEND)   out |= O_APPEND;
    if (f & LINUX_O_NONBLOCK) out |= O_NONBLOCK;
    return out;
}

int bionic_open(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & LINUX_O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    char buf[PATH_BUF];
    const char *p = bionic_path(path, buf, sizeof(buf));
    int nflags = oflags_from_linux(flags);
    int fd = open(p, nflags, mode);
    if (fd < 0 && errno == ENOENT && (nflags & O_CREAT)) {
        mkpath(p);
        fd = open(p, nflags, mode);
    }
    log_debug("[io] open(%s, 0x%x -> 0x%x): %d", p, flags, nflags, fd);
    return fd;
}

// bionic: off_t lseek(int, off_t, int) 32 bites off_t-vel (r0, r1, r2) – a newlibé 64 bites.
int32_t bionic_lseek(int fd, int32_t off, int whence) {
    off_t r = lseek(fd, (off_t)off, whence);
    if (r > INT32_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return (int32_t)r;
}

int bionic_fcntl(int fd, int cmd, ...) {
    // A játékban csak az STLport _Filebuf_base::_M_open(int fd) hívja F_GETFL-lel (Linux: 3),
    // a hozzáférési módot nézi. A devoptab-fájlokra nincs F_GETFL → írható-olvashatónak mondjuk.
    switch (cmd) {
    case 3: // F_GETFL
        return 2; // O_RDWR
    default:
        log_debug("[io] fcntl(%d, %d): figyelmen kívül", fd, cmd);
        return 0;
    }
}

int bionic_fstat(int fd, struct stat_bionic *out) {
    struct stat st;
    int r = fstat(fd, &st);
    if (r == 0) {
        memset(out, 0, sizeof(*out));
        out->st_dev = st.st_dev;
        out->__st_ino = (unsigned long)st.st_ino;
        out->st_ino = st.st_ino;
        out->st_mode = st.st_mode;
        out->st_nlink = st.st_nlink;
        out->st_uid = st.st_uid;
        out->st_gid = st.st_gid;
        out->st_rdev = st.st_rdev;
        out->st_size = st.st_size;
        out->st_blksize = st.st_blksize;
        out->st_blocks = st.st_blocks;
        out->st_atime_sec = (unsigned long)st.st_atime;
        out->st_mtime_sec = (unsigned long)st.st_mtime;
        out->st_ctime_sec = (unsigned long)st.st_ctime;
    }
    log_debug("[io] fstat(%d): %d, méret %lld", fd, r, r == 0 ? (long long)st.st_size : -1LL);
    return r;
}

int bionic_remove(const char *path) {
    char buf[PATH_BUF];
    return remove(bionic_path(path, buf, sizeof(buf)));
}

int bionic_rename(const char *from, const char *to) {
    char b1[PATH_BUF], b2[PATH_BUF];
    return rename(bionic_path(from, b1, sizeof(b1)), bionic_path(to, b2, sizeof(b2)));
}

int bionic_unlink(const char *path) {
    char buf[PATH_BUF];
    return unlink(bionic_path(path, buf, sizeof(buf)));
}

int bionic_chdir(const char *path) {
    char buf[PATH_BUF];
    const char *p = bionic_path(path, buf, sizeof(buf));
    int r = chdir(p);
    log_debug("[io] chdir(%s): %d", p, r);
    return r;
}

// ---------------------------------------------------------------------------------------------
// Idő: bionicban time_t 32 bites, struct tm 44 bájt (tm_gmtoff, tm_zone), CLOCKS_PER_SEC 10^6

static struct tm_bionic *tm_to_bionic(const struct tm *src, struct tm_bionic *dst, const char *zone) {
    dst->tm_sec = src->tm_sec;
    dst->tm_min = src->tm_min;
    dst->tm_hour = src->tm_hour;
    dst->tm_mday = src->tm_mday;
    dst->tm_mon = src->tm_mon;
    dst->tm_year = src->tm_year;
    dst->tm_wday = src->tm_wday;
    dst->tm_yday = src->tm_yday;
    dst->tm_isdst = src->tm_isdst;
    dst->tm_gmtoff = 0;
    dst->tm_zone = zone;
    return dst;
}

static struct tm tm_from_bionic(const struct tm_bionic *src) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_sec = src->tm_sec;
    t.tm_min = src->tm_min;
    t.tm_hour = src->tm_hour;
    t.tm_mday = src->tm_mday;
    t.tm_mon = src->tm_mon;
    t.tm_year = src->tm_year;
    t.tm_wday = src->tm_wday;
    t.tm_yday = src->tm_yday;
    t.tm_isdst = src->tm_isdst;
    return t;
}

time32_t bionic_time(time32_t *t) {
    time32_t now = (time32_t)time(NULL);
    if (t)
        *t = now;
    return now;
}

int bionic_gettimeofday(struct timeval32 *tv, void *tz) {
    struct timeval now;
    int r = gettimeofday(&now, NULL);
    if (r == 0 && tv) {
        tv->tv_sec = (int32_t)now.tv_sec;
        tv->tv_usec = (int32_t)now.tv_usec;
    }
    if (tz)
        memset(tz, 0, 8); // struct timezone { int tz_minuteswest, tz_dsttime; }
    return r;
}

struct tm_bionic *bionic_localtime(const time32_t *t) {
    static struct tm_bionic out;
    struct tm tmp;
    time_t tt = *t;
    if (!localtime_r(&tt, &tmp))
        return NULL;
    return tm_to_bionic(&tmp, &out, "UTC");
}

struct tm_bionic *bionic_gmtime(const time32_t *t) {
    static struct tm_bionic out;
    struct tm tmp;
    time_t tt = *t;
    if (!gmtime_r(&tt, &tmp))
        return NULL;
    return tm_to_bionic(&tmp, &out, "GMT");
}

time32_t bionic_mktime(struct tm_bionic *tm) {
    struct tm t = tm_from_bionic(tm);
    time_t r = mktime(&t);
    // a mktime normalizálja a mezőket – vissza a játék struktúrájába
    tm_to_bionic(&t, tm, tm->tm_zone);
    return (time32_t)r;
}

double bionic_difftime(time32_t a, time32_t b) {
    return (double)a - (double)b;
}

size_t bionic_strftime(char *s, size_t max, const char *fmt, const struct tm_bionic *tm) {
    struct tm t = tm_from_bionic(tm);
    return strftime(s, max, fmt, &t);
}

long bionic_clock(void) {
    // processzoridő helyett a folyamat indulása óta eltelt idő, mikroszekundumban
    return (long)((armTicksToNs(armGetSystemTick()) - g_clock_base_ns) / 1000ULL);
}

int bionic_nanosleep(const struct timespec32 *req, struct timespec32 *rem) {
    if (!req || req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec > 999999999) {
        errno = EINVAL;
        return -1;
    }
    struct timespec ts = { .tv_sec = req->tv_sec, .tv_nsec = req->tv_nsec };
    int r = nanosleep(&ts, NULL);
    if (rem)
        rem->tv_sec = rem->tv_nsec = 0;
    return r;
}

// ---------------------------------------------------------------------------------------------
// pthread: a bionic mutex 4 bájt (statikus inicializálók: 0 normál, 0x4000 rekurzív,
// 0x8000 hibaellenőrző; 0x2000 = pshared bit). A newlib-mutex (16 bájt) lustán, első
// használatkor jön létre, a címe a játék 4 bájtos helyére kerül (atomi CAS, zár nélkül).

#define BIONIC_MUTEX_TYPE_MASK   0xc000
#define BIONIC_MUTEX_RECURSIVE   0x4000
#define BIONIC_MUTEX_ERRORCHECK  0x8000
#define BIONIC_MUTEXATTR_TYPE_MASK 0x000f  // pthread_mutexattr_t: 0 normál, 1 rekurzív, 2 hibaell.

static inline int mutex_is_ptr(uintptr_t v) {
    return v > 0xffff;
}

static pthread_mutex_t *mutex_new(int recursive) {
    pthread_mutex_t *m = malloc(sizeof(*m));
    if (!m)
        fatal_error("pthread_mutex: elfogyott a memória");
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    // a devkitPro-pthread csak normál és rekurzív típust ismer; a hibaellenőrző normál lesz
    pthread_mutexattr_settype(&a, recursive ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_NORMAL);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    return m;
}

static void mutex_free(pthread_mutex_t *m) {
    pthread_mutex_destroy(m);
    free(m);
}

// A játék helyén álló newlib-mutex; ha még inicializáló van ott, most jön létre.
static pthread_mutex_t *mutex_get(bionic_mutex_t *bm) {
    uintptr_t v = __atomic_load_n(&bm->value, __ATOMIC_ACQUIRE);
    while (!mutex_is_ptr(v)) {
        pthread_mutex_t *m = mutex_new((v & BIONIC_MUTEX_TYPE_MASK) == BIONIC_MUTEX_RECURSIVE);
        if (__atomic_compare_exchange_n(&bm->value, &v, (uintptr_t)m, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return m;
        // egy másik szál megelőzött: v most az ő értéke
        mutex_free(m);
    }
    return (pthread_mutex_t *)v;
}

int bionic_pthread_mutex_init(bionic_mutex_t *bm, const bionic_mutexattr_t *attr) {
    int type = attr ? (int)(*attr & BIONIC_MUTEXATTR_TYPE_MASK) : 0;
    // nem nézzük a régi tartalmat: a játék inicializálatlan memórián is hívhatja
    __atomic_store_n(&bm->value, (uintptr_t)mutex_new(type == 1), __ATOMIC_RELEASE);
    return 0;
}

int bionic_pthread_mutex_destroy(bionic_mutex_t *bm) {
    uintptr_t v = __atomic_exchange_n(&bm->value, 0, __ATOMIC_ACQ_REL);
    if (mutex_is_ptr(v))
        mutex_free((pthread_mutex_t *)v);
    return 0;
}

int bionic_pthread_mutex_lock(bionic_mutex_t *bm) {
    return pthread_mutex_lock(mutex_get(bm));
}

int bionic_pthread_mutex_trylock(bionic_mutex_t *bm) {
    return pthread_mutex_trylock(mutex_get(bm));
}

int bionic_pthread_mutex_unlock(bionic_mutex_t *bm) {
    uintptr_t v = __atomic_load_n(&bm->value, __ATOMIC_ACQUIRE);
    if (!mutex_is_ptr(v))
        return 0; // sosem zárták le
    return pthread_mutex_unlock((pthread_mutex_t *)v);
}

#define BIONIC_ATTR_FLAG_DETACHED 0x1
#define THREAD_STACK_DEFAULT (1024 * 1024)  // az Androidé is ~1 MB
#define THREAD_STACK_MIN     (256 * 1024)

int bionic_pthread_create(bionic_pthread_t *thread, const bionic_pthread_attr_t *battr,
                          void *(*start)(void *), void *arg) {
    size_t stack = THREAD_STACK_DEFAULT;
    int detached = 0;
    if (battr) {
        if (battr->stack_size)
            stack = battr->stack_size;
        detached = battr->flags & BIONIC_ATTR_FLAG_DETACHED;
        if (battr->stack_base)
            log_error("pthread_create: a játék saját vermet adna (%p), helyette sajátot foglalunk",
                      battr->stack_base);
    }
    if (stack < THREAD_STACK_MIN)
        stack = THREAD_STACK_MIN;
    stack = (stack + 0xfff) & ~(size_t)0xfff;

    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack);
    if (detached)
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    int r = pthread_create(&t, &a, start, arg);
    pthread_attr_destroy(&a);
    if (r == 0 && thread)
        *thread = (bionic_pthread_t)t;
    log_info("pthread_create(belépés +0x%x, verem %u KB%s): %d",
             (unsigned)((uintptr_t)start - so_mod.base), (unsigned)(stack / 1024),
             detached ? ", leválasztott" : "", r);
    return r;
}

// ---------------------------------------------------------------------------------------------
// mmap/munmap: névtelen leképezés = nullázott, laphatárra igazított heap-blokk; fájlos leképezés
// (STLport _Filebuf_base::_M_mmap, PROT_READ|MAP_PRIVATE) = a fájlrész beolvasva. A táblázat
// csak azt tartja nyilván, mit kell a munmap-nak felszabadítania.

#define LINUX_PROT_WRITE    0x2
#define LINUX_MAP_SHARED    0x01
#define LINUX_MAP_FIXED     0x10
#define LINUX_MAP_ANONYMOUS 0x20
#define BIONIC_MAP_FAILED   ((void *)-1)
#define MMAP_SLOTS 128

static struct { void *addr; size_t len; } g_maps[MMAP_SLOTS];
static Mutex g_maps_mutex;

static int read_at(int fd, void *dst, size_t len, int32_t off) {
    off_t saved = lseek(fd, 0, SEEK_CUR);
    if (saved < 0 || lseek(fd, off, SEEK_SET) < 0)
        return -1;
    size_t done = 0;
    while (done < len) {
        ssize_t n = read(fd, (uint8_t *)dst + done, len - done);
        if (n < 0) {
            lseek(fd, saved, SEEK_SET);
            return -1;
        }
        if (n == 0)
            break; // fájl vége: a maradék nulla marad
        done += n;
    }
    lseek(fd, saved, SEEK_SET); // a valódi mmap sem mozgatja a fájlpozíciót
    return 0;
}

void *bionic_mmap(void *addr, size_t len, int prot, int flags, int fd, int32_t off) {
    if (len == 0 || (flags & LINUX_MAP_FIXED)) {
        log_error("mmap(%p, %u, flags 0x%x): nem támogatott", addr, (unsigned)len, flags);
        errno = EINVAL;
        return BIONIC_MAP_FAILED;
    }
    size_t alen = (len + 0xfff) & ~(size_t)0xfff;
    void *p = memalign(0x1000, alen);
    if (!p) {
        errno = ENOMEM;
        return BIONIC_MAP_FAILED;
    }
    memset(p, 0, alen);
    if (!(flags & LINUX_MAP_ANONYMOUS)) {
        if (read_at(fd, p, len, off) < 0) {
            log_error("mmap(fd %d, %u bájt @%d): olvasási hiba", fd, (unsigned)len, (int)off);
            free(p);
            errno = EACCES;
            return BIONIC_MAP_FAILED;
        }
        if ((prot & LINUX_PROT_WRITE) && (flags & LINUX_MAP_SHARED))
            log_error("mmap: írható megosztott fájl-leképezés – a módosítás nem kerül a fájlba");
    }

    mutexLock(&g_maps_mutex);
    int slot = -1;
    for (int i = 0; i < MMAP_SLOTS; i++) {
        if (!g_maps[i].addr) {
            g_maps[i].addr = p;
            g_maps[i].len = alen;
            slot = i;
            break;
        }
    }
    mutexUnlock(&g_maps_mutex);
    if (slot < 0)
        log_error("mmap: betelt a leképezés-táblázat, a munmap nem fogja felszabadítani: %p", p);
    log_debug("[mem] mmap(%u, prot %d, flags 0x%x, fd %d, off %d): %p",
              (unsigned)len, prot, flags, fd, (int)off, p);
    return p;
}

int bionic_munmap(void *addr, size_t len) {
    mutexLock(&g_maps_mutex);
    int found = 0;
    for (int i = 0; i < MMAP_SLOTS; i++) {
        if (g_maps[i].addr == addr) {
            g_maps[i].addr = NULL;
            found = 1;
            break;
        }
    }
    mutexUnlock(&g_maps_mutex);
    if (found)
        free(addr);
    else
        log_debug("[mem] munmap(%p, %u): ismeretlen leképezés", addr, (unsigned)len);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Hálózat: offline port – minden hívás tisztán hibát ad (a játék a Dragnet/Gameloft-szolgáltatásokat
// próbálná elérni; a socket() hibája után a többi nem is hívódik).

#define BIONIC_EAI_FAIL 4

int bionic_socket(int domain, int type, int protocol) {
    log_debug("[net] socket(%d, %d, %d): nincs hálózat", domain, type, protocol);
    errno = EAFNOSUPPORT;
    return -1;
}

int bionic_bind(int fd, const void *addr, unsigned len) {
    errno = EBADF;
    return -1;
}

int bionic_setsockopt(int fd, int level, int name, const void *val, unsigned len) {
    errno = EBADF;
    return -1;
}

int bionic_recvfrom(int fd, void *buf, size_t len, int flags, void *addr, unsigned *alen) {
    errno = EBADF;
    return -1;
}

int bionic_sendto(int fd, const void *buf, size_t len, int flags, const void *addr, unsigned alen) {
    errno = EBADF;
    return -1;
}

// bionic fd_set: 1024 bit. Nincs olyan leíró, ami kész lehetne → a halmazok kiürülnek, és a
// select a megadott ideig vár (időzítőként is használható).
int bionic_select(int nfds, void *rd, void *wr, void *ex, struct timeval32 *timeout) {
    if (nfds < 0 || nfds > 1024) {
        errno = EINVAL;
        return -1;
    }
    size_t bytes = ((size_t)nfds + 31) / 32 * 4;
    if (rd) memset(rd, 0, bytes);
    if (wr) memset(wr, 0, bytes);
    if (ex) memset(ex, 0, bytes);
    if (timeout) {
        int64_t ns = (int64_t)timeout->tv_sec * 1000000000LL + (int64_t)timeout->tv_usec * 1000LL;
        if (ns > 0)
            svcSleepThread(ns);
        timeout->tv_sec = timeout->tv_usec = 0;
    }
    return 0;
}

int bionic_getaddrinfo(const char *node, const char *service, const void *hints, void **res) {
    log_debug("[net] getaddrinfo(%s): nincs hálózat", node ? node : "(null)");
    if (res)
        *res = NULL;
    return BIONIC_EAI_FAIL;
}

void bionic_freeaddrinfo(void *res) {
}

int bionic_gethostname(char *name, size_t len) {
    if (!name || len == 0) {
        errno = EINVAL;
        return -1;
    }
    snprintf(name, len, "switch");
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Egyéb

int bionic_android_log_print(int prio, const char *tag, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    size_t n = strlen(msg);
    while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r'))
        msg[--n] = 0;

    // Android: 2 VERBOSE, 3 DEBUG, 4 INFO, 5 WARN, 6 ERROR, 7 FATAL
    if (prio >= 6)
        log_printf("E", "[%s] %s", tag ? tag : "", msg);
    else if (prio == 5)
        log_printf("W", "[%s] %s", tag ? tag : "", msg);
    else
        log_debug("[%s] %s", tag ? tag : "", msg);
    return (int)n;
}

char *bionic_getenv(const char *name) {
    log_debug("getenv(%s): NULL", name ? name : "(null)");
    return NULL;
}

int bionic_system(const char *cmd) {
    if (!cmd)
        return 0; // nincs parancsértelmező
    log_debug("system(%s): nem támogatott", cmd);
    errno = ENOSYS;
    return -1;
}

// bionic _SC_* számok (libc/include/sys/sysconf.h)
#define BIONIC_SC_CLK_TCK           0x0006
#define BIONIC_SC_OPEN_MAX          0x000b
#define BIONIC_SC_PAGESIZE          0x0027
#define BIONIC_SC_PAGE_SIZE         0x0028
#define BIONIC_SC_NPROCESSORS_CONF  0x0060
#define BIONIC_SC_NPROCESSORS_ONLN  0x0061

long bionic_sysconf(int name) {
    switch (name) {
    case BIONIC_SC_CLK_TCK:          return 100;
    case BIONIC_SC_OPEN_MAX:         return 1024;
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE:        return 4096;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN: return 3; // a 4. mag a rendszeré
    default:
        log_debug("sysconf(0x%x): ismeretlen", name);
        errno = EINVAL;
        return -1;
    }
}

char *bionic_setlocale(int category, const char *locale) {
    static char c_locale[] = "C";
    return c_locale;
}

void bionic_abort(void) {
    fatal_error("A játék abort()-ot hívott (hívó: libPirates.so+0x%x).", CALLER_OFFSET());
}

void bionic_exit(int code) {
    log_info("A játék exit(%d)-et hívott (hívó: libPirates.so+0x%x).", code, CALLER_OFFSET());
    exit(code);
}

// A játék statikus objektumainak destruktorait nem futtatjuk kilépéskor (a GL-környezet és a
// hang addigra már nem él) – csak sikert jelzünk.
int bionic_aeabi_atexit(void *obj, void (*dtor)(void *), void *dso) {
    return 0;
}

void bionic_cxa_pure_virtual(void) {
    fatal_error("Tisztán virtuális függvény hívása (hívó: libPirates.so+0x%x).", CALLER_OFFSET());
}

void bionic_stack_chk_fail(void) {
    fatal_error("Veremsérülés a játékban (__stack_chk_fail, hívó: libPirates.so+0x%x).",
                CALLER_OFFSET());
}
