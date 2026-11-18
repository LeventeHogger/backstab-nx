/*
 * patch.c – a játék foltjai: hookok (patch_text) és adatfoltok (patch_data)
 *
 * A Vita-port patch.c + patch/{graphics,controls}.c megfelelője. A Vita splash.c-je (javított
 * nyitó- és töltőkép) kimaradt: az a 800×480-as sprite-készlet gyenge képét pótolta, mi viszont
 * 1920×1080-ban futunk (lásd main.c), annak saját képe éles. Két különbség:
 *  - A kód leképezés (so_finalize) után nem írható, ezért minden hook a patch_text()-ben készül,
 *    a Vita futásidejű so_unhook-ja helyett a hook egy kapcsolóval adja tovább a hívást.
 *  - Az eredetit a trampolinon át hívjuk, pontos függvénytípussal (ORIG): a so_util.h
 *    SO_CONTINUE makrójának „type (*)()” kasztja C23-ban (a GCC 16 alapértéke) paraméter nélküli
 *    prototípust jelent, így argumentummal nem fordul.
 *
 * Minden hookolt függvény ARM-módú, és az első két utasítása áthelyezhető (sima utasítás vagy
 * „ldr rX, [pc, #imm]”) – a pirates.dis alapján ellenőrizve, lásd a hookok mellett.
 *
 * Copyright (C) 2023 Volodymyr Atamanenko
 * Copyright (C) 2026 Levi (Switch port)
 * MIT license, lásd LICENSE.
 */
#include <malloc.h>
#include <switch.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "game.h"
#include "port.h"
#include "util.h"

#define ORIG(fn_type, hook) ((fn_type)(hook).trampoline)

static uintptr_t game_sym(const char *name) {
    uintptr_t p = so_symbol(&so_mod, name);
    if (!p)
        fatal_error("patch: hiányzó szimbólum: %s", name);
    return p;
}

static so_hook hook(const char *name, void *dst) {
    return hook_addr(&so_mod, game_sym(name), (uintptr_t)dst);
}

// ---------------------------------------------------------------------------
// A játék saját naplója (glitch::os::Printer) → a mi naplónk
//   print: push {r0-r3}; push {lr}          – statikus, változó argumentumú
//   log:   ldr r3, [pc, #48]; mov ip, r0    – statikus (szöveg, ELOG_LEVEL)

#ifdef DEBUG
#define GAME_LOG_LIMIT 0          // nincs korlát
#else
#define GAME_LOG_LIMIT 2000       // kiadásban ennyi sor után elhallgat (minden sor SD-írás + fflush)
#endif

static int game_log_lines;

static void game_log(int level, const char *msg) {
    if (GAME_LOG_LIMIT && game_log_lines >= GAME_LOG_LIMIT)
        return;
    if (GAME_LOG_LIMIT && ++game_log_lines == GAME_LOG_LIMIT) {
        log_info("[játék] … a további játéknapló elhallgatva (DEBUG-buildben teljes)");
        return;
    }
    size_t len = strlen(msg);
    while (len && (msg[len - 1] == '\n' || msg[len - 1] == '\r'))
        len--;
    if (level < 0)
        log_printf("G", "%.*s", (int)len, msg);
    else
        log_printf("G", "[%d] %.*s", level, (int)len, msg);
}

static void Printer_print(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    game_log(-1, buf);
}

static void Printer_log(const char *text, int level) {
    game_log(level, text ? text : "(null)");
}

// ---------------------------------------------------------------------------
// Grafikai minőség (Vita patch/graphics.c)
//   CGameConfig::CalculateDevicePower: push {r4-r8, sl, lr}; ldr r5, [pc, #1080]
//   CGameConfig::AutoConfig:           push {r4, r5, r6, lr}; mov r5, r0
// A játék a készülékmodell és a GPU-név alapján állítja a CGameSettings jelzőit; utána felülírjuk.
// Az AutoConfig maga hívja a CalculateDevicePower-t (bl a hookolt belépési pontra), így a
// látótávolság-számítása már a mi jelzőinkből dolgozik, a végén pedig a látótávot is felülírjuk.

static int8_t *s_low_gfx, *s_high_gfx, *s_low_geom, *s_med_geom, *s_high_geom;
static float *s_view_distance;

// Csak a config.txt kézi felülírása (-1 = a játék saját választása marad, lásd settings.c).
static void apply_detail_settings(void) {
    // gfx: a játéknak csak „alacsony” és „magas” jelzője van; egyik sem = közepes
    if (settings.gfx_detail >= 0) {
        *s_low_gfx = settings.gfx_detail == 0;
        *s_high_gfx = settings.gfx_detail == 2;
    }
    if (settings.geometry_detail >= 0) {
        *s_low_geom = settings.geometry_detail == 0;
        *s_med_geom = settings.geometry_detail == 1;
        *s_high_geom = settings.geometry_detail == 2;
    }
}

static so_hook calc_power_hook, auto_config_hook;

static void CGameConfig_CalculateDevicePower(void *self) {
    ORIG(void (*)(void *), calc_power_hook)(self);
    apply_detail_settings();
}

// Látótáv: alapból a játék saját AutoConfig-értéke marad (erős eszközön 1.1, mint az eredetiben);
// csak a config.txt viewDistance kulcsa (> 0) írja felül.
static void CGameConfig_AutoConfig(void *self) {
    ORIG(void (*)(void *), auto_config_hook)(self);
    // CGameConfig: +0 m_iCPUProfile, +16 m_iNPC_Limit_Level, +0x444 m_iGPUProfile (GetCurrentConfig sztringjei)
    log_info("patch: AutoConfig: CPU-profil %d, GPU-profil %d, járókelő-korlát %d; gfx %s, geometria %s; "
             "látótáv-szorzó %.2f%s", *(int *)self, *(int *)((char *)self + 0x444), *(int *)((char *)self + 16),
             *s_high_gfx ? "magas" : *s_low_gfx ? "alacsony" : "közepes",
             *s_high_geom ? "magas" : *s_med_geom ? "közepes" : *s_low_geom ? "alacsony" : "?",
             (double)*s_view_distance, settings.view_distance > 0.0f ? " (felülírva)" : "");
    if (settings.view_distance > 0.0f)
        *s_view_distance = settings.view_distance;
}

// ---------------------------------------------------------------------------
// Nyelv. Androidon a Java-oldal adta a készülék nyelvét; a libPirates.so GetDeviceLanguage(char*)-ja
// csak üres sztringet ír. Az Application::InitFromLogo akkor hívja, ha még nincs mentett nyelv
// (Application+168 < 0), és ISO-kódot vár: en de fr it es ja ko zh pt (más → JNI GetPhoneLanguage).
//   GetDeviceLanguage(char*):     mov r3, #0; strb r3, [r0, #2]
//   Application::InitFromLogo():  push {r4-r8, sl, lr}; ldr r4, [pc, #920]
// A config.txt language kulcsa (0..8, a játék sorrendjében) a mentett nyelvet is felülírja.

static const char *const lang_iso[9] = { "en", "fr", "de", "es", "it", "pt", "ja", "ko", "zh" };
static so_hook init_from_logo_hook;

static const char *switch_language_iso(void) {
    u64 code = 0;
    SetLanguage lang = SetLanguage_ENUS;
    if (R_FAILED(setInitialize()))
        return "en";
    if (R_SUCCEEDED(setGetSystemLanguage(&code)))
        setMakeLanguage(code, &lang);
    setExit();
    switch (lang) {
        case SetLanguage_FR: case SetLanguage_FRCA: return "fr";
        case SetLanguage_DE: return "de";
        case SetLanguage_IT: return "it";
        case SetLanguage_ES: case SetLanguage_ES419: return "es";
        case SetLanguage_JA: return "ja";
        case SetLanguage_KO: return "ko";
        case SetLanguage_ZHCN: case SetLanguage_ZHTW: case SetLanguage_ZHHANS: case SetLanguage_ZHHANT: return "zh";
        case SetLanguage_PT: case SetLanguage_PTBR: return "pt";
        default: return "en";   // angol, holland, orosz… (a játékban nincs más)
    }
}

static void GetDeviceLanguage(char *out) {
    const char *iso = settings.language >= 0 ? lang_iso[settings.language] : switch_language_iso();
    out[0] = iso[0];
    out[1] = iso[1];
    out[2] = 0;
    log_info("patch: készüléknyelv → %s", out);
}

static void Application_InitFromLogo(void *self) {
    if (settings.language >= 0)
        *(int *)((char *)self + 168) = -1;   // a mentett nyelv helyett a GetDeviceLanguage döntsön
    ORIG(void (*)(void *), init_from_logo_hook)(self);
}

// ---------------------------------------------------------------------------
// Irányítás (Vita patch/controls.c)
//   LoadControlScheme(int):         push {r4-sl, lr}; ldr sl, [pc, #232]
//   CMapDisplay::Draw(int):         push {r4-fp, lr}; vpush {d8-d9}
//   GS_InGameMenu::Render():        push {r4-fp, lr}; vpush {d8-d9}
// A dpad_open az Xperia Play „kihúzott gamepad” jelzője: 1 esetén a HUD érintőgombjai rejtve
// maradnak (fizikai gombokkal játszunk; a koppintások helye ettől még él). A térkép és a
// játékbeli menü rajzolása alatt 0, hogy azok a megszokott elrendezéssel jelenjenek meg.

static so_hook load_control_scheme_hook, map_draw_hook, ingame_menu_render_hook;
static int8_t *dpad_open;

static void LoadControlScheme(int scheme) {
    ORIG(void (*)(int), load_control_scheme_hook)(scheme);
    *dpad_open = 1;
}

static void CMapDisplay_Draw(void *self, int param) {
    *dpad_open = 0;
    ORIG(void (*)(void *, int), map_draw_hook)(self, param);
    *dpad_open = 1;
}

static void GS_InGameMenu_Render(void *self) {
    *dpad_open = 0;
    ORIG(void (*)(void *), ingame_menu_render_hook)(self);
    *dpad_open = 1;
}

// ---------------------------------------------------------------------------
// Betűkészlet: az eredeti Gameloft-fontok 1080p-re nagyítva, a binárisba ágyazva
//   CCustomFileSystem::createAndOpenFile(char const*):   push {r4-fp, lr}; ldr r5, [pc, #2024]
//   CTextureManager::getTexture(char const*, char const*): push {r4-fp, lr}; ldr r4, [pc, #852]
// A sprites_1920x1080.gla eu_font_* fontjai nem a Gameloft eredetijei (cirill betűk, hiányzó à é ß,
// más méretarányok). A make a cache-beli Gameloft-csomagból legyártja az eredetieket (tools/assets/make-fonts.py,
// hd_ előtaggal), és a binárisba ágyazza (data_install.c: embedded_file); a játék fájlrendszere ezeket
// memóriafájlként kapja (glitch CFileSystem::createMemoryReadFile – a this-t nem használja).
// A .bsprite-ot és a fonttérképet a createAndOpenFile nyitja meg, a textúrát a CSpriteManager::LoadSprite a
// getTexture-rel kéri „eu_font_x.tga” néven: ezt hd_ névre írjuk (a textúra-gyorsítótárban se keveredjen),
// a getTexture pedig szintén a createAndOpenFile-t hívja „hd_eu_font_x.tga” névvel (a .tga → .pvr cserét
// a játék fájlrendszere végezné).
// A getTexture érték szerint ad vissza intrusive_ptr-t: r0 = az eredmény helye, r1 = this.

static bool hd_fonts;
static so_hook open_file_hook;
static void *(*createMemoryReadFile)(void *fs, void *mem, int size, const char *name, bool drop_mem);

static void *CCustomFileSystem_createAndOpenFile(void *self, const char *name) {
#ifdef DEBUG
    if (name && strstr(name, "font"))
        log_info("[fs] createAndOpenFile(%s)", name);
#endif
    if (hd_fonts && name) {
        const char *base = strrchr(name, '/');
        base = base ? base + 1 : name;
        char alt[64];
        if (strncmp(base, "eu_font_", 8) == 0) {   // .bsprite, fonttérkép
            snprintf(alt, sizeof(alt), "hd_%s", base);
            base = alt;
        } else if (strncmp(base, "hd_eu_font_", 11) == 0) {
            // textúra: a getTexture „.tga” néven kéri, a kiterjesztést a játék fájlrendszere cserélné
            // .pvr-re (laza fájlnál fopen-nel) – a memóriafájl is .pvr nevet kap, hogy a PVR-betöltő vegye
            const char *dot = strrchr(base, '.');
            if (dot && strcmp(dot, ".tga") == 0) {
                snprintf(alt, sizeof(alt), "%.*s.pvr", (int)(dot - base), base);
                base = alt;
            }
        }
        int size;
        const void *data = embedded_file(base, &size);
        if (data)
            return createMemoryReadFile(self, (void *)data, size, base, false);
    }
    return ORIG(void *(*)(void *, const char *), open_file_hook)(self, name);
}

static so_hook get_texture_hook;

static void *CTextureManager_getTexture(void *ret, void *self, const char *name, const char *alias) {
    char alt[128];
    if (hd_fonts && name) {
        const char *base = strrchr(name, '/');
        base = base ? base + 1 : name;
        if (strncmp(base, "eu_font_", 8) == 0 && strlen(name) + 4 < sizeof(alt)) {
            snprintf(alt, sizeof(alt), "%.*shd_%s", (int)(base - name), name, base);
            name = alt;
        }
    }
    return ORIG(void *(*)(void *, void *, const char *, const char *), get_texture_hook)(ret, self, name, alias);
}

static void font_hooks(void) {
    int size;
    hd_fonts = embedded_file("hd_eu_font_map.bin", &size) != NULL;
    log_info("patch: betűkészlet: %s", hd_fonts ? "eredeti (beágyazott hd_eu_font_*)"
                                                : "a csomagé (a build nem tartalmazza, lásd Makefile: FONT_PACK)");
    if (hd_fonts) {
        createMemoryReadFile = (__typeof__(createMemoryReadFile))game_sym(
            "_ZN6glitch2io11CFileSystem20createMemoryReadFileEPviPKcb");
        open_file_hook = hook("_ZN17CCustomFileSystem17createAndOpenFileEPKc", CCustomFileSystem_createAndOpenFile);
        get_texture_hook = hook("_ZN6glitch5video15CTextureManager10getTextureEPKcS3_", CTextureManager_getTexture);
    }
}

// ---------------------------------------------------------------------------
// Menügombok kirajzolása (controls.c, „Kirajzolt gombok”): a menü-navigáció csak a valóban kirajzolt
// gombokra lép. Prológusok (pirates.dis):
//   CButtonSpr::Draw(int):     push {r4-fp, lr}; vpush {d8}
//   CButtonText::Draw(int):    push {r4-r8, lr}; ldrb r3, [r0, #136]
//   CButtonAnim::Draw(int):    push {r4-fp, lr}; vpush {d8}
//   CTab::Draw(int):           push {r4-fp, lr}; ldrb r3, [r0, #200]
//   CDialogTextBox::Draw(int): push {r4-r7, lr}; sub sp, sp, #52
//   CSlider::Draw(int):        push {r4-sl, lr}; ldrb r6, [r0, #18]
// (A CButton::Draw egyetlen „bx lr” – nem hookolható, és úgysem rajzol.)
#define DRAW_HOOK(name)                                                        \
    static so_hook draw_h_##name;                                              \
    static void draw_##name(void *self, int arg) {                             \
        controls_note_drawn(self);                                             \
        ORIG(void (*)(void *, int), draw_h_##name)(self, arg);                 \
    }
DRAW_HOOK(spr)
DRAW_HOOK(text)
DRAW_HOOK(anim)
DRAW_HOOK(tab)
DRAW_HOOK(dialog)
DRAW_HOOK(slider)

static void draw_hooks(void) {
    draw_h_spr = hook("_ZN10CButtonSpr4DrawEi", draw_spr);
    draw_h_text = hook("_ZN11CButtonText4DrawEi", draw_text);
    draw_h_anim = hook("_ZN11CButtonAnim4DrawEi", draw_anim);
    draw_h_tab = hook("_ZN4CTab4DrawEi", draw_tab);
    draw_h_dialog = hook("_ZN14CDialogTextBox4DrawEi", draw_dialog);
    draw_h_slider = hook("_ZN7CSlider4DrawEi", draw_slider);
}

// ---------------------------------------------------------------------------

#ifdef DEBUG
// ---------------------------------------------------------------------------
// Diagnosztika (csak debug-build): a vox hangmotor hívásainak naplózása. A VoxSoundManager::Play*
// EmitterHandle-t ad vissza érték szerint, így r0 = a visszatérési hely, r1 = this, r2 = az első
// valódi argumentum (index vagy név). Az argumentumokat 10 szóként változatlanul adjuk tovább.
typedef uint32_t (*w10_fn)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                           uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
#define VOX_HOOK(name, sym, by_name)                                                            \
    static so_hook vox_h_##name;                                                                \
    static uint32_t vox_##name(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,  \
                               uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8, uint32_t a9) { \
        uint32_t r = ORIG(w10_fn, vox_h_##name)(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9);       \
        if (by_name)                                                                            \
            log_info("[vox] " #name "(%s) -> id %d", a2 ? (const char *)a2 : "(null)",          \
                     a0 ? *(int *)a0 : -1);                                                     \
        else                                                                                    \
            log_info("[vox] " #name "(%d) -> id %d", (int)a2, a0 ? *(int *)a0 : -1);            \
        return r;                                                                               \
    }
VOX_HOOK(Play_i, "_ZN15VoxSoundManager4PlayEiNS_6E_LOOPEi", 0)
VOX_HOOK(Play_s, "_ZN15VoxSoundManager4PlayEPKcNS_6E_LOOPEi", 1)
VOX_HOOK(Play3D_i, "_ZN15VoxSoundManager6Play3DEiRKN6glitch4core8vector3dIfEEbffif", 0)
VOX_HOOK(Play3D_s, "_ZN15VoxSoundManager6Play3DEPKcRKN6glitch4core8vector3dIfEEbff", 1)
VOX_HOOK(Play3D_si, "_ZN15VoxSoundManager6Play3DEPKciRKN6glitch4core8vector3dIfEEbff", 1)
VOX_HOOK(PlayEvent, "_ZN15VoxSoundManager9PlayEventEPKcRKN6glitch4core8vector3dIfEEff", 1)
VOX_HOOK(PlayUninterruptible, "_ZN15VoxSoundManager19PlayUninterruptibleEPKcNS_6E_LOOPEi", 1)

static so_hook vox_h_load;
static int vox_LoadSound(void *self, int idx) {
    int r = ORIG(int (*)(void *, int), vox_h_load)(self, idx);
    log_info("[vox] LoadSound(%d) -> %d", idx, r);
    return r;
}

static void vox_debug_hooks(void) {
    vox_h_Play_i = hook("_ZN15VoxSoundManager4PlayEiNS_6E_LOOPEi", vox_Play_i);
    vox_h_Play_s = hook("_ZN15VoxSoundManager4PlayEPKcNS_6E_LOOPEi", vox_Play_s);
    vox_h_Play3D_i = hook("_ZN15VoxSoundManager6Play3DEiRKN6glitch4core8vector3dIfEEbffif", vox_Play3D_i);
    vox_h_Play3D_s = hook("_ZN15VoxSoundManager6Play3DEPKcRKN6glitch4core8vector3dIfEEbff", vox_Play3D_s);
    vox_h_Play3D_si = hook("_ZN15VoxSoundManager6Play3DEPKciRKN6glitch4core8vector3dIfEEbff", vox_Play3D_si);
    vox_h_PlayEvent = hook("_ZN15VoxSoundManager9PlayEventEPKcRKN6glitch4core8vector3dIfEEff", vox_PlayEvent);
    vox_h_PlayUninterruptible = hook("_ZN15VoxSoundManager19PlayUninterruptibleEPKcNS_6E_LOOPEi", vox_PlayUninterruptible);
    vox_h_load = hook("_ZN15VoxSoundManager9LoadSoundEi", vox_LoadSound);
}
#endif

// ---------------------------------------------------------------------------
// Zóna-láthatóság: NULL-védelem
//   CZone::SetVisible(SViewFrustum const*, CZonePortal const*): push {r4-fp, lr}; sub sp, sp, #580
// A portálokon át rekurzívan a túloldali zónát hívja (CZonePortal +0xa8/+0xac – amelyik nem ő maga),
// NULL-ellenőrzés nélkül; az első utasítása már a this+0x2b0-at olvassa. Pályabejárás közben egyszer
// NULL zónát kapott (a level_007 elején) és elszállt. A védelem semmi mást nem változtat: NULL
// zónán át nincs mit láthatóvá tenni.

static so_hook zone_visible_hook;

static void CZone_SetVisible(void *self, const void *frustum, const void *portal) {
    if (!self) {
        static int logged;
        if (logged++ < 3)
            log_info("patch: CZone::SetVisible NULL zónára (portál %p) – kihagyva", portal);
        return;
    }
    ORIG(void (*)(void *, const void *, const void *), zone_visible_hook)(self, frustum, portal);
}

void patch_text(void) {
    // Itt még csak címeket oldunk fel: a végleges címek (so_symbol) a leképezés után élnek,
    // ezért adatot csak a hookokban és a patch_data()-ban írunk rajtuk át.
    s_low_gfx = (int8_t *)game_sym("_ZN13CGameSettings22s_hasLowPerformanceGfxE");
    s_high_gfx = (int8_t *)game_sym("_ZN13CGameSettings23s_hasHighPerformanceGfxE");
    s_low_geom = (int8_t *)game_sym("_ZN13CGameSettings23s_hasLowPerformanceGeomE");
    s_med_geom = (int8_t *)game_sym("_ZN13CGameSettings23s_hasMedPerformanceGeomE");
    s_high_geom = (int8_t *)game_sym("_ZN13CGameSettings24s_hasHighPerformanceGeomE");
    s_view_distance = (float *)game_sym("_ZN13CGameSettings20s_viewDistanceFactorE");
    dpad_open = (int8_t *)game_sym("dpad_open");

    hook("_ZN6glitch2os7Printer5printEPKcz", Printer_print);
    hook("_ZN6glitch2os7Printer3logEPKcNS_10ELOG_LEVELE", Printer_log);

#ifdef DEBUG
    vox_debug_hooks();
#endif
    hook("_Z17GetDeviceLanguagePc", GetDeviceLanguage);
    init_from_logo_hook = hook("_ZN11Application12InitFromLogoEv", Application_InitFromLogo);

    calc_power_hook = hook("_ZN11CGameConfig20CalculateDevicePowerEv", CGameConfig_CalculateDevicePower);
    auto_config_hook = hook("_ZN11CGameConfig10AutoConfigEv", CGameConfig_AutoConfig);


    load_control_scheme_hook = hook("_Z17LoadControlSchemei", LoadControlScheme);
    map_draw_hook = hook("_ZN11CMapDisplay4DrawEi", CMapDisplay_Draw);
    ingame_menu_render_hook = hook("_ZN13GS_InGameMenu6RenderEv", GS_InGameMenu_Render);
    draw_hooks();
    font_hooks();
    zone_visible_hook = hook("_ZN5CZone10SetVisibleEPKN6glitch5scene12SViewFrustumEPK11CZonePortal", CZone_SetVisible);

    log_info("patch: hookok kész");
}

void patch_data(void) {
    // DRM-kerülés: a játék a lockPointer1..4 által mutatott int-et ellenőrzi (1 = rendben)
    static const char *const locks[] = { "lockPointer1", "lockPointer2", "lockPointer3", "lockPointer4" };
    for (size_t i = 0; i < sizeof(locks) / sizeof(locks[0]); i++) {
        int **lock = (int **)game_sym(locks[i]);
        int *ok = malloc(sizeof(int));
        if (!ok)
            fatal_error("patch: kevés a memória");
        *ok = 1;
        *lock = ok;
    }

    // Kezdőértékek; a játék AutoConfig-ja felülírná, ezért a hookok újra beállítják.
    apply_detail_settings();
    if (settings.view_distance > 0.0f)
        *s_view_distance = settings.view_distance;

    log_info("patch: adatfoltok kész");
}
