/*
 * debug_hooks.c – csak debug-build: a játék grafikai paramétereinek naplózása (köd, látótáv)
 *
 * A köd a pályák Lua-szkriptjeiből jön (SetFogDistance/SetFogColor), a shaderek FogStartEnd/FogColor
 * uniformokkal számolják (a köd erőssége FogColor.a-ra vágva). Itt csak megnézzük, mit kap a játék.
 * Prológusok (pirates.dis):
 *   CLevel::SetFogDistance(float,float):          ldr r0, [pc, #60]; push {lr}
 *   CLevel::SetFogColor(SColor const&):            mov r3, r1; ldr r1, [pc, #24]
 *   CLevel::SetFogColorInterpolator(vec4 const&,int): movw r3, #9336; movw ip, #9252
 *   CCameraObject::SetViewDistance(float):          push {r4, lr}; ldr r3, [r0, #532]
 *   GS_Loading::Render():                           push {r4-fp, lr}; ldr r4, [pc, #1164]
 *   Application::CheckLoadLevel():                  push {r4, r5, r6, lr}; ldr r3, [r0, #92]
 *
 * tiptest: a töltőképernyő tippje helyett mindig a fájlban megadott szövegsorszám (MAIN_en.bin) – pl.
 * 1536 = „Wanted level” a koponyaikonnal. A GS_Loading::Create a +56 (érintéses) és +60 (Xperia/gamepad,
 * dpad_open) mezőbe sorsol; mindkettőt felülírjuk.
 *
 * leveltest: pályabejárás (a végigjátszhatóság füstpróbája) – a fájlban „<kezdő index> <másodperc>”, vagy
 * lista: „idx:mp idx:mp …” (pl. „10:40 25:600”). A mentést kézzel kell betölteni (Continue → Save 1 → Continue);
 * mentés nélkül, a menüből indítva a CheckLoadLevel a NULL CQuestManager-en elszáll.
 * Amint a játékmenet fut (a mentés betöltése után), sorban betölti a GS_BaseMenu::m_levelsInfo pályáit
 * (a *_menu háttérpályák és a level_9xx tesztpályák nélkül), mindegyiket ennyi másodperc játékmenet után
 * váltja. A betöltés a játék saját útja: a Lua RequireLoadLevel az Application+0x5c-be írja a pálya
 * indexét (+0x60 a belépési pont, -1 = a pálya alapértelmezett belépése; a 0 egy tetszőleges spawn-pont, amivel
 * egyes pályákon a játékos minden zónán kívül, az „égben” jelent meg), az Application::CheckLoadLevel tölt – közben a globális adatokat menti
 * és új checkpointot állít, ezért a mentéseket (files/ …save….dat, settings.dat) előtte el kell tenni.
 * Bejegyzés: 296 bájt, +0x50 a pálya fájlneve. Minden pályán képkockákat ment (shots/lt<idx>_<n>.rgba, a gépen:
 * tools/dev/shot2png.py). A mentés betöltése az autoinput-tal (controls.c) gépelés nélkül is megy.
 *
 * Copyright (C) 2026 Levi (Switch port), MIT license.
 */
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "game.h"
#include "port.h"
#include "util.h"

void debug_hooks_install(void);

#ifdef DEBUG
static float f32(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }

static so_hook h_fog_dist, h_fog_color, h_fog_interp, h_view_dist;

static int fogtest = -1;   // sdmc:/switch/backstab/fogtest: erős piros köd – látszik-e egyáltalán köd

static int fog_test(void) {
    if (fogtest < 0)
        fogtest = file_exists(DATA_PATH "fogtest");
    return fogtest;
}

static uint32_t u32f(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static void SetFogDistance(void *self, uint32_t start, uint32_t end) {
    log_info("[gfx] CLevel::SetFogDistance(%.1f, %.1f)", (double)f32(start), (double)f32(end));
    if (fog_test()) {
        start = u32f(100.0f);
        end = u32f(900.0f);
    }
    SO_CONTINUE(void (*)(void *, uint32_t, uint32_t), h_fog_dist, self, start, end);
}

static void SetFogColor(void *self, const uint32_t *argb) {
    log_info("[gfx] CLevel::SetFogColor(0x%08x)", argb ? (unsigned)*argb : 0u);
    static const uint32_t red = 0xFFFF0000;
    if (fog_test())
        argb = &red;
    SO_CONTINUE(void (*)(void *, const uint32_t *), h_fog_color, self, argb);
}

static void SetFogColorInterpolator(void *self, const float *v, int t) {
    log_info("[gfx] CLevel::SetFogColorInterpolator(%.2f %.2f %.2f %.2f, %d)", (double)v[0], (double)v[1],
             (double)v[2], (double)v[3], t);
    SO_CONTINUE(void (*)(void *, const float *, int), h_fog_interp, self, v, t);
}

static void SetViewDistance(void *self, uint32_t dist) {
    log_info("[gfx] CCameraObject::SetViewDistance(%.1f)", (double)f32(dist));
    SO_CONTINUE(void (*)(void *, uint32_t), h_view_dist, self, dist);
}

static so_hook h_loading_render;
static int tiptest = -2;   // -2: még nem olvastuk; -1: nincs
static int tip_list[16], tip_n, tip_pos = -1;
static void *tip_screen;   // a mostani GS_Loading (új töltőképernyőn a lista következő eleme)
static int tip_frames;

// tiptest: egy vagy több MAIN-szövegsorszám („1510 1514 1515”) – töltőképernyőnként a következő; a 40. képkockán
// képet ment (shots/tip_<sorszám>.rgba)
static void GS_Loading_Render(void *self) {
    if (tiptest == -2) {
        tiptest = -1;
        FILE *f = fopen(DATA_PATH "tiptest", "r");
        if (f) {
            while (tip_n < 16 && fscanf(f, "%d", &tip_list[tip_n]) == 1)
                tip_n++;
            fclose(f);
            if (tip_n)
                tiptest = tip_list[0];
            log_info("[tiptest] %d töltőképernyő-tipp", tip_n);
        }
    }
    if (tiptest >= 0) {
        if (self != tip_screen) {
            tip_screen = self;
            tip_pos = (tip_pos + 1) % tip_n;
            tiptest = tip_list[tip_pos];
            tip_frames = 0;
            log_info("[tiptest] tipp: %d", tiptest);
        }
        *(int *)((char *)self + 56) = tiptest;
        *(int *)((char *)self + 60) = tiptest;
        if (++tip_frames == 40) {
            char name[32];
            snprintf(name, sizeof(name), "tip_%d", tiptest);
            gl_debug_shot(name);
        }
    }
    SO_CONTINUE(void (*)(void *), h_loading_render, self);
}

static so_hook h_check_load;
static int leveltest = -2;         // -2: még nem olvastuk; -1: nincs; különben a következő pálya indexe
static int lt_secs = 25;
static int lt_list[32], lt_list_secs[32], lt_list_n, lt_list_pos;   // „idx:mp idx:mp …” lista-mód
static int lt_cur_secs = 25;
static int lt_cur = -1;            // a mostani pálya indexe (-1: még a mentés pályája)
static uint64_t lt_play_ms;        // mióta fut a mostani pálya játékmenete (0: még nem)
static uint64_t lt_req_ms;         // mikor kértük a mostani pályát
static int lt_shot;                // hány képkockát mentettünk a mostani pályán (shots/lt<idx>_<n>.rgba)

// a heap állapota (a pályaváltásonkénti szivárgás kereséséhez)
static void lt_mem(const char *what) {
    extern char *fake_heap_start, *fake_heap_end;
    struct mallinfo mi = mallinfo();
    unsigned lr, lu;
    heap_stats(&lr, &lu);
    log_info("[leveltest] memória (%s): kicsik %u MB foglalt + %u MB lyuk (aréna %u MB); nagyok %u MB foglalt, "
             "régió %u MB; heap %u MB", what, (unsigned)(mi.uordblks >> 20), (unsigned)(mi.fordblks >> 20),
             (unsigned)(mi.arena >> 20), lu, lr, (unsigned)((fake_heap_end - fake_heap_start + lr * 0x100000u) >> 20));
}

static const char *lt_name(int idx) {   // a tábla maga a globális (nem mutató)
    const char *info = (const char *)so_symbol(&so_mod, "_ZN11GS_BaseMenu12m_levelsInfoE");
    return info ? info + idx * 296 + 0x50 : "?";
}

static bool lt_testable(int idx) {
    const char *n = lt_name(idx);
    return !strstr(n, "_menu") && strncmp(n, "level_9", 7) != 0;
}

static void Application_CheckLoadLevel(void *app) {
    if (leveltest == -2) {
        leveltest = -1;
        FILE *f = fopen(DATA_PATH "leveltest", "r");
        if (f) {
            char buf[512] = "";
            size_t got = fread(buf, 1, sizeof(buf) - 1, f);
            buf[got] = 0;
            fclose(f);
            if (strchr(buf, ':')) {
                for (char *t = strtok(buf, " \t\r\n"); t && lt_list_n < 32; t = strtok(NULL, " \t\r\n"))
                    if (sscanf(t, "%d:%d", &lt_list[lt_list_n], &lt_list_secs[lt_list_n]) == 2)
                        lt_list_n++;
                leveltest = 0;
                log_info("[leveltest] lista: %d pálya", lt_list_n);
            } else {
                int start = 0, secs = 25;
                int n = sscanf(buf, "%d %d", &start, &secs);
                leveltest = n >= 1 ? start : 0;
                if (n >= 2 && secs > 0)
                    lt_secs = secs;
                log_info("[leveltest] kezdés: %d, %d mp/pálya", leveltest, lt_secs);
            }
        }
    }
    if (leveltest >= 0 && *(int *)((char *)app + 0x5c) < 0) {
        static void *(*current_state)(void *);
        if (!current_state)
            current_state = (void *(*)(void *))so_symbol(&so_mod, "_ZN12gxStateStack12CurrentStateEv");
        void *st = current_state((char *)app + 4);
        bool playing = st && ((int (*)(void *, int))(*(void ***)st)[2])(st, 2);
        uint64_t now = time_ms();
        if (playing && !lt_play_ms) {
            lt_play_ms = now;
            if (lt_cur >= 0)
                log_info("[leveltest] %d %s: játékmenet (betöltés %llu mp)", lt_cur, lt_name(lt_cur),
                         (unsigned long long)(now - lt_req_ms) / 1000);
            lt_mem("játékmenet");
        }
        // képkocka-mentés: a mentés pályáján 3 mp után egyszer, a tesztpályákon 8 mp után, majd 20 mp-enként (max. 6)
        uint64_t shot_ms = lt_cur < 0 ? 3000 : 8000 + (uint64_t)lt_shot * 20000;
        if (lt_play_ms && lt_shot < (lt_cur < 0 ? 1 : 6) && now - lt_play_ms > shot_ms) {
            char name[32];
            snprintf(name, sizeof(name), "lt%02d_%d", lt_cur, lt_shot);
            gl_debug_shot(name);
            lt_shot++;
            log_info("[leveltest] kép: %s %s", name, lt_cur >= 0 ? lt_name(lt_cur) : "mentés");
        }
        bool stuck = lt_cur >= 0 && !lt_play_ms && now - lt_req_ms > 180000;
        if (stuck)
            log_error("[leveltest] %d %s: 180 mp alatt nem jutott játékmenetbe (állapot vtábla %p)", lt_cur,
                      lt_name(lt_cur), st ? *(void **)st : NULL);
        // a mentés pályáján (lt_cur < 0) csak pár másodpercet várunk
        uint64_t wait_ms = lt_cur < 0 ? 5000 : (uint64_t)lt_cur_secs * 1000;
        if ((lt_play_ms && now - lt_play_ms > wait_ms) || stuck) {
            int count = *(int *)so_symbol(&so_mod, "_ZN11GS_BaseMenu12m_levelCountE");
            if (lt_cur < 0)   // a tábla a mentés betöltésekor már kész
                for (int i = 0; i < count; i++)
                    log_info("[leveltest]   %2d: %s%s", i, lt_name(i), lt_testable(i) ? "" : " (kihagyva)");
            int next = -1;
            if (lt_list_n) {
                if (lt_list_pos < lt_list_n) {
                    next = lt_list[lt_list_pos];
                    lt_cur_secs = lt_list_secs[lt_list_pos++];
                }
            } else {
                while (leveltest < count && !lt_testable(leveltest))
                    leveltest++;
                if (leveltest < count) {
                    next = leveltest++;
                    lt_cur_secs = lt_secs;
                }
            }
            if (next < 0 || next >= count) {
                log_info("[leveltest] kész");
                leveltest = -1;
            } else {
                lt_cur = next;
                lt_play_ms = 0;
                lt_shot = 0;
                lt_req_ms = now;
                log_info("[leveltest] %d %s: betöltés", lt_cur, lt_name(lt_cur));
                *(int *)((char *)app + 0x5c) = lt_cur;
                *(int *)((char *)app + 0x60) = -1;   // mint a Lua RequireLoadLevel 2. paraméter nélkül
            }
        }
    }
    SO_CONTINUE(void (*)(void *), h_check_load, app);
}

// A játék eseményei (EventManager::raiseSync: { vtábla, azonosító, … }) játékmenet közben – az irányítás
// próbájához: melyik gomb milyen eseményt vált ki. A sűrű érintés- (4) és kar-eseményeket (10, 15) kihagyjuk,
// az ismétlődő azonosítót 300 ms-on belül egyszer.
//   EventManager::raiseSync(IEvent const&): push {r4-fp, lr}; ldr r4, [pc, #460]
static so_hook h_raise_sync;
static int ev_lines;

static void EventManager_raiseSync(void *mgr, const int *ev) {
    static void *(*get_level)(void);
    static int last_id = -1;
    static uint64_t last_ms;
    if (!get_level)
        get_level = (void *(*)(void))so_symbol(&so_mod, "_ZN6CLevel8GetLevelEv");
    int id = ev ? ev[1] : -1;
    if (ev_lines < 3000 && id != 4 && id != 10 && id != 15 && get_level && get_level()) {
        uint64_t now = time_ms();
        if (id != last_id || now - last_ms > 300) {
            ev_lines++;
            log_info("[ev] %d (vtábla 0x%x) +8: %d", id, (unsigned)((uintptr_t)*(void *const *)ev - so_mod.base - 8),
                     ev[2]);
        }
        last_id = id;
        last_ms = now;
    }
    SO_CONTINUE(void (*)(void *, const int *), h_raise_sync, mgr, ev);
}

static so_hook hook_sym(const char *name, void *dst) {
    uintptr_t a = so_symbol(&so_mod, name);
    if (!a)
        fatal_error("debug_hooks: hiányzó szimbólum: %s", name);
    return hook_addr(&so_mod, a, (uintptr_t)dst);
}

void debug_hooks_install(void) {
    h_fog_dist = hook_sym("_ZN6CLevel14SetFogDistanceEff", SetFogDistance);
    h_fog_color = hook_sym("_ZN6CLevel11SetFogColorERKN6glitch5video6SColorE", SetFogColor);
    h_fog_interp = hook_sym("_ZN6CLevel23SetFogColorInterpolatorERKN6glitch4core8vector4dIfEEi", SetFogColorInterpolator);
    h_view_dist = hook_sym("_ZN13CCameraObject15SetViewDistanceEf", SetViewDistance);
    h_loading_render = hook_sym("_ZN10GS_Loading6RenderEv", GS_Loading_Render);
    h_check_load = hook_sym("_ZN11Application14CheckLoadLevelEv", Application_CheckLoadLevel);
    if (file_exists(DATA_PATH "evlog"))
        h_raise_sync = hook_sym("_ZN12EventManager9raiseSyncERK6IEvent", EventManager_raiseSync);
    log_info("debug_hooks: köd/látótáv naplózás be");
}
#else
void debug_hooks_install(void) {}
#endif
