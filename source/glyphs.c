/*
 * glyphs.c – Switch-gombjelek a játék Xperia Play (PlayStation-stílusú) gombjelei helyett
 *
 * A súgószövegek és a HUD a gombjeleket az interface sprite moduljaiból rajzolják (a szövegben
 * „\x02<képkocka>” hivatkozással). Ezek a modulok az interface.pvr (2048×2048, PVRTC4) textúrában
 * vannak; a játék a PVRTC-t szoftveresen bontja ki (a nouveau nem ismeri), és RGBA8888-ként
 * glTexImage2D-vel tölti fel. Itt, a feltöltésnél cseréljük ki a modulok pixeleit a saját jeleinkre.
 *
 * Copyright (C) 2026 Levi (Switch port), MIT license.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <GLES2/gl2.h>

#include "util.h"
#include "glyphs_data.h"   // tools/assets/make-glyphs.py

typedef struct { int x, y, w, h; int r, g, b, a; } RefRect;

// Az interface.pvr néhány gombjel-moduljának átlagszíne (a PVRTC-dekódolásunkból; lásd tools/assets/make-glyphs.py)
static const RefRect refs[] = {
    {  262, 601, 37, 10, 174, 172, 173,  96 },   // START
    {  457, 608, 46, 10, 184, 184, 183,  93 },   // SELECT
    { 1684, 265, 16, 17, 138, 137, 137, 137 },   // ×
    { 1100, 598, 19, 17, 156, 155, 156, 155 },   // △
    { 1878, 523, 19, 18, 149, 146, 145, 185 },   // ○
};

static void rect_mean(const uint8_t *px, int stride, int x, int y, int w, int h, int out[4]) {
    long sum[4] = { 0 };
    for (int j = 0; j < h; j++) {
        const uint8_t *row = px + ((size_t)(y + j) * stride + x) * 4;
        for (int i = 0; i < w * 4; i++)
            sum[i & 3] += row[i];
    }
    for (int c = 0; c < 4; c++)
        out[c] = (int)(sum[c] / (w * h));
}

// Igaz, ha a 2048×2048-as RGBA kép az interface textúra (flipped: függőlegesen tükrözve tárolt).
static int is_interface(const uint8_t *px, int flipped) {
    int bad = 0;
    for (size_t k = 0; k < sizeof(refs) / sizeof(refs[0]); k++) {
        const RefRect *r = &refs[k];
        int m[4];
        rect_mean(px, 2048, r->x, flipped ? 2048 - r->y - r->h : r->y, r->w, r->h, m);
        if (abs(m[0] - r->r) > 16 || abs(m[1] - r->g) > 16 || abs(m[2] - r->b) > 16 || abs(m[3] - r->a) > 16)
            bad++;
    }
    return bad == 0;
}

void glyphs_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                         GLint border, GLenum format, GLenum type, const void *pixels) {
    if (target == GL_TEXTURE_2D && level == 0 && width == 2048 && height == 2048 && format == GL_RGBA &&
        type == GL_UNSIGNED_BYTE && pixels) {
        // A játék a sorokat felülről lefelé adja át (mérve), tükrözést nem kell kezelni.
        if (is_interface(pixels, 0)) {
            uint8_t *px = (uint8_t *)pixels;   // a játék saját, feltöltés utáni pufferét írjuk
            for (size_t g = 0; g < sizeof(glyphs) / sizeof(glyphs[0]); g++) {
                const Glyph *gl = &glyphs[g];
                for (int j = 0; j < gl->h; j++)
                    memcpy(px + ((size_t)(gl->y + j) * 2048 + gl->x) * 4, gl->rgba + (size_t)j * gl->w * 4,
                           (size_t)gl->w * 4);
            }
            log_info("glyphs: Switch-gombjelek az interface textúrában (%u modul)",
                     (unsigned)(sizeof(glyphs) / sizeof(glyphs[0])));
        }
    }
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
}
