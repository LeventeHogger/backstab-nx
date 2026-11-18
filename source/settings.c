/*
 * settings.c – a loader beállításai: sdmc:/switch/backstab/config.txt
 *
 * Formátum (a Vita-port configurátorával egyező): soronként „kulcs egész”, a tört értékek
 * százszorosan (pl. „viewDistance 100” = 1.0). A '#'-tel kezdődő sor megjegyzés.
 * Ha a fájl hiányzik, az alapértékekkel megírjuk, hogy kézzel szerkeszthető legyen.
 *
 * Copyright (C) 2021 TheFloW, (C) 2022-2023 Volodymyr Atamanenko
 * Copyright (C) 2026 Levi (Switch port)
 * MIT license, lásd LICENSE.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "game.h"
#include "util.h"

#define CONFIG_PATH DATA_PATH "config.txt"

Settings settings;

static void settings_reset(void) {
    settings.fps_lock = 30;
    // -1 = a játék saját választása (CGameConfig::CalculateDevicePower), mint a gyári appban: a jni_impl.c
    // „NVIDIA Tegra X1” GPU-ja és 1785 MHz-es CPU-ja a legerősebb osztály → magas grafika és geometria.
    // 0..2 csak kézi felülírás (a Vita alapja Low/Low volt).
    settings.gfx_detail = -1;
    settings.geometry_detail = -1;
    // A játék magától 0.85 (gyenge) / 1.1 (erős gép) értéket adna; az 1.0 a 30 fps-hez biztos tartalék.
    settings.view_distance = 0.0f;   // 0 = a játék saját értéke (AutoConfig, erős eszközön 1.1)
    settings.left_deadzone = 0.11f;
    settings.right_deadzone = 0.11f;
    settings.camera_sensitivity = 100;
    settings.language = 0;   // angol; -1 = a Switch rendszernyelve
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void settings_save(void) {
    mkpath(CONFIG_PATH);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        log_error("settings: nem írható: %s", CONFIG_PATH);
        return;
    }
    fprintf(f, "# backstab-nx beállítások. Tört értékek százszorosan (100 = 1.0).\n");
    fprintf(f, "# fpsLock: 30 vagy 60 (0 = nincs korlát, vsync); gfxDetail/geometryDetail: -1 a játék választ (alap), 0 alacsony, 1 közepes, 2 magas\n");
    fprintf(f, "# viewDistance: 0 = a játék saját értéke (mint az eredetiben), vagy 40..300; leftStickDeadZone/rightStickDeadZone: 1..50\n");
    fprintf(f, "%s %d\n", "leftStickDeadZone", (int)(settings.left_deadzone * 100.f + 0.5f));
    fprintf(f, "%s %d\n", "rightStickDeadZone", (int)(settings.right_deadzone * 100.f + 0.5f));
    fprintf(f, "# cameraSensitivity: a jobb kar (kamera) erőssége %%-ban, 10..300\n");
    fprintf(f, "%s %d\n", "cameraSensitivity", settings.camera_sensitivity);
    fprintf(f, "%s %d\n", "fpsLock", settings.fps_lock);
    fprintf(f, "%s %d\n", "gfxDetail", settings.gfx_detail);
    fprintf(f, "%s %d\n", "geometryDetail", settings.geometry_detail);
    fprintf(f, "%s %d\n", "viewDistance", (int)(settings.view_distance * 100.f + 0.5f));
    fprintf(f, "# language: -1 a Switch nyelve; 0 en, 1 fr, 2 de, 3 es, 4 it, 5 pt, 6 ja, 7 ko, 8 zh (magyar nincs a játékban)\n");
    fprintf(f, "%s %d\n", "language", settings.language);
    fclose(f);
}

void settings_load(void) {
    settings_reset();

    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        log_info("settings: nincs %s, alapértékek kiírva", CONFIG_PATH);
        settings_save();
        return;
    }

    char line[128], key[64];
    int value;
    bool has_language = false, has_camera = false;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || sscanf(line, "%63s %d", key, &value) != 2)
            continue;
        if (!strcmp(key, "leftStickDeadZone"))       settings.left_deadzone = value / 100.f;
        else if (!strcmp(key, "rightStickDeadZone")) settings.right_deadzone = value / 100.f;
        else if (!strcmp(key, "cameraSensitivity"))  settings.camera_sensitivity = value, has_camera = true;
        else if (!strcmp(key, "fpsLock"))            settings.fps_lock = value;
        else if (!strcmp(key, "gfxDetail"))          settings.gfx_detail = value;
        else if (!strcmp(key, "geometryDetail"))     settings.geometry_detail = value;
        else if (!strcmp(key, "viewDistance"))       settings.view_distance = value / 100.f;
        else if (!strcmp(key, "language"))           settings.language = value, has_language = true;
        else if (strcmp(key, "enableMipMaps"))       // a Vita-configurátor kulcsa, itt nincs szerepe
            log_info("settings: ismeretlen kulcs: %s", key);
    }
    fclose(f);

    // Érvényesítés: a Vita 25/40-es korlátja itt nincs (a vsync-intervallum csak 60/30 fps-t ad).
    if (settings.fps_lock != 0 && settings.fps_lock != 60)
        settings.fps_lock = 30;
    settings.gfx_detail = clampi(settings.gfx_detail, -1, 2);
    settings.geometry_detail = clampi(settings.geometry_detail, -1, 2);
    if (settings.view_distance > 0.0f)
        settings.view_distance = clampf(settings.view_distance, 0.4f, 3.0f);   // a Vita-configurátor tartománya
    settings.left_deadzone = clampf(settings.left_deadzone, 0.01f, 0.5f);
    settings.right_deadzone = clampf(settings.right_deadzone, 0.01f, 0.5f);
    settings.language = clampi(settings.language, -1, 8);
    settings.camera_sensitivity = clampi(settings.camera_sensitivity, 10, 300);
    if (!has_language || !has_camera)
        settings_save();   // régebbi config.txt: az új kulcsok is kerüljenek bele

    log_info("settings: fps %d, gfx %d, geom %d, látótáv %.2f, holtjáték %.2f/%.2f",
             settings.fps_lock, settings.gfx_detail, settings.geometry_detail,
             (double)settings.view_distance, (double)settings.left_deadzone, (double)settings.right_deadzone);
}
