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
// a CFont::DrawString horgonyai (J2ME-szerű bitek; a menücím 17 = vízszintesen középre | fent)
// CFont::DrawString horgonybitjei (a függvény ezeket vizsgálja): vízszintesen 1 közép, 2 jobb (alap: bal),
// függőlegesen 16 közép, 32 alul (alap: felül)
enum { ANCHOR_LEFT = 0, ANCHOR_TOP = 0, ANCHOR_HCENTER = 1, ANCHOR_RIGHT = 2, ANCHOR_VCENTER = 16, ANCHOR_BOTTOM = 32 };
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
// A sprites_1920x1080.gla eu_font_* fontjai nem a Gameloft eredetijei (hiányzó à é ß,
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
DRAW_HOOK(slider)

// Gombjelek a játék saját feliratain (controls.c: controls_button_glyph). A gomb felirata
// a rajzolás idejére „\x02<képkocka> ” előtagot kap: a játék ezt szövegbe ágyazott ikonként, a gomb saját
// betűjével és igazításával rajzolja; utána a felirat-mutató visszaáll (a játék objektumában tartósan semmi
// sem változik). A párbeszéd-dobozra (átvezetőben) a jobb szélére, a fülsor szélső füleire (L/R) alájuk
// rajzolunk jelet.
//   CFont::DrawString(text, x, y, anchor, alpha, 0, 0x10000, NULL) – logikai képpontban (480×320)
//   CFont::GetStringSize(text, &w, &h, NULL)
//   CSpriteManager::GetFont(név) – a GS_BaseMenu::DrawMenuTitle-ben is így (s_FontBig/Medium/Small)
// A feliratok helye osztályonként (a Draw-ok DrawString-hívásai szerint): CButtonSpr szöveg +0x88, betű
// +0x84; CButtonAnim +0x98, +0x9c; CButtonText +0x80, +0x84. Szöveg nélküli gombra (a vissza-nyíl) csak a
// B kerül, magában.
#define BTN_RECT    0x08          // short x0, y0, x1, y1 (logikai)
#define DIALOG_GLYPH_MARGIN 12   // a párbeszéd B-je a doboz jobb szélétől (logikai)

static void (*CFont_DrawString)(void *font, const uint16_t *text, int x, int y, int anchor, uint8_t alpha,
                                int unk, int scale, uint16_t *out);
static void (*CFont_GetStringSize)(void *font, const uint16_t *text, int *w, int *h, int *out);
static void *(*SpriteManager_GetFont)(void *mgr, const char *name);
static void **sprite_manager;
static const char *font_small;
static int *logical_width;

static void draw_glyph(uint16_t glyph, int x, int y, int anchor) {
    void *font = *sprite_manager ? SpriteManager_GetFont(*sprite_manager, font_small) : NULL;
#ifdef DEBUG
    static int logged;
    if (logged++ < 4)
        log_info("patch: gombjel %d (%d,%d) horgony %d, font %p (%s)", glyph, x, y, anchor, font, font_small);
#endif
    if (!font)
        return;
    const uint16_t s[3] = { 2, glyph, 0 };
    CFont_DrawString(font, s, x, y, anchor, 255, 0, 0x10000, NULL);
}

// A képernyő jobb széléig érő CButtonSpr (a mentésválasztó „Continue”-ja, az átvezető Skip-sávja) előtagos
// felirata a CFont::DrawString-hookban kerül a helyére (lásd lent): a gomb és a háttere nem mozdul. A draw_labeled
// rajzolása alatt érvényes.
static const uint16_t *edge_text, *edge_orig;

static void draw_labeled(so_hook *h, void *self, int arg, int text_off, bool edge_shift) {
    controls_note_drawn(self);
    uint16_t glyph = controls_button_glyph(self);
    const uint16_t **text = (const uint16_t **)((char *)self + text_off);
    const uint16_t *orig = *text;
    bool has_text = orig && orig[0];
    if (!glyph || (!has_text && glyph != GLYPH_B)) {
        ORIG(void (*)(void *, int), *h)(self, arg);
        return;
    }
    uint16_t buf[80];
    int n = 0;
    buf[n++] = 2;
    buf[n++] = glyph;
    if (has_text) {
        buf[n++] = ' ';
        for (int i = 0; orig[i] && n < (int)(sizeof(buf) / sizeof(buf[0])) - 1; i++)
            buf[n++] = orig[i];
    }
    buf[n] = 0;
    const int16_t *r = (const int16_t *)((char *)self + BTN_RECT);
    if (edge_shift && has_text && r[2] > *logical_width) {
        edge_text = buf;
        edge_orig = orig;
    }
    *text = buf;
    ORIG(void (*)(void *, int), *h)(self, arg);
    *text = orig;
    edge_text = edge_orig = NULL;
}

//   CFont::DrawString: push {r4-fp, lr}; vpush {d8}
// Az előtagos szél-felirat (edge_text) a képen maradjon, legalább 10 egységre a széltől, ahogy a játék a saját
// Skip-/Continue-feliratát igazítja (CButtonSpr::Draw: x = szélesség − 10, jobbra igazítva). A jobbra igazított
// szövegbe ágyazott ikonnal a DrawString elszámolja magát (a felirat az ikon szélességével jobbra csúszik, és
// lelóg), ezért ott a szöveg változatlanul, a játék helyén marad, az ikon pedig külön kerül elé.
#define EDGE_MARGIN 10
static so_hook drawstring_hook;
typedef void (*drawstring_fn)(void *, const uint16_t *, int, int, int, uint8_t, int, int, uint16_t *);

static int text_width_px(void *font, const uint16_t *text) {
    int w = 0, h = 0, w2 = 0;
    CFont_GetStringSize(font, text, &w, &h, &w2);   // képernyő-képpontban
    return w;
}

static void CFont_DrawString_hook(void *font, const uint16_t *text, int x, int y, int anchor, uint8_t alpha, int unk,
                                  int scale, uint16_t *out) {
    const drawstring_fn orig = ORIG(drawstring_fn, drawstring_hook);
    if (text && text == edge_text && font && screen_width > 0 && *logical_width > 0) {
        const int sw = screen_width, lw = *logical_width;
        if (anchor & ANCHOR_RIGHT) {
            orig(font, edge_orig, x, y, anchor, alpha, unk, scale, out);
            const uint16_t space[2] = { ' ', 0 }, icon[3] = { 2, text[1], 0 };
            // (az ikon a mértnél kb. egy szóközzel szélesebben rajzolódik: két szóköz a rés)
            const int left_px = x * sw / lw - text_width_px(font, edge_orig) - 2 * text_width_px(font, space);
            orig(font, icon, (left_px * lw + sw / 2) / sw, y, anchor, alpha, unk, scale, out);
            return;
        }
        // balra/középre igazítva az ikon a szöveg elején jó helyen van; ha lelógna, balra visszük
        const int px = x * sw / lw, w = text_width_px(font, text), w0 = text_width_px(font, edge_orig);
        const int r0 = anchor & ANCHOR_HCENTER ? px + w0 / 2 : px + w0;
        const int r1 = anchor & ANCHOR_HCENTER ? px + w / 2 : px + w;
        const int edge = sw - EDGE_MARGIN * sw / lw, limit = r0 > edge ? r0 : edge;
        if (r1 > limit)
            x -= ((r1 - limit) * lw + sw - 1) / sw;
    }
    orig(font, text, x, y, anchor, alpha, unk, scale, out);
}

static so_hook draw_h_spr, draw_h_anim, draw_h_text, draw_h_dialog, draw_h_tab;

// (a jobb szélre lógó feliratot csak a CButtonSpr-nél igazítjuk: a CButtonAnim-ok – főmenü – becsúsznak a
// képre, közben a téglalapjuk kilóg, és a felirat a gomb előtt érkezne)
// A jobb szélre lógó CButtonSpr-ek alsó sávja (Skip, mentésválasztó Continue) 1920 szélesen 3 képponttal a szél
// előtt ér véget (a játék saját elrendezése): a rajzolás idejére egy egységgel jobbra visszük a gomb helyét (a sáv
// kép 4 képponttal jobbra kerül, a felirat a gomb téglalapjához igazodik, az nem mozdul).
#define BTN_POS_X 0x34   // short: a gomb sprite-jának helye (logikai)
static void draw_spr(void *self, int arg) {
    const int16_t *r = (const int16_t *)((char *)self + BTN_RECT);
    int16_t *pos_x = (int16_t *)((char *)self + BTN_POS_X);
    const int16_t old_x = *pos_x;
    if (r[2] > *logical_width)
        *pos_x = (int16_t)(old_x + 1);
    draw_labeled(&draw_h_spr, self, arg, 0x88, true);
    *pos_x = old_x;
}
static void draw_anim(void *self, int arg) { draw_labeled(&draw_h_anim, self, arg, 0x98, false); }
static void draw_text(void *self, int arg) { draw_labeled(&draw_h_text, self, arg, 0x80, false); }

static void draw_tab(void *self, int arg) {
    controls_note_drawn(self);
    ORIG(void (*)(void *, int), draw_h_tab)(self, arg);
    uint16_t glyph = controls_button_glyph(self);
    const int16_t *r = (const int16_t *)((char *)self + BTN_RECT);
    // A szélső fül alá, középre (az utolsó fül és a képernyő széle közé nem fér be). Az L/R jel egymodulos
    // képkocka, eltolás nélkül (glyph_frames_patch).
    if (glyph == GLYPH_L || glyph == GLYPH_R)
        draw_glyph(glyph, (r[0] + r[2]) / 2, r[3] + 1, ANCHOR_HCENTER | ANCHOR_TOP);
}

// A párbeszéd-doboz B-je a doboz jobb szélén, függőlegesen középen (a szöveg sorában): a jobb alsó sarokban a
// Skip-sáv A-ja mellé került volna, és a kettő összefolyt.
static void draw_dialog(void *self, int arg) {
    controls_note_drawn(self);
    ORIG(void (*)(void *, int), draw_h_dialog)(self, arg);
    uint16_t glyph = controls_button_glyph(self);
    if (glyph) {
        const int16_t *r = (const int16_t *)((char *)self + BTN_RECT);
        draw_glyph(glyph, r[2] - DIALOG_GLYPH_MARGIN, (r[1] + r[3]) / 2, ANCHOR_RIGHT | ANCHOR_VCENTER);
    }
}

static void draw_hooks(void) {
    draw_h_spr = hook("_ZN10CButtonSpr4DrawEi", draw_spr);
    draw_h_text = hook("_ZN11CButtonText4DrawEi", draw_text);
    draw_h_anim = hook("_ZN11CButtonAnim4DrawEi", draw_anim);
    draw_h_tab = hook("_ZN4CTab4DrawEi", draw_tab);
    draw_h_dialog = hook("_ZN14CDialogTextBox4DrawEi", draw_dialog);
    draw_h_slider = hook("_ZN7CSlider4DrawEi", draw_slider);
    drawstring_hook = hook("_ZN5CFont10DrawStringEPKtiiihiiPt", CFont_DrawString_hook);
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

// ---------------------------------------------------------------------------
// Betöltési mérés (util.h perf): a GS_Loading élete alatt összegzett idők a naplóba, kiadásban is.
//   GS_Loading::Create():  push {r4-sl, lr}; sub sp, sp, #16
//   GS_Loading::Release(): ldr r3, [pc, #148]; push {r4, r5, r6, lr}

static so_hook loading_create_hook, loading_release_hook, set_target_fps_hook;
static uint64_t loading_t0;

//   Application::SetTargetFPS(int): push {r4, lr}; mov r4, r0
// A játék a pályatöltés alatt 1000-re emeli a célját (lásd gl.c gl_set_fast_frames), utána 30-ra állítja.
static void Application_SetTargetFPS(void *self, int fps) {
    ORIG(void (*)(void *, int), set_target_fps_hook)(self, fps);
    gl_set_fast_frames(fps > 60);
}

#ifdef DEBUG
static struct {
    uint32_t same, diff, skipped;
    uint64_t orig_ticks;
} pvr_check;   // debug: a PVRTC-ellenőrzés a betöltés alatt
#endif

static void perf_begin(void) {
    memset(&perf, 0, sizeof(perf));
#ifdef DEBUG
    memset(&pvr_check, 0, sizeof(pvr_check));
#endif
    loading_t0 = perf_tick();
}

static void GS_Loading_Create(void *self) {
    perf_begin();
    ORIG(void (*)(void *), loading_create_hook)(self);
}

//   GS_LoadMenu::Create(): push {r4-r7, lr}; sub sp, sp, #20
//   GS_LoadMenu::Release(): ldr r3, [pc, #116]; push {r4, r5, r6, lr}
// Az indulás töltése (a GS_Intro után, a címkép előtt): sprite-ok, a menü háttérpályája, a felület, a hangok.
// Az eredeti a menütöltés alatt nem emeli a célját (a GS_Loading igen: SetTargetFPS(1000)), így ~150 kis
// lépésének mindegyike kivárná a 30 fps-es képkockát (Switchen ~5 mp) – ugyanúgy emeljük, utána visszaáll.
#define APP_TARGET_FPS 0x50   // Application: int cél-fps (SetTargetFPS írja, +0x54 az ezredmásodperce)
static so_hook loadmenu_create_hook, loadmenu_release_hook;
static void *(*Application_GetInstance)(void);
static int loadmenu_prev_fps;

static void GS_LoadMenu_Create(void *self) {
    perf_begin();
    ORIG(void (*)(void *), loadmenu_create_hook)(self);
    char *app = Application_GetInstance ? Application_GetInstance() : NULL;
    if (app) {
        loadmenu_prev_fps = *(int *)(app + APP_TARGET_FPS);
        Application_SetTargetFPS(app, 1000);
    }
}

#ifdef DEBUG
// csak debug: a lassú lépések (a lépésszámláló +0x18) a naplóba
static so_hook loadmenu_update_hook;
static void GS_LoadMenu_Update(void *self) {
    const int step = *(int *)((char *)self + 0x18);
    const uint64_t t = perf_tick();
    ORIG(void (*)(void *), loadmenu_update_hook)(self);
    const uint32_t ms = perf_ms(perf_tick() - t);
    if (ms >= 100)
        log_info("menütöltés: %d. lépés %u ms", step, ms);
}
#endif

//   PVRTCDecompress(data, 2bit, w, h, out):  push {r4-fp, lr}; sub sp, sp, #1440
// A PVR-textúrák kibontása (a pixel_format::convert-ből) a betöltés idejének nagy része – Edenben 30 mp a 35-ből.
// Helyette a pvrtc.c bájtra azonos, gyors változata fut; amit az nem kezel, azt az eredeti. Debugban minden
// textúrát az eredetivel is kibontunk és összevetünk (eltérésnél az eredeti marad), induláskor öntesztet futtatunk.
static so_hook pvrtc_hook;

static void PVRTCDecompress_fast(const void *src, int two_bit, int w, int h, uint8_t *out) {
    const pvrtc_fn orig = ORIG(pvrtc_fn, pvrtc_hook);
#ifdef DEBUG
    static bool tested;
    if (!tested) {
        tested = true;
        pvrtc_selftest(orig);
    }
#endif
    uint64_t t = perf_tick();
    const bool fast = pvrtc_decompress(src, two_bit, w, h, out);
    if (!fast)
        orig(src, two_bit, w, h, out);
    perf.pvr_ticks += perf_tick() - t;
    perf.pvrs++;
#ifdef DEBUG
    static int check = -1;   // a nopvrtccheck kapcsoló az SD-n: a valódi (kiadási) betöltési idő méréséhez
    if (check < 0)
        check = !file_exists(DATA_PATH "nopvrtccheck");
    if (!fast || !check)
        return;
    const size_t px = (size_t)w * h * 4;
    uint8_t *ref = malloc(px);
    if (!ref) {
        pvr_check.skipped++;
        return;
    }
    t = perf_tick();
    orig(src, two_bit, w, h, ref);
    pvr_check.orig_ticks += perf_tick() - t;
    if (memcmp(ref, out, px) == 0) {
        pvr_check.same++;
    } else {
        size_t i = 0;
        while (ref[i] == out[i])
            i++;
        i &= ~(size_t)3;
        if (pvr_check.diff++ < 10)
            log_error("pvrtc: ELTÉR – %s %dx%d, első képpont (%u, %u): eredeti %02x%02x%02x%02x, új %02x%02x%02x%02x",
                      two_bit ? "2 bit" : "4 bit", w, h, (unsigned)(i / 4 % w), (unsigned)(i / 4 / w), ref[i],
                      ref[i + 1], ref[i + 2], ref[i + 3], out[i], out[i + 1], out[i + 2], out[i + 3]);
        memcpy(out, ref, px);   // a játék az eredetit kapja
    }
    free(ref);
#endif
}

static void perf_end(const char *what) {
    if (!loading_t0)
        return;
    uint32_t total = perf_ms(perf_tick() - loading_t0);
    uint32_t io = perf_ms(perf.io_ticks), sh = perf_ms(perf.shader_ticks), tex = perf_ms(perf.tex_ticks);
    uint32_t swap = perf_ms(perf.swap_ticks), pvr = perf_ms(perf.pvr_ticks);
    uint32_t known = io + sh + tex + swap + pvr;
    log_info("%s: %u ms, %u képkocka (csere/vsync %u ms); fájl %u ms (%u megnyitás, %u olvasás, %u MB); "
             "PVRTC-kibontás %u ms (%u db); shader %u ms (%u db); textúra-feltöltés %u ms (%u db); egyéb %u ms",
             what, total, perf.frames, swap, io, perf.opens, perf.reads, (unsigned)(perf.io_bytes >> 20), pvr,
             perf.pvrs, sh, perf.shaders, tex, perf.textures, total > known ? total - known : 0);
#ifdef DEBUG
    if (pvr_check.same || pvr_check.diff || pvr_check.skipped)
        log_info("pvrtc-ellenőrzés: %u textúra azonos, %u ELTÉR, %u kihagyva (kevés memória); az eredeti "
                 "ugyanezekre %u ms", pvr_check.same, pvr_check.diff, pvr_check.skipped,
                 perf_ms(pvr_check.orig_ticks));
#endif
    loading_t0 = 0;
}

static void GS_Loading_Release(void *self) {
    ORIG(void (*)(void *), loading_release_hook)(self);
    perf_end("betöltés");
}

// ---------------------------------------------------------------------------
// Gombjel-képkockák az interface sprite-ban (memóriában, egyszer, a menütöltés végén – a LoadInterface után)
// A súgók egyes jelei közös modulon osztoztak, a Switch-jelekhez külön kép kell, a textúraatlaszban viszont nincs
// szabad hely. Ezért néhány képkockát az érintéses súgó (kontrolleres módban – dpad_open – soha meg nem jelenő)
// moduljaira állítunk át; a képeket a glyphs.c rajzolja oda (tools/assets/make-glyphs.py):
//   354 mozgás  → a 249-es modul marad (bal kar);   397 nézelődés → a 332-es modulra (jobb kar)
//   348 „L1”    → csak a 246-os pirula (ZL);         351 „R1”      → a 334-es modul (ZR), a 348 helyén
//   GLYPH_L/R (430/435, a fülsor jele) → a 319/313-as modul (L/R), eltolás nélkül
// A CSprite mezői (CSprite::_PaintFrame / _PaintFModule): +0x40 u16 képkockaszám, +0x44 u8* képkockánkénti
// fmodul-szám, +0x48 u16* az első fmodul, +0x30 u16* az fmodul modulja, +0x34 u8* a jelzői (1: vízszintes
// tükrözés – a jobb oldali „érintőpad” és vállgomb tükrözött kép volt), +0x38/+0x3c s16* az eltolása.
static void *(*SpriteManager_GetSprite)(void *mgr, const char *name);
static bool glyph_frames_done;

static void glyph_frames_patch(void) {
    if (glyph_frames_done || !*sprite_manager)
        return;
#ifdef DEBUG
    if (file_exists(DATA_PATH "noglyphpatch")) {
        glyph_frames_done = true;
        return;
    }
#endif
    uint8_t *spr = SpriteManager_GetSprite(*sprite_manager, "interface");
    if (!spr)
        return;
    glyph_frames_done = true;
    uint8_t *nfm = *(uint8_t **)(spr + 0x44);
    const uint16_t *first = *(uint16_t **)(spr + 0x48);
    uint16_t *mod = *(uint16_t **)(spr + 0x30);
    uint8_t *flags = *(uint8_t **)(spr + 0x34);
    int16_t *ox = *(int16_t **)(spr + 0x38), *oy = *(int16_t **)(spr + 0x3c);
    static const struct { int frame, nfm, module; } want[] = {
        { 348, 2, 246 }, { 351, 2, 246 }, { 354, 1, 249 }, { 397, 1, 249 }, { GLYPH_L, 1, 319 }, { GLYPH_R, 1, 313 },
    };
    if (*(uint16_t *)(spr + 0x40) < 448 || !nfm || !first || !mod || !flags || !ox || !oy) {
        log_error("glyphs: az interface sprite nem a várt (képkockák: %u)", *(uint16_t *)(spr + 0x40));
        return;
    }
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        int f = want[i].frame;
        log_debug("glyphs: képkocka %d: %d fmodul, modul %d (%d,%d) jelző %d", f, nfm[f], mod[first[f]], ox[first[f]],
                  oy[first[f]], flags[first[f]]);
        if (nfm[f] != want[i].nfm || mod[first[f]] != want[i].module) {
            log_error("glyphs: a %d. képkocka nem a várt – a gombjelek maradnak", f);
            return;
        }
    }
    const int zl = first[348], zr = first[351], ls = first[354], rs = first[397];
    nfm[348] = nfm[351] = 1;              // a 6×9-es betű nélkül: a felirat a pirulán van
    mod[zr] = 334;
    ox[zr] = ox[zl];
    oy[zr] = oy[zl];
    flags[zr] = flags[zl];
    mod[rs] = 332;                        // 69×67 a 86×85 helyett: a közepe ugyanoda essen
    ox[rs] = (int16_t)(ox[ls] + (86 - 69) / 2);
    oy[rs] = (int16_t)(oy[ls] + (85 - 67) / 2);
    flags[rs] = flags[ls];
    ox[first[GLYPH_L]] = oy[first[GLYPH_L]] = 0;
    ox[first[GLYPH_R]] = oy[first[GLYPH_R]] = 0;
    flags[first[GLYPH_L]] = flags[first[GLYPH_R]] = 0;
    log_info("glyphs: L/R kar, ZL/ZR és a fülsor L/R jele beállítva");
}

static void GS_LoadMenu_Release(void *self) {
    ORIG(void (*)(void *), loadmenu_release_hook)(self);
    glyph_frames_patch();
    perf_end("menütöltés");
    char *app = Application_GetInstance ? Application_GetInstance() : NULL;
    if (app && loadmenu_prev_fps > 0)
        Application_SetTargetFPS(app, loadmenu_prev_fps);
    loadmenu_prev_fps = 0;
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
    CFont_DrawString = (void *)game_sym("_ZN5CFont10DrawStringEPKtiiihiiPt");
    SpriteManager_GetFont = (void *)game_sym("_ZN14CSpriteManager7GetFontEPKc");
    sprite_manager = (void **)game_sym("_ZN14CSpriteManager9SingletonE");
    SpriteManager_GetSprite = (void *)game_sym("_ZN14CSpriteManager9GetSpriteEPKc");
    font_small = (const char *)game_sym("s_FontSmall");
    CFont_GetStringSize = (void *)game_sym("_ZN5CFont13GetStringSizeEPKtPiS2_S2_");
    logical_width = (int *)game_sym("s_logicalScreenWidth");

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
    loading_create_hook = hook("_ZN10GS_Loading6CreateEv", GS_Loading_Create);
    loading_release_hook = hook("_ZN10GS_Loading7ReleaseEv", GS_Loading_Release);
    loadmenu_create_hook = hook("_ZN11GS_LoadMenu6CreateEv", GS_LoadMenu_Create);
    loadmenu_release_hook = hook("_ZN11GS_LoadMenu7ReleaseEv", GS_LoadMenu_Release);
    Application_GetInstance = (void *(*)(void))game_sym("_ZN11Application11GetInstanceEv");
#ifdef DEBUG
    loadmenu_update_hook = hook("_ZN11GS_LoadMenu6UpdateEv", GS_LoadMenu_Update);
#endif
    set_target_fps_hook = hook("_ZN11Application12SetTargetFPSEi", Application_SetTargetFPS);
    pvrtc_hook = hook("_Z15PVRTCDecompressPKviiiPh", PVRTCDecompress_fast);
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
