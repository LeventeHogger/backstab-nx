/*
 * gl.c – EGL + GLES2 (Mesa/nouveau) a Switch alapablakára
 *
 * Androidon a Java GLSurfaceView hozta létre a környezetet; a játék maga csak GLES2-t hív.
 * Itt ugyanazt adjuk: RGBA8 színpuffer, 24 bites mélység + 8 bites stencil (a játék glClearStencilt
 * és glStencilMaskot hív, és a nouveau natív formátuma úgyis a packed D24S8), 1920×1080-as ablak
 * (a játék logikai felbontása, lásd main.c; kézi módban a rendszer skálázza le).
 *
 * Copyright (C) 2026 Levi (Switch port), MIT license.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>
#include <stdint.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "game.h"
#include "port.h"
#include "util.h"

static EGLDisplay s_display = EGL_NO_DISPLAY;
static EGLContext s_context = EGL_NO_CONTEXT;
static EGLSurface s_surface = EGL_NO_SURFACE;

#define EGL_CHECK(ok, what) \
    do { if (!(ok)) fatal_error("EGL-hiba: %s (0x%04x)", what, (unsigned)eglGetError()); } while (0)

// Hosszú GL-szöveg (a kiterjesztéslista) naplózása darabokban: a log_printf puffere 1 KiB.
static void log_gl_string(const char *name, GLenum e) {
    const char *s = (const char *)glGetString(e);
    if (!s) {
        log_info("%s: (null)", name);
        return;
    }
    size_t len = strlen(s);
    if (len < 800) {
        log_info("%s: %s", name, s);
        return;
    }
    for (size_t off = 0; off < len; off += 800)
        log_info("%s [%u]: %.800s", name, (unsigned)(off / 800), s + off);
}

void gl_init(void) {
    NWindow *win = nwindowGetDefault();
    Result rc = nwindowSetDimensions(win, (u32)screen_width, (u32)screen_height);
    if (R_FAILED(rc))
        fatal_error("nwindowSetDimensions(%d, %d): 0x%x", screen_width, screen_height, (unsigned)rc);

    s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGL_CHECK(s_display != EGL_NO_DISPLAY, "eglGetDisplay");
    EGL_CHECK(eglInitialize(s_display, NULL, NULL), "eglInitialize");
    EGL_CHECK(eglBindAPI(EGL_OPENGL_ES_API), "eglBindAPI");

    static const EGLint config_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      24,
        EGL_STENCIL_SIZE,    8,
        EGL_NONE
    };
    EGLConfig config;
    EGLint num_configs = 0;
    EGL_CHECK(eglChooseConfig(s_display, config_attribs, &config, 1, &num_configs), "eglChooseConfig");
    if (num_configs == 0)
        fatal_error("EGL: nincs RGBA8/D24S8 GLES2-konfiguráció");

    s_surface = eglCreateWindowSurface(s_display, config, (EGLNativeWindowType)win, NULL);
    EGL_CHECK(s_surface != EGL_NO_SURFACE, "eglCreateWindowSurface");

    static const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, context_attribs);
    EGL_CHECK(s_context != EGL_NO_CONTEXT, "eglCreateContext");
    EGL_CHECK(eglMakeCurrent(s_display, s_surface, s_surface, s_context), "eglMakeCurrent");

    // 30 fps-es korlát = minden második vsync; 60 és „nincs korlát” = minden vsync.
    int interval = settings.fps_lock == 30 ? 2 : 1;
    if (!eglSwapInterval(s_display, interval))
        log_error("eglSwapInterval(%d) nem sikerült (0x%04x)", interval, (unsigned)eglGetError());

    log_info("EGL kész: %dx%d, swap interval %d", screen_width, screen_height, interval);
    log_gl_string("GL_VENDOR", GL_VENDOR);
    log_gl_string("GL_RENDERER", GL_RENDERER);
    log_gl_string("GL_VERSION", GL_VERSION);
    log_gl_string("GL_SHADING_LANGUAGE_VERSION", GL_SHADING_LANGUAGE_VERSION);
    log_gl_string("GL_EXTENSIONS", GL_EXTENSIONS);
}

// Egy kitöltött téglalap a képernyőn (bal felső sarok szerinti koordinátákkal) – ollós glClear,
// így nem kell shader, puffer vagy textúra, és a játék GL-állapotából csak keveset kell menteni.
static void fill_rect(int x, int y, int w, int h, float r, float g, float b) {
    glScissor(x, screen_height - y - h, w, h);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

// Téglalap-keret: kívül fekete, belül sárga (a menü fókuszban lévő gombja körül).
static void frame_rect(int x, int y, int w, int h) {
    const int t = 3;   // a vonalvastagság (képernyőpixel)
    x -= 2 * t;
    y -= 2 * t;
    w += 4 * t;
    h += 4 * t;
    fill_rect(x, y, w, 2 * t, 0.f, 0.f, 0.f);
    fill_rect(x, y + h - 2 * t, w, 2 * t, 0.f, 0.f, 0.f);
    fill_rect(x, y, 2 * t, h, 0.f, 0.f, 0.f);
    fill_rect(x + w - 2 * t, y, 2 * t, h, 0.f, 0.f, 0.f);
    fill_rect(x + t / 2, y + t / 2, w - t, t, 1.f, 0.82f, 0.1f);
    fill_rect(x + t / 2, y + h - t - t / 2, w - t, t, 1.f, 0.82f, 0.1f);
    fill_rect(x + t / 2, y + t / 2, t, h - t, 1.f, 0.82f, 0.1f);
    fill_rect(x + w - t - t / 2, y + t / 2, t, h - t, 1.f, 0.82f, 0.1f);
}

// A menükurzor (fekete keretes fehér célkereszt) vagy a menüfókusz kerete (controls.c) a kép
// tetejére. A glitch-motor gyorsítótárazza a GL-állapotot, ezért mindent pontosan visszaállítunk.
static void draw_cursor(void) {
    int x, y, fw = 0, fh = 0;
    bool cursor = controls_cursor(&x, &y);
    int fx, fy;
    bool focus = !cursor && controls_focus_rect(&fx, &fy, &fw, &fh);
    if (!cursor && !focus)
        return;

    GLint fbo, scissor_box[4];
    GLfloat clear_color[4];
    GLboolean color_mask[4];
    GLboolean scissor_on = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_SCISSOR_BOX, scissor_box);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);

    if (fbo != 0)
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!scissor_on)
        glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    if (cursor) {
        fill_rect(x - 12, y - 3, 25, 7, 0.f, 0.f, 0.f);
        fill_rect(x - 3, y - 12, 7, 25, 0.f, 0.f, 0.f);
        fill_rect(x - 10, y - 1, 21, 3, 1.f, 1.f, 1.f);
        fill_rect(x - 1, y - 10, 3, 21, 1.f, 1.f, 1.f);
    } else {
        frame_rect(fx, fy, fw, fh);
    }

    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
    glScissor(scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3]);
    if (!scissor_on)
        glDisable(GL_SCISSOR_TEST);
    if (fbo != 0)
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
}

#ifdef DEBUG
static char shot_name[64];

void gl_debug_shot(const char *name) {
    snprintf(shot_name, sizeof(shot_name), "%s", name);
}

// a hátsó puffer a csere előtt (a játék képe + a menükeret/kurzor) → shots/<név>.rgba
static void write_shot(void) {
    GLint fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    if (fbo != 0)
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    size_t size = (size_t)screen_width * screen_height * 4;
    uint8_t *px = malloc(size);
    if (px) {
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, screen_width, screen_height, GL_RGBA, GL_UNSIGNED_BYTE, px);
        mkdir(DATA_PATH "shots", 0777);
        char path[128];
        snprintf(path, sizeof(path), DATA_PATH "shots/%s.rgba", shot_name);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(px, 1, size, f);
            fclose(f);
            log_info("[shot] %s", path);
        } else {
            log_error("[shot] nem írható: %s", path);
        }
        free(px);
    }
    if (fbo != 0)
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
    shot_name[0] = 0;
}
#endif

void gl_swap(void) {
    static unsigned fails;
    draw_cursor();
#ifdef DEBUG
    if (shot_name[0])
        write_shot();
#endif
    if (!eglSwapBuffers(s_display, s_surface) && fails++ < 5)
        log_error("eglSwapBuffers: 0x%x", eglGetError());
#ifdef DEBUG
    // képkockaszám-napló (5 mp-enként), hogy a naplóból is látszódjon, él-e a render-ciklus
    static unsigned frames;
    static uint64_t last_ms;
    frames++;
    uint64_t now = time_ms();
    if (now - last_ms >= 5000) {
        if (last_ms)
            log_info("gl: %u képkocka / %u ms, glGetError=0x%x", frames, (unsigned)(now - last_ms), glGetError());
        frames = 0;
        last_ms = now;
    }
#endif
}

void gl_exit(void) {
    if (s_display == EGL_NO_DISPLAY)
        return;
    eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (s_context != EGL_NO_CONTEXT)
        eglDestroyContext(s_display, s_context);
    if (s_surface != EGL_NO_SURFACE)
        eglDestroySurface(s_display, s_surface);
    eglTerminate(s_display);
    s_display = EGL_NO_DISPLAY;
    s_context = EGL_NO_CONTEXT;
    s_surface = EGL_NO_SURFACE;
}
