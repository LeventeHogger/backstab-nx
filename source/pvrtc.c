/*
 * pvrtc.c – a játék PVRTC-kibontásának gyors, bájtra azonos változata
 *
 * A nouveau nem ismeri a PVRTC-t, ezért a glitch-motor minden PVR-textúrát a libPirates.so PVRTCDecompress-ével
 * (0x425b60, a régi PowerVR SDK képpontonkénti Decompress-e) bont RGBA8888-ra – Edenben ez a pályatöltés ~87%-a.
 * Az eredeti minden képpontnál négyszer twiddle-öz ciklusban, oszt, memcmp-vel nézi, változott-e a 2×2-es
 * blokkszomszédság, és két függvényhívással interpolál. Itt ugyanez a számítás szomszédságonként: a négy blokkot
 * egyszer bontjuk ki, és a hozzá tartozó XB×4 képpontot (XB = 4, 2 bitesnél 8) egyben számoljuk. A szomszédság-
 * sorokat a hívó szál és két segédszál osztja szét (az 1. és a 2. magon; a játék szálai mind a 0.-n futnak).
 *
 * Minden lépés a gépi kódot követi, a régi SDK furcsaságaival együtt (docs/visszafejtes.md, Betöltés):
 *   - Unpack5554Colour: a kék csatorna kiegészítése (|= >> 3 / >> 4) mindig az A színt írja, a B-nél is;
 *   - a 2 bites interpolált módban nincs H/V-csak változat: a nem tárolt képpont a 4 szomszéd átlaga;
 *   - kerekítés: (A·8 + mod·(B−A)) >> 3, a 4 bites 2-es kód átlátszó („punch-through”).
 * A szomszédság egy képpontra az eredetiben ((x − XB/2) & (W−1)) / XB – 2-hatvány méretnél, ha a kép legalább
 * 2×2 blokk, ez pontosan a blokkrács (más méretnél a hívó az eredetit futtatja). Debugban a patch.c minden
 * textúrát mindkettővel kibont és bájtra összevet; induláskor véletlen blokkokon is (pvrtc_selftest).
 *
 * Copyright (C) 2026 Levi (Switch port), MIT license.
 */
#include <arm_neon.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "port.h"
#include "util.h"

typedef struct {
    uint32_t mod;   // PackedData[0]: modulációs bitek
    uint32_t col;   // PackedData[1]: B (felső 16 bit) és A szín, a 0. bit a modulációs mód
} Block;

typedef struct {
    const Block *src;
    uint8_t *out;
    int two_bit, xdim, ydim;
    int bxd, byd;           // a blokkrács mérete
    atomic_int next_row;    // a következő kiosztandó szomszédság-sor
} Job;

static const int rep0[4] = { 0, 3, 5, 8 };   // a játék táblái (0x681904, 0x681914)
static const int rep1[4] = { 0, 4, 4, 8 };

// TwiddleUV (0x425534): a blokk helye a Morton-sorrendben. A két koordináta bitjei nem fedik egymást, ezért
// twiddle(y, x) = twiddle(y, 0) | twiddle(0, x) – soronként és oszloponként egyszer számoljuk.
static uint32_t twiddle(uint32_t ysize, uint32_t xsize, uint32_t ypos, uint32_t xpos) {
    uint32_t min_dim, max_val;
    if (ysize < xsize) {
        min_dim = ysize;
        max_val = xpos;
    } else {
        min_dim = xsize;
        max_val = ypos;
    }
    uint32_t src = 1, dst = 1, tw = 0, shift = 0;
    while (src < min_dim) {
        if (ypos & src)
            tw |= dst;
        if (xpos & src)
            tw |= dst << 1;
        src <<= 1;
        dst <<= 2;
        shift++;
    }
    return tw | ((max_val >> shift) << (2 * shift));
}

// Unpack5554Colour: c[0] az A, c[1] a B szín (R, G, B, A; 5554-es pontosság), a gépi kód sorrendjében
static void unpack_colours(uint32_t col, int c[2][4]) {
    const uint32_t raw[2] = { col & 0xfffe, col >> 16 };
    for (int k = 0; k < 2; k++) {
        const uint32_t r = raw[k];
        if (r & 0x8000) {
            c[k][0] = (r >> 10) & 31;
            c[k][1] = (r >> 5) & 31;
            c[k][2] = r & 31;
            if (k == 0)
                c[0][2] |= c[0][2] >> 4;
            c[k][3] = 15;
        } else {
            c[k][0] = (r >> 7) & 0x1e;
            c[k][0] |= c[k][0] >> 4;
            c[k][1] = (r >> 3) & 0x1e;
            c[k][1] |= c[k][1] >> 4;
            c[k][2] = (r & 15) << 1;
            c[0][2] |= c[0][2] >> (k == 0 ? 3 : 4);   // az eredetiben is mindig a 0. szín
            c[k][3] = (r >> 11) & 14;
        }
    }
}

// Az eredeti UnpackModulations a 4 blokk modulációját egy 8×16-os ablakba írja (P bal felül, Q jobb felül, R bal
// alul, S jobb alul); ez az ablak (wy, wx) helyének értéke, ahogy ott állna. A 2 bites interpolált blokk csak a
// páros (x^y) helyeket tárolja – a többit az eredeti sem olvassa (a szomszédaik mindig párosak).
static inline __attribute__((always_inline)) int mod_val(const Block *const b[4], int two_bit, int wy, int wx) {
    const int xb = two_bit ? 8 : 4;
    const Block *blk = b[(wy >= 4 ? 2 : 0) + (wx >= xb ? 1 : 0)];
    const int ly = wy & 3, lx = wx & (xb - 1);
    if (!two_bit)
        return (blk->mod >> (2 * (ly * 4 + lx))) & 3;
    if (blk->col & 1)
        return (blk->mod >> (2 * (ly * 4 + (lx >> 1)))) & 3;
    return (blk->mod >> (ly * 8 + lx)) & 1 ? 3 : 0;
}

// GetModulationValue: a moduláció (0..8) és az átlátszóság (4 bites 2-es kód)
static inline __attribute__((always_inline)) int modulation(const Block *const b[4], int two_bit, int wy, int wx,
                                                            int *pt) {
    const int xb = two_bit ? 8 : 4;
    const Block *blk = b[(wy >= 4 ? 2 : 0) + (wx >= xb ? 1 : 0)];
    *pt = 0;
    if (!(blk->col & 1))
        return rep0[mod_val(b, two_bit, wy, wx)];
    if (two_bit) {
        if (((wx ^ wy) & 1) == 0)
            return rep0[mod_val(b, two_bit, wy, wx)];
        return (rep0[mod_val(b, two_bit, wy - 1, wx)] + rep0[mod_val(b, two_bit, wy + 1, wx)] +
                rep0[mod_val(b, two_bit, wy, wx - 1)] + rep0[mod_val(b, two_bit, wy, wx + 1)] + 2) >> 2;
    }
    const int v = mod_val(b, two_bit, wy, wx);
    *pt = v == 2;
    return rep1[v];
}

// Egy blokk két színe egy vektorban: A r, g, b, a | B r, g, b, a
static inline __attribute__((always_inline)) int16x8_t colour_vec(uint32_t col) {
    int c[2][4];
    unpack_colours(col, c);
    const int16_t v[8] = { (int16_t)c[0][0], (int16_t)c[0][1], (int16_t)c[0][2], (int16_t)c[0][3],
                           (int16_t)c[1][0], (int16_t)c[1][1], (int16_t)c[1][2], (int16_t)c[1][3] };
    return vld1q_s16(v);
}

// Egy szomszédság-sor: a ty. és a következő blokksor közti 4 képpontsor ((ty·4 + 2 + v) & (H−1)).
// InterpolateColours a két színre egyszerre (NEON, 8 × int16):
//   t1 = P·xb + u·(Q−P), t2 = R·xb + u·(S−R), t = t1·4 + v·(t2−t1) = t1·(4−v) + t2·v,
//   azaz soronként t(u) = [P·xb·(4−v) + R·xb·v] + u·[(Q−P)·(4−v) + (S−R)·v] – képpontonként egy összeadás;
//   utána csatornánként >> 1/2 (RGB) ill. 0/1 (alfa), majd += >> 5 (RGB) ill. >> 4 (alfa).
// Minden köztes érték 0..2040 (5 bites szín · 8 · 4, ill. A·8 + mod·(B−A)), így int16-ban pontos.
// A szomszédos szomszédságok két blokkot közösen használnak: a jobb oldaliak (Q, S) a következő bal oldaliak.
static inline __attribute__((always_inline)) void decode_row(const Job *j, int ty, const int two_bit) {
    const int xb = two_bit ? 8 : 4, half = xb / 2, lg = two_bit ? 3 : 2;
    const int xmask = j->xdim - 1;
    const int ty1 = (ty + 1) & (j->byd - 1);
    const uint32_t yt0 = twiddle(j->byd, j->bxd, ty, 0), yt1 = twiddle(j->byd, j->bxd, ty1, 0);
    uint8_t *rows[4];
    for (int v = 0; v < 4; v++)
        rows[v] = j->out + (size_t)((ty * 4 + 2 + v) & (j->ydim - 1)) * j->xdim * 4;
    const int16_t s1[8] = { -lg + 1, -lg + 1, -lg + 1, -lg + 2, -lg + 1, -lg + 1, -lg + 1, -lg + 2 };
    const int16_t s2[8] = { -5, -5, -5, -4, -5, -5, -5, -4 };
    const int16x8_t sh1 = vld1q_s16(s1), sh2 = vld1q_s16(s2);

    const uint32_t xt0 = twiddle(j->byd, j->bxd, 0, 0);
    const Block *bp = j->src + (yt0 | xt0), *br = j->src + (yt1 | xt0);
    int16x8_t cp = colour_vec(bp->col), cr = colour_vec(br->col);
    for (int tx = 0; tx < j->bxd; tx++) {
        const uint32_t xt1 = twiddle(j->byd, j->bxd, 0, (tx + 1) & (j->bxd - 1));
        const Block *bq = j->src + (yt0 | xt1), *bs = j->src + (yt1 | xt1);
        const Block *const b[4] = { bp, bq, br, bs };
        const int16x8_t cq = colour_vec(bq->col), cs = colour_vec(bs->col);
        const int16x8_t p1 = vshlq_n_s16(cp, two_bit ? 3 : 2), p2 = vshlq_n_s16(cr, two_bit ? 3 : 2);
        const int16x8_t d1 = vsubq_s16(cq, cp), d2 = vsubq_s16(cs, cr);
#pragma GCC unroll 4
        for (int v = 0; v < 4; v++) {
            uint8_t *row = rows[v];
            int16x8_t t = vaddq_s16(vmulq_n_s16(p1, (int16_t)(4 - v)), vmulq_n_s16(p2, (int16_t)v));
            const int16x8_t dd = vaddq_s16(vmulq_n_s16(d1, (int16_t)(4 - v)), vmulq_n_s16(d2, (int16_t)v));
#pragma GCC unroll 8
            for (int u = 0; u < xb; u++) {
                int pt;
                const int m = modulation(b, two_bit, v + 2, u + half, &pt);
                int16x8_t sig = vshlq_s16(t, sh1);
                sig = vaddq_s16(sig, vshlq_s16(sig, sh2));
                const int16x4_t ca = vget_low_s16(sig), cb = vget_high_s16(sig);
                const int16x4_t res = vshr_n_s16(vmla_n_s16(vshl_n_s16(ca, 3), vsub_s16(cb, ca), (int16_t)m), 3);
                uint32_t px = vget_lane_u32(vreinterpret_u32_u8(vmovn_u16(vreinterpretq_u16_s16(vcombine_s16(res, res)))), 0);
                if (pt)
                    px &= 0x00ffffff;   // átlátszó: alfa 0
                memcpy(row + ((tx * xb + half + u) & xmask) * 4, &px, 4);
                t = vaddq_s16(t, dd);
            }
        }
        bp = bq, br = bs;
        cp = cq, cr = cs;
    }
}

static void decode_row4(const Job *j, int ty) { decode_row(j, ty, 0); }
static void decode_row2(const Job *j, int ty) { decode_row(j, ty, 1); }

static void run(Job *j) {
    void (*const fn)(const Job *, int) = j->two_bit ? decode_row2 : decode_row4;
    int r;
    while ((r = atomic_fetch_add(&j->next_row, 1)) < j->byd)
        fn(j, r);
}

// Segédszálak: minden feladatnál mindegyik bejelentkezik (busy), és a hívó csak akkor tér vissza, ha mind végzett –
// így egy későn ébredő szál sem nyúlhat egy már visszaadott feladathoz.
#define WORKERS 2
static Thread workers[WORKERS];
static int nworkers = -1;      // -1: még nem indultak el
static Mutex call_lock;        // a hívók sorban (a feladat és a szálak közösek)
static Mutex mtx;
static CondVar cv_start, cv_done;
static Job *cur_job;
static uint32_t job_gen;
static int busy;

static void worker_main(void *arg) {
    (void)arg;
    uint32_t seen = 0;
    mutexLock(&mtx);
    for (;;) {
        while (job_gen == seen)
            condvarWait(&cv_start, &mtx);
        seen = job_gen;
        Job *j = cur_job;
        mutexUnlock(&mtx);
        run(j);
        mutexLock(&mtx);
        if (--busy == 0)
            condvarWakeAll(&cv_done);
    }
}

static void start_workers(void) {
    nworkers = 0;
    for (int i = 0; i < WORKERS; i++) {
        Result rc = threadCreate(&workers[nworkers], worker_main, NULL, NULL, 0x4000, 0x2C, 1 + i);
        if (R_SUCCEEDED(rc) && R_SUCCEEDED(rc = threadStart(&workers[nworkers])))
            nworkers++;
        else
            log_error("pvrtc: a %d. magra nem indult szál: 0x%x", 1 + i, rc);
    }
    log_info("pvrtc: gyors kibontás, %d segédszál", nworkers);
}

bool pvrtc_decompress(const void *src, int two_bit, int xdim, int ydim, uint8_t *out) {
    const int xb = two_bit ? 8 : 4;
    if (!src || !out || xdim < 2 * xb || ydim < 8 || (xdim & (xdim - 1)) || (ydim & (ydim - 1)))
        return false;
    Job j = { .src = src, .out = out, .two_bit = two_bit != 0, .xdim = xdim, .ydim = ydim,
              .bxd = xdim / xb, .byd = ydim / 4 };
    atomic_init(&j.next_row, 0);

    mutexLock(&call_lock);
    if (nworkers < 0)
        start_workers();
    if (nworkers > 0 && xdim * ydim >= 128 * 128) {   // kis képnél a szálak ébresztése többe kerülne
        mutexLock(&mtx);
        cur_job = &j;
        job_gen++;
        busy = nworkers;
        condvarWakeAll(&cv_start);
        mutexUnlock(&mtx);
        run(&j);
        mutexLock(&mtx);
        while (busy)
            condvarWait(&cv_done, &mtx);
        cur_job = NULL;
        mutexUnlock(&mtx);
    } else {
        run(&j);
    }
    mutexUnlock(&call_lock);
    return true;
}

#ifdef DEBUG
// Véletlen blokkokon (minden mód és színkódolás előfordul) a játék eredetijével összevetve, sok méretben.
bool pvrtc_selftest(pvrtc_fn orig) {
    static const struct { int two_bit, w, h; } sizes[] = {
        { 0, 8, 8 },   { 0, 16, 8 },  { 0, 8, 16 },  { 0, 32, 8 },   { 0, 8, 64 },   { 0, 64, 64 },
        { 0, 128, 32 }, { 0, 32, 256 }, { 0, 256, 256 },
        { 1, 16, 8 },  { 1, 16, 16 }, { 1, 32, 8 },  { 1, 16, 64 },  { 1, 64, 64 },  { 1, 256, 32 },
        { 1, 64, 256 }, { 1, 256, 256 },
    };
    uint32_t seed = 0x2545f491;
    int bad = 0, n = 0;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        for (int rep = 0; rep < 3; rep++) {
            const int w = sizes[i].w, h = sizes[i].h, two_bit = sizes[i].two_bit;
            const size_t blocks = (size_t)(w / (two_bit ? 8 : 4)) * (h / 4), px = (size_t)w * h * 4;
            uint32_t *data = malloc(blocks * 8);
            uint8_t *a = malloc(px), *b = malloc(px);
            if (!data || !a || !b) {
                free(data), free(a), free(b);
                log_error("pvrtc-önteszt: kevés a memória");
                return false;
            }
            for (size_t k = 0; k < blocks * 2; k++) {
                seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
                data[k] = seed;
            }
            memset(a, 0xcd, px);
            memset(b, 0x5a, px);
            orig(data, two_bit, w, h, a);
            if (!pvrtc_decompress(data, two_bit, w, h, b) || memcmp(a, b, px)) {
                bad++;
                log_error("pvrtc-önteszt: ELTÉR – %s %dx%d", two_bit ? "2 bit" : "4 bit", w, h);
            }
            n++;
            free(data), free(a), free(b);
        }
    log_info("pvrtc-önteszt: %d/%d méret azonos", n - bad, n);
    return bad == 0;
}
#endif
