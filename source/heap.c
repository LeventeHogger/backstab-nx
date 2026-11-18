/*
 * heap.c – nagy foglalások külön régióban (a heap feldarabolódása ellen)
 *
 * A newlib malloc egyetlen sbrk-arénából dolgozik. A játék pályánként több száz MB-ot foglal és szabadít
 * fel (textúrák, hálók, fájlpufferek – a Mesa GPU-pufferei is a heapből jönnek); a nagy blokkok helyét a
 * közben megmaradt apró foglalások szétdarabolják, ezért az aréna minden pályaváltással nő (mérve:
 * 743 → 1309 MB hat váltás alatt, miközben a ténylegesen foglalt 490–670 MB maradt), és néhány váltás
 * után egy nagy foglalás nem fér el (a játék memset-je NULL-ra írt). Androidon a bionic allokátora a nagy
 * blokkokat mmap-pel külön kezeli – itt ezt pótoljuk:
 *
 *  - a LARGE_MIN-nél nagyobb kérések a heap tetejéről LEFELÉ növő régióba kerülnek (best-fit, lapnyi
 *    szemcse, a szomszédos szabad blokkok összeolvadnak, a legalsó szabad blokk azonnal visszajár);
 *  - a kicsik maradnak a newlib-nél, alulról felfelé; a határ a fake_heap_end, amit a libsysbase sbrk-je
 *    minden hívásnál újraolvas – a newlib így nem nőhet a nagy régióba.
 *
 * A newlib _malloc_r/_free_r/… függvényeit a linker --wrap-pel ide tereli (Makefile), így a játék, a
 * Mesa, a libnx és a libstdc++ foglalásai mind átmennek rajta. Zárolás: a newlib saját (rekurzív)
 * malloc-zára.
 *
 * Copyright (C) 2026 Levi (Switch port), MIT license.
 */
#include <errno.h>
#include <malloc.h>
#include <reent.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "port.h"

#define LARGE_MIN (128 * 1024)
#define PAGE      0x1000u
#define LMAGIC    0x4c424c4bu   // blokkfejléc
#define UMAGIC    0x55534552u   // a felhasználói mutató előtti 8 bájt

typedef struct LBlock {
    uint32_t size;              // a teljes blokk (fejléccel), PAGE többszöröse
    struct LBlock *prev;        // a fizikailag előző (alacsonyabb című) blokk; a legalsóé NULL
    uint32_t used;
    uint32_t magic;
    uint32_t pad[4];
} LBlock;                       // 32 bájt

extern char *fake_heap_end;
extern void *_sbrk_r(struct _reent *r, ptrdiff_t incr);
extern void __malloc_lock(struct _reent *r);
extern void __malloc_unlock(struct _reent *r);

void *__real__malloc_r(struct _reent *r, size_t n);
void __real__free_r(struct _reent *r, void *p);
void *__real__realloc_r(struct _reent *r, void *p, size_t n);
void *__real__calloc_r(struct _reent *r, size_t n, size_t m);
void *__real__memalign_r(struct _reent *r, size_t align, size_t n);
size_t __real__malloc_usable_size_r(struct _reent *r, void *p);

static char *l_low, *l_high;    // a nagy régió: [l_low, l_high); üresen l_low == l_high
static size_t l_used;           // a foglalt nagy blokkok összmérete (statisztika)

static inline LBlock *next_of(LBlock *b) {
    char *n = (char *)b + b->size;
    return n < l_high ? (LBlock *)n : NULL;
}

static inline int in_large(const void *p) {
    return l_high && (const char *)p >= l_low && (const char *)p < l_high;
}

static LBlock *block_of(void *p) {
    const uint32_t *u = (const uint32_t *)p - 2;
    if (u[1] != UMAGIC)
        return NULL;
    LBlock *b = (LBlock *)((char *)p - u[0]);
    return b->magic == LMAGIC && b->used ? b : NULL;
}

static void *user_ptr(LBlock *b, size_t align) {
    uintptr_t p = (uintptr_t)b + sizeof(LBlock) + 8;
    p = (p + align - 1) & ~(uintptr_t)(align - 1);
    uint32_t *u = (uint32_t *)p - 2;
    u[0] = (uint32_t)(p - (uintptr_t)b);
    u[1] = UMAGIC;
    return (void *)p;
}

// a zár alatt: blokk a nagy régióból (NULL, ha nincs hely)
static void *large_alloc(struct _reent *r, size_t n, size_t align) {
    if (align < 8)
        align = 8;
    if (n > 0x7fff0000u - align)
        return NULL;
    size_t total = (sizeof(LBlock) + 8 + (align > 8 ? align : 0) + n + PAGE - 1) & ~(size_t)(PAGE - 1);
    if (!l_high)
        l_low = l_high = (char *)((uintptr_t)fake_heap_end & ~(uintptr_t)(PAGE - 1));

    LBlock *best = NULL;
    for (LBlock *b = l_low < l_high ? (LBlock *)l_low : NULL; b; b = next_of(b))
        if (!b->used && b->size >= total && (!best || b->size < best->size))
            best = b;

    if (!best) {
        // lefelé növünk; ha a legalsó blokk szabad, azt bővítjük
        LBlock *low = l_low < l_high ? (LBlock *)l_low : NULL;
        size_t have = low && !low->used ? low->size : 0;
        size_t grow = total - have;
        char *brk = _sbrk_r(r, 0);
        if (brk == (char *)-1 || brk > l_low || (size_t)(l_low - brk) < grow)
            return NULL;
        char *nl = l_low - grow;
        best = (LBlock *)nl;
        best->size = total;
        best->prev = NULL;
        best->used = 0;
        best->magic = LMAGIC;
        LBlock *above = have ? next_of(low) : low;   // a régi legalsó szabad blokk beleolvad (total = have + grow)
        if (have)
            low->magic = 0;
        if (above)
            above->prev = best;
        l_low = nl;
        fake_heap_end = l_low;
    }

    if (best->size - total >= PAGE) {
        // a felső részét adjuk ki; az alsó marad szabad (lent könnyebben visszajár)
        LBlock *nb = (LBlock *)((char *)best + best->size - total);
        LBlock *after = next_of(best);
        nb->size = total;
        nb->prev = best;
        nb->magic = LMAGIC;
        best->size -= total;
        if (after)
            after->prev = nb;
        best = nb;
    }
    best->used = 1;
    l_used += best->size;
    return user_ptr(best, align);
}

// a zár alatt
static void large_free(LBlock *b) {
    b->used = 0;
    l_used -= b->size;
    LBlock *n = next_of(b);
    if (n && !n->used) {
        b->size += n->size;
        n->magic = 0;
        LBlock *nn = next_of(b);
        if (nn)
            nn->prev = b;
    }
    LBlock *p = b->prev;
    if (p && !p->used) {
        p->size += b->size;
        b->magic = 0;
        LBlock *nn = next_of(p);
        if (nn)
            nn->prev = p;
        b = p;
    }
    if ((char *)b == l_low) {   // a legalsó szabad blokk visszajár a newlib-nek
        LBlock *nn = next_of(b);
        l_low += b->size;
        if (nn)
            nn->prev = NULL;
        b->magic = 0;
        fake_heap_end = l_low;
    }
}

static size_t large_usable(void *p, LBlock *b) {
    return b->size - (uint32_t)((char *)p - (char *)b);
}

void *__wrap__malloc_r(struct _reent *r, size_t n) {
    void *p = NULL;
    if (n >= LARGE_MIN) {
        __malloc_lock(r);
        p = large_alloc(r, n, 8);
        __malloc_unlock(r);
    }
    if (!p)
        p = __real__malloc_r(r, n);
    if (!p && n < LARGE_MIN) {   // a newlib nem nőhet tovább: lapnyi blokk a nagy régióból
        __malloc_lock(r);
        p = large_alloc(r, n, 8);
        __malloc_unlock(r);
    }
    if (!p)
        r->_errno = ENOMEM;
    return p;
}

void __wrap__free_r(struct _reent *r, void *p) {
    if (!p)
        return;
    if (in_large(p)) {
        __malloc_lock(r);
        LBlock *b = block_of(p);
        if (b)
            large_free(b);
        __malloc_unlock(r);
        return;
    }
    __real__free_r(r, p);
}

size_t __wrap__malloc_usable_size_r(struct _reent *r, void *p) {
    if (p && in_large(p)) {
        LBlock *b = block_of(p);
        return b ? large_usable(p, b) : 0;
    }
    return __real__malloc_usable_size_r(r, p);
}

void *__wrap__realloc_r(struct _reent *r, void *p, size_t n) {
    if (!p)
        return __wrap__malloc_r(r, n);
    if (n == 0) {
        __wrap__free_r(r, p);
        return NULL;
    }
    size_t old;
    if (in_large(p)) {
        LBlock *b = block_of(p);
        if (!b)
            return NULL;
        old = large_usable(p, b);
        if (n <= old && n >= old / 2)
            return p;
    } else {
        if (n < LARGE_MIN)
            return __real__realloc_r(r, p, n);
        old = __real__malloc_usable_size_r(r, p);
    }
    void *np = __wrap__malloc_r(r, n);
    if (!np)
        return NULL;
    memcpy(np, p, old < n ? old : n);
    __wrap__free_r(r, p);
    return np;
}

void *__wrap__calloc_r(struct _reent *r, size_t n, size_t m) {
    if (m && n > SIZE_MAX / m) {
        r->_errno = ENOMEM;
        return NULL;
    }
    size_t total = n * m;
    if (total < LARGE_MIN)
        return __real__calloc_r(r, n, m);
    void *p = __wrap__malloc_r(r, total);
    if (p)
        memset(p, 0, total);
    return p;
}

void *__wrap__memalign_r(struct _reent *r, size_t align, size_t n) {
    // a newlib memalign-je n + align + … bájtot kér a _malloc_r-től, és a kapott blokkot feldarabolja:
    // ami ebből nagy lenne, azt végig itt kell kezelni
    if (align & (align - 1))
        return NULL;
    if (n + align + 64 < LARGE_MIN) {
        void *p = __real__memalign_r(r, align, n);
        if (p)
            return p;
    }
    __malloc_lock(r);
    void *p = large_alloc(r, n, align);
    __malloc_unlock(r);
    if (!p)
        r->_errno = ENOMEM;
    return p;
}

void heap_stats(unsigned *region_mb, unsigned *used_mb) {
    *region_mb = (unsigned)((l_high - l_low) >> 20);
    *used_mb = (unsigned)(l_used >> 20);
}
