/*
 * main.c – Backstab HD (Android v1.2.8d) betöltő, Nintendo Switch (32 bites Horizon-folyamat)
 *
 * Copyright (C) 2021 Andy Nguyen, (C) 2021-2023 Rinnegatamante, (C) 2022-2023 Volodymyr Atamanenko
 * Copyright (C) 2026 Levi (Switch port)
 * MIT license, lásd LICENSE.
 */
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <FalsoJNI/FalsoJNI.h>

#include "game.h"
#include "port.h"
#include "util.h"

so_module so_mod;
void debug_hooks_install(void);
// A játék (Application::Init) a képernyő arányából és szélességéből választ sprite-készletet:
// 16:9-nél 1920 szélesen a sprites_1920x1080.gla-t, minden más szélességnél a sprites_800x480.gla-t.
// Az 1080p-s adatcsomag csak az előbbit tartalmazza, ezért 1920×1080-ban
// renderelünk; kézi módban a rendszer skálázza le 720p-re.
int screen_width = 1920, screen_height = 1080;

#define JAVA(name) "Java_com_gameloft_android_ANMP_GloftSDHM_" name

static void *sym(const char *name) {
    void *p = (void *)so_symbol(&so_mod, name);
    if (!p)
        fatal_error("Hiányzó szimbólum a libPirates.so-ban: %s", name);
    return p;
}

int main(int argc, char *argv[]) {
    log_init();
#ifdef DEBUG
    // a játék saját printf/puts/stderr-kimenete (pl. a vox hangmotoré) – Switchen különben elveszne
    freopen(DATA_PATH "stdout.txt", "w", stdout);
    setvbuf(stdout, NULL, _IOLBF, 0);
    freopen(DATA_PATH "stderr.txt", "w", stderr);
    setvbuf(stderr, NULL, _IONBF, 0);
#endif
    log_info("backstab-nx indul (main @ %p)", (void *)main);  // a tools/dev/eden-stacks.py ebből számol bázist

    if (!file_exists(SO_PATH))
        fatal_error("Nincs meg a %s.\nMásold az APK lib/armeabi-v7a/libPirates.so fájlját oda, "
                    "az adatmappát pedig a %s helyre.", SO_PATH, GAME_FILES);
    if (!file_exists(GAME_FILES "commons.gla"))
        fatal_error("Nincsenek meg a játék adatfájljai: %s", GAME_FILES);

    settings_load();
#ifdef DEBUG
    // ref800: referencia-mód – 1280×800-as képernyő, így a játék az eredeti Gameloft-csomag
    // sprites_1280x800.gla-ját tölti (a Gameloft eredeti csomagjából az SD-re másolva); a kép torzul
    if (file_exists(DATA_PATH "ref800")) {
        screen_width = 1280;
        screen_height = 800;
        log_info("ref800: 1280x800-as referencia-mód");
    }
#endif

    // diagnosztika: ha van sdmc:/switch/backstab/gltest, a játék helyett csak a GL-láncot próbáljuk
    if (file_exists(DATA_PATH "gltest")) {
        gl_init();
        for (unsigned f = 0; appletMainLoop(); f++) {
            glClearColor((f % 120) / 120.0f, 0.2f, 0.4f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            gl_swap();
        }
        gl_exit();
        return 0;
    }

    data_install();

    int rc = so_file_load(&so_mod, SO_PATH);
    if (rc < 0)
        fatal_error("A %s betöltése nem sikerült (%d).", SO_PATH, rc);
    so_relocate(&so_mod);
    resolve_imports(&so_mod);
    patch_text();
    debug_hooks_install();   // csak debug-buildben csinál valamit (debug_hooks.c)
    so_finalize(&so_mod);
    so_initialize(&so_mod);
    patch_data();
    log_info("modul kész");

    jni_init();
    audio_init();
    gl_init();
    controls_init();
    video_play_intro();   // mint Androidon a Java-indító: a motor indulása előtt

    int (*JNI_OnLoad)(void *vm, void *reserved) = sym("JNI_OnLoad");
    void (*renderer_init)(void *env, void *obj, int manufacturer, int w, int h, const char *ver) =
        sym(JAVA("GameRenderer_nativeInit"));
    void (*renderer_resize)(void *env, void *obj, int w, int h) = sym(JAVA("GameRenderer_nativeResize"));
    void (*renderer_render)(void *env, void *obj) = sym(JAVA("GameRenderer_nativeRender"));
    void (*game_init)(void *env, void *obj) = sym(JAVA("Game_nativeInit"));

    JNI_OnLoad(&jvm, NULL);
    log_info("JNI_OnLoad kész");

    renderer_init(&jni, NULL, 0, screen_width, screen_height, "1.0");
    game_init(&jni, NULL);
    renderer_resize(&jni, NULL, screen_width, screen_height);
    log_info("nativeInit kész, render-ciklus indul");

    while (appletMainLoop()) {
        controls_poll();
        renderer_render(&jni, NULL);
        gl_swap();
    }

    log_info("kilépés");
    audio_exit();
    gl_exit();
    return 0;
}
