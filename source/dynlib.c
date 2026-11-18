/*
 * dynlib.c – a libPirates.so 277 importjának feloldása
 *
 * DIRECT(x): a bionic és a newlib/libgcc/libstdc++/Mesa ABI-ja azonos, a saját függvényünk kapja.
 * WRAP(x, f): eltérő ABI vagy Switch-specifikus viselkedés – a reimpl/bionic.c (vagy setjmp.S)
 *             f függvénye. Részletek: reimpl/bionic.h.
 * Az importlista ellenőrzése: tools/dev/check-imports.sh <imports.txt>.
 *
 * A backstab-vita loader/dynlib.c felépítését követi:
 * Copyright (C) 2021 Andy Nguyen, (C) 2021 Rinnegatamante, (C) 2022-2023 Volodymyr Atamanenko
 * Copyright (C) 2026 Levi (Switch port), MIT license.
 */
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include <GLES2/gl2.h>

#include "game.h"
#include "util.h"
#include "reimpl/bionic.h"

void fix_glShaderSource(GLuint sh, GLsizei count, const GLchar *const *str, const GLint *len);   // gl_fix.c
void glyphs_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                         GLint border, GLenum format, GLenum type, const void *pixels);

// libgcc (ARM EABI futásidő) és libstdc++ – csak a címük kell
extern void __aeabi_d2lz(void);
extern void __aeabi_idiv(void);
extern void __aeabi_idivmod(void);
extern void __aeabi_ldivmod(void);
extern void __aeabi_uidiv(void);
extern void __aeabi_uidivmod(void);
extern void __aeabi_ul2d(void);
extern void __aeabi_uldivmod(void);
extern void __aeabi_unwind_cpp_pr0(void);
extern void __aeabi_unwind_cpp_pr1(void);
extern void __cxa_guard_acquire(void);
extern void __cxa_guard_release(void);
extern void _ZnajRKSt9nothrow_t(void);   // operator new[](size_t, const std::nothrow_t &)
extern const char _ZSt7nothrow;          // std::nothrow

#ifdef DEBUG
// Shader-diagnosztika (csak debug): a Mesán le nem forduló / nem linkelődő shader és a hiányzó
// (kioptimalizált vagy elírt) köd-/mátrix-uniformok a naplóba.
#define SHADER_SRC_MAX 512
static char *shader_src[SHADER_SRC_MAX];

static int shadertest = -1;   // sdmc:/switch/backstab/shadertest: a VertexAnim-csúcsárnyalók színe piros, köd nélkül

static void dbg_glShaderSource(GLuint sh, GLsizei n, const GLchar *const *str, const GLint *len) {
    if (shadertest < 0) {
        shadertest = 0;
        FILE *f = fopen(DATA_PATH "shadertest", "r");
        if (f) {
            if (fscanf(f, "%d", &shadertest) != 1)
                shadertest = 1;
            fclose(f);
            log_info("[gl] shadertest mód %d", shadertest);
        }
    }
    if (shadertest) {
        size_t total = 1;
        for (GLsizei i = 0; i < n; i++)
            total += len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
        char *src = malloc(total + 128);
        if (src) {
            src[0] = 0;
            for (GLsizei i = 0; i < n; i++)
                strncat(src, str[i], len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]));
            char *end = strrchr(src, '}');
            if (strstr(src, "RightAmplitude") && end) {
                // 1: piros, köd nélkül; 2: csak köd nélkül; 3: fehér fény, köd marad
                strcpy(end, shadertest == 2 ? "FogFactor = 0.0; }\n"
                          : shadertest == 3 ? "vColor = vec4(1.0, 1.0, 1.0, 1.0); }\n"
                                            : "vColor = vec4(1.0, 0.0, 0.0, 1.0); FogFactor = 0.0; }\n");
                const GLchar *one = src;
                fix_glShaderSource(sh, 1, &one, NULL);
                if (sh < SHADER_SRC_MAX) {
                    free(shader_src[sh]);
                    shader_src[sh] = src;
                } else {
                    free(src);
                }
                return;
            }
            free(src);
        }
    }
    fix_glShaderSource(sh, n, str, len);
    if (sh >= SHADER_SRC_MAX)
        return;
    size_t total = 1;
    for (GLsizei i = 0; i < n; i++)
        total += len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
    free(shader_src[sh]);
    char *s = shader_src[sh] = malloc(total);
    if (!s)
        return;
    s[0] = 0;
    for (GLsizei i = 0; i < n; i++)
        strncat(s, str[i], len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]));
}

static void dbg_glCompileShader(GLuint sh) {
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (ok)
        return;
    char info[1024] = "";
    glGetShaderInfoLog(sh, sizeof(info), NULL, info);
    log_error("[gl] shader %u NEM fordul: %s", sh, info);
    if (sh < SHADER_SRC_MAX && shader_src[sh]) {
        const char *src = shader_src[sh];
        // a forrás soronként (az első 40 sor elég a #define-okhoz és a hibahelyhez)
        int line = 1;
        while (*src && line <= 40) {
            const char *e = strchr(src, '\n');
            int l = e ? (int)(e - src) : (int)strlen(src);
            log_error("[gl]   %3d: %.*s", line++, l > 160 ? 160 : l, src);
            src += l + (e ? 1 : 0);
        }
    }
}

static void dbg_glLinkProgram(GLuint prog) {
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (ok)
        return;
    char info[1024] = "";
    glGetProgramInfoLog(prog, sizeof(info), NULL, info);
    log_error("[gl] program %u NEM linkelődik: %s", prog, info);
}

// VertexAnim* (növényzet) programok uniform-értékei – a ködszínű fák nyomozásához
#define PROG_MAX 1024
static uint8_t prog_watch[PROG_MAX];          // 1: a program csúcsárnyalója VertexAnim
static uint16_t prog_logged[PROG_MAX];
static GLuint cur_prog;
typedef struct { GLuint prog; GLint loc; char name[32]; } UniName;
static UniName uni_names[4096];
static int uni_count;

static uint8_t prog_deleted[PROG_MAX];       // valaha törölt programnév (újrahasznosítás figyelése)
static GLint prog_fog_loc[PROG_MAX];         // a FogStartEnd helye (+1; 0 = nincs)
static uint8_t prog_fog_bad[PROG_MAX];

static GLuint dbg_glCreateProgram(void) {
    GLuint p = glCreateProgram();
    if (p < PROG_MAX) {
        if (prog_deleted[p])
            log_info("[gl] programnév újrahasznosítva: %u", p);
        prog_watch[p] = 0;
        prog_logged[p] = 0;
        prog_fog_loc[p] = 0;
        prog_fog_bad[p] = 0;
    }
    return p;
}

static void dbg_glDeleteProgram(GLuint p) {
    glDeleteProgram(p);
    if (p && p < PROG_MAX)
        prog_deleted[p] = 1;
}

static uint32_t prog_vs_hash[PROG_MAX], prog_fs_hash[PROG_MAX];
static GLint prog_fogc_loc[PROG_MAX];        // a FogColor helye (+1)

static uint32_t src_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

// minden különböző árnyaló forrása egyszer a sdmc:/switch/backstab/shaders/<hash>.glsl-be
static void dump_src(uint32_t h, const char *src) {
    char path[96];
    snprintf(path, sizeof(path), DATA_PATH "shaders/%08x.glsl", h);
    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
        return;
    }
    mkpath(path);
    f = fopen(path, "w");
    if (f) {
        fputs(src, f);
        fclose(f);
    }
}

// mipmap-szintek átlagszíne a nagy (≥1024) RGBA-textúráknál – a szoftveres PVRTC-kibontás ellenőrzéséhez
static GLuint cur_tex2d;
static GLuint big_tex;                       // az utolsó ≥1024-es 0. szintű feltöltés textúrája
static GLuint unit_tex[8];                   // egységenként a 2D-textúra
static int cur_unit;
static GLuint veg_tex[16];                   // alfás nagy textúrák (növényzet-gyanús)
static int veg_count;

static void dbg_glActiveTexture(GLenum unit) {
    glActiveTexture(unit);
    cur_unit = (int)(unit - GL_TEXTURE0) & 7;
}

static void dbg_glBindTexture(GLenum target, GLuint tex) {
    glBindTexture(target, tex);
    if (target == GL_TEXTURE_2D) {
        cur_tex2d = tex;
        unit_tex[cur_unit] = tex;
    }
}

#define TEXINFO_MAX 4096
static struct { uint16_t w, h; uint8_t r, g, b, a; } texinfo[TEXINFO_MAX];

static void dbg_glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                             GLenum fmt, GLenum type, const void *px) {
    glyphs_glTexImage2D(target, level, ifmt, w, h, border, fmt, type, px);
    if (target == GL_TEXTURE_2D && level == 0 && cur_tex2d < TEXINFO_MAX) {
        texinfo[cur_tex2d].w = (uint16_t)w;
        texinfo[cur_tex2d].h = (uint16_t)h;
        texinfo[cur_tex2d].a = 0;
        if (px && fmt == GL_RGBA && type == GL_UNSIGNED_BYTE && (uint64_t)w * h <= 4u * 1024 * 1024) {
            const uint8_t *q = px;
            uint64_t r = 0, g = 0, b = 0, a = 0, n = (uint64_t)w * h;
            for (uint64_t i = 0; i < n; i++, q += 4) {
                r += q[0]; g += q[1]; b += q[2]; a += q[3];
            }
            texinfo[cur_tex2d].r = (uint8_t)(r / n);
            texinfo[cur_tex2d].g = (uint8_t)(g / n);
            texinfo[cur_tex2d].b = (uint8_t)(b / n);
            texinfo[cur_tex2d].a = (uint8_t)(a / n);
        }
    }
    if (target != GL_TEXTURE_2D || fmt != GL_RGBA || type != GL_UNSIGNED_BYTE || !px)
        return;
    if (level == 0)
        big_tex = (w >= 1024 && h >= 1024) ? cur_tex2d : 0;
    if (!big_tex || cur_tex2d != big_tex)
        return;
    // átlag: RGB az alfával súlyozva (a látszó rész), és a teljes alfa
    const uint8_t *p = px;
    uint64_t r = 0, g = 0, b = 0, a = 0, n = (uint64_t)w * h;
    for (uint64_t i = 0; i < n; i++, p += 4) {
        r += p[0] * p[3];
        g += p[1] * p[3];
        b += p[2] * p[3];
        a += p[3];
    }
    if (level == 0 && a && a / n < 200 && veg_count < 16)
        veg_tex[veg_count++] = cur_tex2d;
    if (a)
        log_info("[gl] tex %u szint %d %dx%d: átlag RGB (%llu %llu %llu), alfa %llu", cur_tex2d, level, w, h,
                 (unsigned long long)(r / a), (unsigned long long)(g / a), (unsigned long long)(b / a),
                 (unsigned long long)(a / n));
    else
        log_info("[gl] tex %u szint %d %dx%d: teljesen átlátszó", cur_tex2d, level, w, h);
}

static void dbg_glGenerateMipmap(GLenum target) {
    glGenerateMipmap(target);
    if (big_tex && cur_tex2d == big_tex)
        log_info("[gl] tex %u: glGenerateMipmap", cur_tex2d);
}

static void dbg_glAttachShader(GLuint prog, GLuint sh) {
    glAttachShader(prog, sh);
    if (prog >= PROG_MAX || sh >= SHADER_SRC_MAX || !shader_src[sh])
        return;
    prog_watch[prog] = 1;
    uint32_t h = src_hash(shader_src[sh]);
    GLint type = 0;
    glGetShaderiv(sh, GL_SHADER_TYPE, &type);
    if (type == GL_VERTEX_SHADER)
        prog_vs_hash[prog] = h;
    else
        prog_fs_hash[prog] = h;
    dump_src(h, shader_src[sh]);
}

static void dbg_glUseProgram(GLuint prog) {
    glUseProgram(prog);
    cur_prog = prog;
}

// rajzoláskor a növényzet-program tényleges FogStartEnd-je a GL-ből (5 mp-enként egyszer programonként)
static uint64_t prog_fog_ms[PROG_MAX];
static void fog_check(void) {
    GLuint prog = cur_prog;
    if (prog >= PROG_MAX || !prog_watch[prog] || !prog_fog_loc[prog])
        return;
    uint64_t now = time_ms();
    if (now - prog_fog_ms[prog] < 5000)
        return;
    prog_fog_ms[prog] = now;
    GLfloat v[2] = { -1, -1 }, c[4] = { -1, -1, -1, -1 };
    glGetUniformfv(prog, prog_fog_loc[prog] - 1, v);
    if (prog_fogc_loc[prog])
        glGetUniformfv(prog, prog_fogc_loc[prog] - 1, c);
    log_info("[gl] rajz p%u vs %08x fs %08x: FogStartEnd (%g, %g) FogColor (%.2f %.2f %.2f %.2f)", prog,
             prog_vs_hash[prog], prog_fs_hash[prog], (double)v[0], (double)v[1], (double)c[0], (double)c[1],
             (double)c[2], (double)c[3]);
}

static uint64_t veg_log_ms;
static void veg_check(void) {
    GLuint t = unit_tex[0];
    bool veg = false;
    for (int i = 0; i < veg_count; i++)
        veg |= t && veg_tex[i] == t;
    if (!veg || cur_prog >= PROG_MAX || !prog_fog_loc[cur_prog])   // csak a 3D-s (ködös) programok
        return;
    uint64_t now = time_ms();
    if (now - veg_log_ms < 700)
        return;
    veg_log_ms = now;
    GLuint p = cur_prog;
    GLfloat fse[2] = { -1, -1 }, fc[4] = { -1, -1, -1, -1 }, wvt[16] = { 0 }, glc[4] = { -1 }, gac[4] = { -1 };
    GLint l;
    if ((l = glGetUniformLocation(p, "FogStartEnd")) >= 0) glGetUniformfv(p, l, fse);
    if ((l = glGetUniformLocation(p, "FogColor")) >= 0) glGetUniformfv(p, l, fc);
    if ((l = glGetUniformLocation(p, "WorldViewTransposed")) >= 0) glGetUniformfv(p, l, wvt);
    if ((l = glGetUniformLocation(p, "GlobalLightColor")) >= 0) glGetUniformfv(p, l, glc);
    if ((l = glGetUniformLocation(p, "GlobalAmbientColor")) >= 0) glGetUniformfv(p, l, gac);
    GLint ca = glGetAttribLocation(p, "Color"), en = -1;
    GLfloat cv[4] = { -1, -1, -1, -1 };
    if (ca >= 0) {
        glGetVertexAttribiv(ca, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en);
        glGetVertexAttribfv(ca, GL_CURRENT_VERTEX_ATTRIB, cv);
    }
    GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), dmask = 0;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask);
    GLint bsrc = 0, bdst = 0;
    glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc);
    glGetIntegerv(GL_BLEND_DST_RGB, &bdst);
    log_info("[gl] növényzet tex %u p%u vs %08x: köd (%g,%g) szín (%.2f %.2f %.2f %.2f) WVT2 (%.3f %.3f %.3f %.1f) "
             "fény (%.2f %.2f %.2f %.2f) amb (%.2f %.2f %.2f %.2f) Color@%d tömb %d érték (%.2f %.2f %.2f %.2f) "
             "blend %d (%x,%x) depth %d mask %d",
             t, p, p < PROG_MAX ? prog_vs_hash[p] : 0, (double)fse[0], (double)fse[1], (double)fc[0], (double)fc[1],
             (double)fc[2], (double)fc[3], (double)wvt[8], (double)wvt[9], (double)wvt[10], (double)wvt[11],
             (double)glc[0], (double)glc[1], (double)glc[2], (double)glc[3], (double)gac[0], (double)gac[1],
             (double)gac[2], (double)gac[3], ca, en, (double)cv[0], (double)cv[1], (double)cv[2], (double)cv[3],
             blend, bsrc, bdst, depth, dmask);
}

static uint32_t va_seen[1024];
static int va_seen_n;
static void va_check(void) {
    GLuint p = cur_prog;
    if (p >= PROG_MAX || !prog_vs_hash[p] || p >= SHADER_SRC_MAX)
        return;
    // a program csúcsárnyalója VertexAnim? (a forrásból)
    static uint8_t is_va[PROG_MAX];
    if (!is_va[p]) {
        GLuint sh[4];
        GLsizei n = 0;
        glGetAttachedShaders(p, 4, &n, sh);
        is_va[p] = 2;
        for (GLsizei i = 0; i < n; i++)
            if (sh[i] < SHADER_SRC_MAX && shader_src[sh[i]] && strstr(shader_src[sh[i]], "RightAmplitude"))
                is_va[p] = 1;
    }
    if (is_va[p] != 1)
        return;
    GLuint t = unit_tex[0];
    // mátrixok: a ködmélység (WorldViewTransposed 2. oszlopa) és a vágótér w-je (WorldViewProjection 3. sora)
    static uint64_t va_ms;
    uint64_t now = time_ms();
    if (now - va_ms > 400) {
        va_ms = now;
        GLfloat wvt[16] = { 0 }, wvp[16] = { 0 }, fse[2] = { -1, -1 };
        GLint l;
        if ((l = glGetUniformLocation(p, "WorldViewTransposed")) >= 0) glGetUniformfv(p, l, wvt);
        if ((l = glGetUniformLocation(p, "WorldViewProjection")) >= 0) glGetUniformfv(p, l, wvp);
        if ((l = glGetUniformLocation(p, "FogStartEnd")) >= 0) glGetUniformfv(p, l, fse);
        log_info("[gl] VA p%u tex %u: köd (%g,%g) WVT[2] (%.3f %.3f %.3f %.1f) WVP w-sor (%.3f %.3f %.3f %.1f)", p, t,
                 (double)fse[0], (double)fse[1], (double)wvt[8], (double)wvt[9], (double)wvt[10], (double)wvt[11],
                 (double)wvp[3], (double)wvp[7], (double)wvp[11], (double)wvp[15]);
    }
    uint32_t key = (p << 16) | (t & 0xffff);
    for (int i = 0; i < va_seen_n; i++)
        if (va_seen[i] == key)
            return;
    if (va_seen_n < 1024)
        va_seen[va_seen_n++] = key;
    if (t < TEXINFO_MAX)
        log_info("[gl] VertexAnim p%u (vs %08x) tex %u: %ux%u átlag (%u %u %u %u)", p, prog_vs_hash[p], t,
                 texinfo[t].w, texinfo[t].h, texinfo[t].r, texinfo[t].g, texinfo[t].b, texinfo[t].a);
}

// távoli rajzok köde programtól függetlenül (programonként fél másodpercenként egyszer)
static uint64_t far_ms[PROG_MAX];
static void far_check(void) {
    GLuint p = cur_prog;
    if (p >= PROG_MAX || !prog_fog_loc[p])
        return;
    uint64_t now = time_ms();
    if (now - far_ms[p] < 500)
        return;
    GLfloat wvt[16] = { 0 }, fse[2] = { -1, -1 }, fc[4] = { -1, -1, -1, -1 };
    GLint l;
    if ((l = glGetUniformLocation(p, "WorldViewTransposed")) < 0)
        return;
    glGetUniformfv(p, l, wvt);
    if (wvt[11] < 8000.0f)
        return;
    far_ms[p] = now;
    glGetUniformfv(p, prog_fog_loc[p] - 1, fse);
    if (prog_fogc_loc[p])
        glGetUniformfv(p, prog_fogc_loc[p] - 1, fc);
    log_info("[gl] TÁVOLI p%u vs %08x tex %u: mélység ~%.0f köd (%g,%g) alfa %.2f", p, prog_vs_hash[p], unit_tex[0],
             (double)wvt[11], (double)fse[0], (double)fse[1], (double)fc[3]);
}

static void dbg_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx) {
    fog_check();
    far_check();
    veg_check();
    va_check();
    glDrawElements(mode, count, type, idx);
}

static void dbg_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    fog_check();
    glDrawArrays(mode, first, count);
}

static const char *uni_name(GLint loc) {
    for (int i = 0; i < uni_count; i++)
        if (uni_names[i].prog == cur_prog && uni_names[i].loc == loc)
            return uni_names[i].name;
    return "?";
}

static bool uni_watch(void) {
    return false;   // a rajzoláskori fog_check elég
}

static void dbg_glUniform2fv(GLint loc, GLsizei n, const GLfloat *v) {
    glUniform2fv(loc, n, v);
    if (uni_watch()) {
        prog_logged[cur_prog]++;
        log_info("[gl] p%u %s = (%g, %g)", cur_prog, uni_name(loc), (double)v[0], (double)v[1]);
    }
}

static void dbg_glUniform4fv(GLint loc, GLsizei n, const GLfloat *v) {
    glUniform4fv(loc, n, v);
    if (uni_watch()) {
        prog_logged[cur_prog]++;
        log_info("[gl] p%u %s = (%g, %g, %g, %g)", cur_prog, uni_name(loc), (double)v[0], (double)v[1],
                 (double)v[2], (double)v[3]);
    }
}

static void dbg_glUniform1f(GLint loc, GLfloat v) {
    glUniform1f(loc, v);
    if (uni_watch()) {
        prog_logged[cur_prog]++;
        log_info("[gl] p%u %s = %g", cur_prog, uni_name(loc), (double)v);
    }
}

static void dbg_glUniformMatrix4fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat *m) {
    glUniformMatrix4fv(loc, n, tr, m);
    if (uni_watch()) {
        prog_logged[cur_prog]++;
        log_info("[gl] p%u %s%s = [%g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g]", cur_prog,
                 uni_name(loc), tr ? " (T)" : "", (double)m[0], (double)m[1], (double)m[2], (double)m[3],
                 (double)m[4], (double)m[5], (double)m[6], (double)m[7], (double)m[8], (double)m[9],
                 (double)m[10], (double)m[11], (double)m[12], (double)m[13], (double)m[14], (double)m[15]);
    }
}

static GLint dbg_glGetUniformLocation(GLuint prog, const GLchar *name) {
    GLint loc = glGetUniformLocation(prog, name);
    if (loc >= 0 && prog < PROG_MAX && !strcmp(name, "FogStartEnd"))
        prog_fog_loc[prog] = loc + 1;
    if (loc >= 0 && prog < PROG_MAX && !strcmp(name, "FogColor"))
        prog_fogc_loc[prog] = loc + 1;
    if (loc >= 0 && uni_count < (int)(sizeof(uni_names) / sizeof(uni_names[0]))) {
        UniName *u = &uni_names[uni_count++];
        u->prog = prog;
        u->loc = loc;
        snprintf(u->name, sizeof(u->name), "%s", name);
    }
    if (loc < 0 && (strstr(name, "Fog") || strstr(name, "WorldView")))
        log_info("[gl] program %u: nincs „%s” uniform", prog, name);
    return loc;
}
#endif

#define DIRECT(name)   { #name, (uintptr_t)&name }
#define WRAP(name, fn) { #name, (uintptr_t)&fn }

static const so_default_dynlib dynlib[] = {
    // --- C++ / ARM EABI futásidő ---------------------------------------------------------------
    WRAP(__aeabi_atexit, bionic_aeabi_atexit),
    DIRECT(__aeabi_d2lz),
    DIRECT(__aeabi_idiv),
    DIRECT(__aeabi_idivmod),
    DIRECT(__aeabi_ldivmod),
    DIRECT(__aeabi_uidiv),
    DIRECT(__aeabi_uidivmod),
    DIRECT(__aeabi_ul2d),
    DIRECT(__aeabi_uldivmod),
    DIRECT(__aeabi_unwind_cpp_pr0),
    DIRECT(__aeabi_unwind_cpp_pr1),
    DIRECT(__cxa_guard_acquire),
    DIRECT(__cxa_guard_release),
    WRAP(__cxa_pure_virtual, bionic_cxa_pure_virtual),
    WRAP(__dso_handle, bionic_dso_handle),
    WRAP(__stack_chk_fail, bionic_stack_chk_fail),
    WRAP(__stack_chk_guard, bionic_stack_chk_guard),
    DIRECT(_ZnajRKSt9nothrow_t),
    DIRECT(_ZSt7nothrow),

    // --- Android ---------------------------------------------------------------------------------
    WRAP(__android_log_print, bionic_android_log_print),

    // --- folyamat, környezet ---------------------------------------------------------------------
    WRAP(abort, bionic_abort),
    WRAP(exit, bionic_exit),
    WRAP(getenv, bionic_getenv),
    WRAP(setlocale, bionic_setlocale),
    WRAP(sysconf, bionic_sysconf),
    WRAP(system, bionic_system),
    DIRECT(__errno),            // newlib szálankénti errno (a Lua strerror-ja is newlibes)
    WRAP(setjmp, bionic_setjmp),
    WRAP(longjmp, bionic_longjmp),

    // --- stdio (FILE*: a __sF-tömb elemei → newlib stdin/stdout/stderr) --------------------------
    WRAP(__sF, bionic_sF),
    WRAP(__srget, bionic_srget),
    WRAP(fclose, bionic_fclose),
    WRAP(ferror, bionic_ferror),
    WRAP(fflush, bionic_fflush),
    WRAP(fgetpos, bionic_fgetpos),
    WRAP(fgets, bionic_fgets),
    WRAP(fopen, bionic_fopen),
    WRAP(fprintf, bionic_fprintf),
    WRAP(fputc, bionic_fputc),
    WRAP(fputs, bionic_fputs),
    WRAP(fread, bionic_fread),
    WRAP(freopen, bionic_freopen),
    WRAP(fscanf, bionic_fscanf),
    WRAP(fseek, bionic_fseek),
    WRAP(fsetpos, bionic_fsetpos),
    WRAP(ftell, bionic_ftell),
    WRAP(fwrite, bionic_fwrite),
    WRAP(getc, bionic_getc),
    WRAP(putc, bionic_putc),
    WRAP(setvbuf, bionic_setvbuf),
    WRAP(tmpfile, bionic_tmpfile),
    WRAP(tmpnam, bionic_tmpnam),
    WRAP(ungetc, bionic_ungetc),
    DIRECT(printf),
    DIRECT(putchar),
    DIRECT(puts),
    DIRECT(snprintf),
    DIRECT(sprintf),
    DIRECT(sscanf),
    DIRECT(vsnprintf),

    // --- fájlleírók, fájlrendszer (Android-utak → sdmc:) ----------------------------------------
    WRAP(chdir, bionic_chdir),
    DIRECT(close),
    WRAP(fcntl, bionic_fcntl),
    WRAP(fstat, bionic_fstat),
    WRAP(lseek, bionic_lseek),
    WRAP(open, bionic_open),
    DIRECT(read),
    WRAP(remove, bionic_remove),
    WRAP(rename, bionic_rename),
    WRAP(unlink, bionic_unlink),
    DIRECT(write),

    // --- memória ---------------------------------------------------------------------------------
    DIRECT(free),
    DIRECT(malloc),
    DIRECT(memchr),
    DIRECT(memcmp),
    DIRECT(memcpy),
    DIRECT(memmove),
    DIRECT(memset),
    WRAP(mmap, bionic_mmap),
    WRAP(munmap, bionic_munmap),
    DIRECT(realloc),

    // --- stdlib, string --------------------------------------------------------------------------
    DIRECT(atoi),
    DIRECT(lrand48),
    DIRECT(qsort),
    DIRECT(srand48),
    DIRECT(strcasecmp),
    DIRECT(strcat),
    DIRECT(strchr),
    DIRECT(strcmp),
    DIRECT(strcoll),
    DIRECT(strcpy),
    DIRECT(strcspn),
    DIRECT(strdup),
    DIRECT(strerror),
    DIRECT(strlen),
    DIRECT(strncasecmp),
    DIRECT(strncat),
    DIRECT(strncmp),
    DIRECT(strncpy),
    DIRECT(strpbrk),
    DIRECT(strrchr),
    DIRECT(strstr),
    DIRECT(strtod),
    DIRECT(strtok),
    DIRECT(strtoul),

    // --- ctype (bionic-táblák), wchar (wchar_t mindkét oldalon 4 bájt) ----------------------------
    WRAP(_ctype_, BIONIC_ctype_),
    WRAP(_tolower_tab_, BIONIC_tolower_tab_),
    WRAP(_toupper_tab_, BIONIC_toupper_tab_),
    DIRECT(iswalpha),
    DIRECT(iswcntrl),
    DIRECT(iswdigit),
    DIRECT(iswlower),
    DIRECT(iswprint),
    DIRECT(iswpunct),
    DIRECT(iswspace),
    DIRECT(iswupper),
    DIRECT(iswxdigit),
    DIRECT(swprintf),
    DIRECT(towlower),
    DIRECT(towupper),
    DIRECT(wcscmp),
    DIRECT(wcscpy),
    DIRECT(wcslen),
    DIRECT(wcsncpy),
    DIRECT(wmemcmp),
    DIRECT(wmemcpy),
    DIRECT(wmemmove),
    DIRECT(wmemset),

    // --- libm (softfp mindkét oldalon) -----------------------------------------------------------
    DIRECT(acos),
    DIRECT(acosf),
    DIRECT(asinf),
    DIRECT(atan),
    DIRECT(atan2),
    DIRECT(atan2f),
    DIRECT(atanf),
    DIRECT(ceil),
    DIRECT(ceilf),
    DIRECT(cos),
    DIRECT(cosf),
    DIRECT(cosh),
    DIRECT(expf),
    DIRECT(floor),
    DIRECT(floorf),
    DIRECT(fmodf),
    DIRECT(frexpf),
    DIRECT(ldexpf),
    DIRECT(log10),
    DIRECT(logf),
    DIRECT(modff),
    DIRECT(pow),
    DIRECT(powf),
    DIRECT(sin),
    DIRECT(sinf),
    DIRECT(sinh),
    DIRECT(tan),
    DIRECT(tanf),
    DIRECT(tanh),

    // --- idő (32 bites time_t) -------------------------------------------------------------------
    WRAP(clock, bionic_clock),
    WRAP(difftime, bionic_difftime),
    WRAP(gettimeofday, bionic_gettimeofday),
    WRAP(gmtime, bionic_gmtime),
    WRAP(localtime, bionic_localtime),
    WRAP(mktime, bionic_mktime),
    WRAP(nanosleep, bionic_nanosleep),
    WRAP(strftime, bionic_strftime),
    WRAP(time, bionic_time),
    DIRECT(usleep),

    // --- pthread (pthread_t, pthread_key_t: 4 bájt mindkét oldalon) ------------------------------
    WRAP(pthread_create, bionic_pthread_create),
    DIRECT(pthread_getspecific),
    DIRECT(pthread_join),
    DIRECT(pthread_key_create),
    WRAP(pthread_mutex_destroy, bionic_pthread_mutex_destroy),
    WRAP(pthread_mutex_init, bionic_pthread_mutex_init),
    WRAP(pthread_mutex_lock, bionic_pthread_mutex_lock),
    WRAP(pthread_mutex_trylock, bionic_pthread_mutex_trylock),
    WRAP(pthread_mutex_unlock, bionic_pthread_mutex_unlock),
    DIRECT(pthread_self),
    DIRECT(pthread_setspecific),
    DIRECT(sched_yield),

    // --- hálózat (offline: tiszta hibák) ---------------------------------------------------------
    WRAP(bind, bionic_bind),
    WRAP(freeaddrinfo, bionic_freeaddrinfo),
    WRAP(getaddrinfo, bionic_getaddrinfo),
    WRAP(gethostname, bionic_gethostname),
    WRAP(recvfrom, bionic_recvfrom),
    WRAP(select, bionic_select),
    WRAP(sendto, bionic_sendto),
    WRAP(setsockopt, bionic_setsockopt),
    WRAP(socket, bionic_socket),

    // --- OpenGL ES 2.0 (Mesa) --------------------------------------------------------------------
#ifdef DEBUG
    WRAP(glActiveTexture, dbg_glActiveTexture),
#else
    DIRECT(glActiveTexture),
#endif
#ifdef DEBUG
    WRAP(glAttachShader, dbg_glAttachShader),
#else
    DIRECT(glAttachShader),
#endif
    DIRECT(glBindBuffer),
    DIRECT(glBindFramebuffer),
    DIRECT(glBindRenderbuffer),
#ifdef DEBUG
    WRAP(glBindTexture, dbg_glBindTexture),
#else
    DIRECT(glBindTexture),
#endif
    DIRECT(glBlendColor),
    DIRECT(glBlendEquation),
    DIRECT(glBlendFunc),
    DIRECT(glBufferData),
    DIRECT(glBufferSubData),
    DIRECT(glCheckFramebufferStatus),
    DIRECT(glClear),
    DIRECT(glClearColor),
    DIRECT(glClearDepthf),
    DIRECT(glClearStencil),
    DIRECT(glColorMask),
#ifdef DEBUG
    WRAP(glCompileShader, dbg_glCompileShader),
#else
    DIRECT(glCompileShader),
#endif
    DIRECT(glCompressedTexImage2D),
    DIRECT(glCompressedTexSubImage2D),
    DIRECT(glCopyTexSubImage2D),
#ifdef DEBUG
    WRAP(glCreateProgram, dbg_glCreateProgram),
#else
    DIRECT(glCreateProgram),
#endif
    DIRECT(glCreateShader),
    DIRECT(glCullFace),
    DIRECT(glDeleteBuffers),
    DIRECT(glDeleteFramebuffers),
#ifdef DEBUG
    WRAP(glDeleteProgram, dbg_glDeleteProgram),
#else
    DIRECT(glDeleteProgram),
#endif
    DIRECT(glDeleteRenderbuffers),
    DIRECT(glDeleteShader),
    DIRECT(glDeleteTextures),
    DIRECT(glDepthFunc),
    DIRECT(glDepthMask),
    DIRECT(glDepthRangef),
    DIRECT(glDisable),
    DIRECT(glDisableVertexAttribArray),
#ifdef DEBUG
    WRAP(glDrawArrays, dbg_glDrawArrays),
#else
    DIRECT(glDrawArrays),
#endif
#ifdef DEBUG
    WRAP(glDrawElements, dbg_glDrawElements),
#else
    DIRECT(glDrawElements),
#endif
    DIRECT(glEnable),
    DIRECT(glEnableVertexAttribArray),
    DIRECT(glFlush),
    DIRECT(glFramebufferRenderbuffer),
    DIRECT(glFramebufferTexture2D),
    DIRECT(glFrontFace),
    DIRECT(glGenBuffers),
#ifdef DEBUG
    WRAP(glGenerateMipmap, dbg_glGenerateMipmap),
#else
    DIRECT(glGenerateMipmap),
#endif
    DIRECT(glGenFramebuffers),
    DIRECT(glGenRenderbuffers),
    DIRECT(glGenTextures),
    DIRECT(glGetActiveAttrib),
    DIRECT(glGetActiveUniform),
    DIRECT(glGetAttribLocation),
    DIRECT(glGetError),
    DIRECT(glGetFloatv),
    DIRECT(glGetIntegerv),
    DIRECT(glGetProgramInfoLog),
    DIRECT(glGetProgramiv),
    DIRECT(glGetShaderInfoLog),
    DIRECT(glGetShaderiv),
    DIRECT(glGetString),
#ifdef DEBUG
    WRAP(glGetUniformLocation, dbg_glGetUniformLocation),
#else
    DIRECT(glGetUniformLocation),
#endif
    DIRECT(glLineWidth),
#ifdef DEBUG
    WRAP(glLinkProgram, dbg_glLinkProgram),
#else
    DIRECT(glLinkProgram),
#endif
    DIRECT(glPixelStorei),
    DIRECT(glPolygonOffset),
    DIRECT(glReadPixels),
    DIRECT(glRenderbufferStorage),
    DIRECT(glSampleCoverage),
    DIRECT(glScissor),
#ifdef DEBUG
    WRAP(glShaderSource, dbg_glShaderSource),
#else
    WRAP(glShaderSource, fix_glShaderSource),   // a Mesán le nem forduló játék-shaderek javítása (gl_fix.c)
#endif
    DIRECT(glStencilMask),
#ifdef DEBUG
    WRAP(glTexImage2D, dbg_glTexImage2D),      // napló + glyphs_glTexImage2D
#else
    WRAP(glTexImage2D, glyphs_glTexImage2D),   // Switch-gombjelek, lásd glyphs.c
#endif
    DIRECT(glTexParameterf),
    DIRECT(glTexParameteri),
    DIRECT(glTexSubImage2D),
#ifdef DEBUG
    WRAP(glUniform1f, dbg_glUniform1f),
#else
    DIRECT(glUniform1f),
#endif
    DIRECT(glUniform1fv),
    DIRECT(glUniform1i),
    DIRECT(glUniform1iv),
#ifdef DEBUG
    WRAP(glUniform2fv, dbg_glUniform2fv),
#else
    DIRECT(glUniform2fv),
#endif
    DIRECT(glUniform2iv),
    DIRECT(glUniform3fv),
    DIRECT(glUniform3iv),
#ifdef DEBUG
    WRAP(glUniform4fv, dbg_glUniform4fv),
#else
    DIRECT(glUniform4fv),
#endif
    DIRECT(glUniform4iv),
#ifdef DEBUG
    WRAP(glUniformMatrix4fv, dbg_glUniformMatrix4fv),
#else
    DIRECT(glUniformMatrix4fv),
#endif
#ifdef DEBUG
    WRAP(glUseProgram, dbg_glUseProgram),
#else
    DIRECT(glUseProgram),
#endif
    DIRECT(glVertexAttrib4f),
    DIRECT(glVertexAttribPointer),
    DIRECT(glViewport),
};

void resolve_imports(so_module *mod) {
    bionic_init();
    so_resolve(mod, dynlib, sizeof(dynlib) / sizeof(dynlib[0]));
}
