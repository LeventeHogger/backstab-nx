/*
 * game.h – a betöltött játékmodul és a modulok közti felület
 *
 * Ki mit ad:
 *   dynlib.c   resolve_imports()            – a 277 import (libc-híd: reimpl/bionic.c, GLES2: Mesa)
 *   jni_impl.c a FalsoJNI-táblák + audio.c  – Java-oldal (AudioTrack → audout)
 *   gl.c       gl_init/gl_swap              – EGL (Mesa) a nwindowGetDefault()-ra
 *   controls.c controls_init/controls_poll  – HID → érintés/billentyű a játéknak
 *   patch.c    patch_text/patch_data        – hookok és adatfoltok
 *   settings.c settings_load                – sdmc:/switch/backstab/config.txt
 *
 * Copyright (C) 2026 Levi (Switch port), MIT license.
 */
#pragma once

#include <stdbool.h>
#include "so_util.h"

extern so_module so_mod;

// A játék logikai képernyőmérete, 1920×1080 (ezt kapja a nativeInit/nativeResize; az érintés-
// koordináták is ebben). Az okát lásd main.c.
extern int screen_width, screen_height;

// dynlib.c – minden import feloldása so_resolve()-val (so_relocate után, so_finalize előtt).
void resolve_imports(so_module *mod);

// patch.c – patch_text(): hookok (hook_addr), CSAK so_finalize() ELŐTT;
//           patch_data(): adatfoltok a végleges címeken, so_initialize() UTÁN.
void patch_text(void);
void patch_data(void);

// gl.c – EGL-környezet (GLES2) a Switch alapablakára; gl_swap = eglSwapBuffers.
void gl_init(void);
void gl_swap(void);
void gl_exit(void);

// controls.c – a játék szimbólumainak feloldása (so_finalize után) és képkockánkénti bevitel.
void controls_init(void);
void controls_poll(void);

// audio.c – hangkimenet (audout); a JNI AudioTrack-metódusai hívják.
void audio_init(void);
void audio_exit(void);

// settings.c
typedef struct {
    int fps_lock;              // 30 (alap) vagy 60; 0 = nincs korlát (vsync)
    int gfx_detail;            // -1 = a játék választ (alap), 0 = alacsony, 1 = közepes, 2 = magas
    int geometry_detail;       // -1..2, mint fent
    float view_distance;       // 0.5..1.0
    float left_deadzone, right_deadzone;
    int camera_sensitivity;    // jobb kar (kamera) erőssége %-ban, alap 100
    int language;              // 0..8 = en fr de es it pt ja ko zh (a játék sorrendje), alap 0; -1 = a Switch nyelve
} Settings;
extern Settings settings;
void settings_load(void);
