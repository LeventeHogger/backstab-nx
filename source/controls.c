/*
 * controls.c – Switch-kontroller és érintőképernyő → a játék érintés- és billentyűeseményei
 *
 * A játék (Androidon) szinte mindent érintéssel kezel. A Vita-port fix képernyőpontokra koppintott;
 * itt minden virtuális érintés a játék saját, futásidejű HUD-objektumaiból számol (hely, méret,
 * engedélyezettség, a joystick MAX_DISTANCE-e), így a testreszabott/balkezes elrendezés és a
 * helyzetfüggő gombok (a CButtonSprMulti elemei) is stimmelnek. A Vita-pontok csak tartaléknak
 * maradtak, ha a HUD nem olvasható.
 *
 * A HUD felépítése (libPirates.so 1.2.8d, statikus elemzés):
 *  - Koordináták: a HUD és az érintés a játék LOGIKAI képernyőjén él (s_logicalScreenWidth/Height,
 *    480×320); a nativeOnTouch képpontját az appOnTouch elosztja az s_screenScaleW/H-val
 *    (1920×1080-nál 4 és 3,375), és egészre csonkolja.
 *  - CLevel::GetLevel() → +0x90 ILevelInterface → +0x0c az aktív CBaseControlScheme (ControlScheme
 *    vagy ControlScheme4, azonos mezőkkel; az ILevelInterface::SetControlSchemeTouchByIndex állítja).
 *    A séma mezői (a GetXxxControl/GetXxxElement getterek szerint):
 *      +0x08 WalkJoystick (mozgás)        +0x0c FullScreenRotateJoystick (kamera, az egész képernyő)
 *      +0x10 „Shoot”  multi: 0 PistolSheath, 1 SwordParry, 2 SwordDeflect, 3 SilentKill,
 *                            4 CannonShoot, 5 Sword, 6 GetOffHorse
 *      +0x14 „Jump”   multi: 0 Sheath, 1 Jump
 *      +0x18 „Second” multi: 0 PistolKill, 1 CannonExit, 2 PistolShoot, 3 PistolAim
 *      +0x1c „Counter” multi: 0 CounterAttack, 1 CounterAttackGrenade, 2 Contextual (interakció),
 *                            3 Vengeance
 *      +0x20 „Horse”  multi: 0 CallHorse       +0x24 CButtonSweep (gránát)
 *    CLevel+0x1b04: CButtonMinimap (koppintásra térkép).
 *  - CButton (TouchScreenBase): +0x08 érintési téglalap (short x0,y0,x1,y1, logikai), +0x12 lenyomva,
 *    +0x15 engedélyezve (= feliratkozva az érintésekre, CButton::SetEnabled), +0x34/+0x36 helyzet.
 *    A CButtonSprMulti a legkisebb prioritású engedélyezett elemét mutatja és annak EvGeneric-
 *    eseményét küldi (GetFirstEnabled) – ezért a gomb közepére koppintunk, nem eseményt küldünk.
 *  - WalkJoystick: lebegő középpont (a lenyomás helye); kitérés = (ujj − lenyomás) / MAX_DISTANCE
 *    tengelyenként ±1-re vágva (EvStickMove 15). MAX_DISTANCE = 40 · s_screenScaleH / 2 logikai
 *    képpont (1080p-n 67) – a régi kód képernyőpixelben húzott, ezért csak sétált a karakter.
 *  - FullScreenRotateJoystick: minden mozgás-eseménynél a LÉPÉSKÖZI elmozdulást küldi
 *    (EvStickMove 10, logikai képpont); a PlayerComponent::ProcessRotateCameraEvent ebből holtjáték
 *    nélkül, a szög ∝ elmozdulás · s_screenScaleW, az utolsó 10 eseményt súlyozva (1, ½, ¼ …) simítva
 *    forgat. Koppintásnál EvFullScreenPress/Release, 200 ms-on belüli két koppintásnál EvDoubleTap
 *    (→ CLevel::ProcessTouchScreen) is megy – a Vita-féle képkockánkénti újralenyomás ezt váltotta ki.
 *    Ezért a kamerát a játék saját eseményével hajtjuk (raiseSync(EvStickMove 10)), ugyanazzal, amit
 *    a FullScreenRotateJoystick::RaiseMoveEvent küld, csak törtpontosan; ha a forgatás tiltva van
 *    (a joystick nincs engedélyezve), nem küldünk.
 *
 * Egyéb:
 *  - Menüben (a játék állapotverme nem a játékmenet GS_Test-állapota) fókusz-navigáció van, a játék
 *    saját (Xperia Play-es) fókuszrendszerén: D-pad/bal kar = lépés a gombok között, A = koppintás a
 *    fókuszban lévő gombra, B = Android BACK (a szünetmenü alfülein a „Back” gomb), L/R = fülváltás
 *    (részletek a „Menük” résznél). Szabad
 *    kurzor (bal kar + A) csak ahol nincs fókuszálható gomb, vagy R3-ra (játék közben is).
 *  - A valódi érintőképernyő (kézi módban) többujjas érintésként megy át, külön indexeken.
 *  - Minden virtuális gomb-érintést ugyanott engedünk fel, ahol lenyomtuk.
 *
 * Copyright (C) 2021 Andy Nguyen, (C) 2021-2023 Rinnegatamante, (C) 2022-2023 Volodymyr Atamanenko
 * Copyright (C) 2026 Levi (Switch port)
 * MIT license, lásd LICENSE.
 */
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include <FalsoJNI/FalsoJNI.h>

#include "game.h"
#include "port.h"
#include "util.h"

// A játék nativeOnTouch-akciói (a saját Java-oldali konvenciója, nem a MotionEvent-kódok)
enum { TOUCH_UP = 0, TOUCH_DOWN = 1, TOUCH_MOVE = 2 };

// Android-billentyűkódok, amelyeket a játék kezel (az AbsorbKey táblája: Xperia Play-kiosztás)
#define AKEYCODE_BACK         4
#define AKEYCODE_BUTTON_START 108

// A gxGameState::isKindOf azonosítója a játékmenet állapotára (GS_Test); a többi állapot menü,
// töltőképernyő, halál-képernyő stb.
#define GS_KIND_GAMEPLAY 2

// Érintési indexek (pointer id). A valódi ujjak 0..9, a virtuálisak ettől távol, hogy ne ütközzenek.
#define REAL_TOUCH_MAX 10
#define PTR_LSTICK     12   // a Vitán is 12/13 (fingerId + 1)
#define PTR_RSTICK     13
#define PTR_CURSOR     14
#define PTR_FOCUS      15   // menüben az A: koppintás a fókuszban lévő gombra
#define PTR_BUTTON     20   // + logikai gomb
#define PTR_DPAD       40   // + irány

// A játék eseményazonosítója: kamera forgatása (EvStickMove, dx/dy logikai képpontban)
#define EV_ROTATE_CAMERA 10

// ---------------------------------------------------------------------------
// A játék HUD-objektumainak mezői (lásd a fájl elején)

#define LEVEL_INTERFACE  0x90     // CLevel → ILevelInterface*
#define LEVEL_MINIMAP    0x1b04   // CLevel → CButtonMinimap*
#define IFACE_SCHEME     0x0c     // ILevelInterface → aktív CBaseControlScheme*

#define BTN_RECT         0x08     // short x0, y0, x1, y1
#define BTN_PRESSED      0x12
#define BTN_ENABLED      0x15
#define BTN_VISIBLE      0x16

#define MULTI_ELEM_BEGIN 0xac     // std::vector<sButtonMultiEntry> (64 bájtos elemek)
#define MULTI_ELEM_END   0xb0
#define MULTI_ELEM_SIZE  64

enum {
    HUD_MOVE,      // WalkJoystick
    HUD_ROTATE,    // FullScreenRotateJoystick
    HUD_SHOOT,     // kard / célzás vége / ágyúlövés / leszállás a lóról
    HUD_JUMP,      // ugrás / kard elrakása
    HUD_SECOND,    // célzás / lövés / pisztolyos kivégzés / ágyú elhagyása
    HUD_COUNTER,   // kontra / interakció / bosszú
    HUD_HORSE,     // ló hívása
    HUD_GRENADE,   // gránát (CButtonSweep)
    HUD_MINIMAP,   // minitérkép (CLevel-ben)
    HUD_COUNT
};

// a séma mezőinek eltolása (HUD_MINIMAP-ig)
static const int scheme_off[HUD_MINIMAP] = { 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24 };

typedef struct {
    void *obj;               // NULL: nincs / ismeretlen osztály → Vita-tartalék
    bool enabled;            // fogad-e most érintést
    bool pressed;
    short x0, y0, x1, y1;    // érintési téglalap, logikai képpont
    int elem;                // multi: az első engedélyezett elem (a gomb mostani szerepe), különben -1
} HudCtl;

static HudCtl hud[HUD_COUNT];
static bool hud_valid;
static void *hud_level, *hud_scheme;

// ---------------------------------------------------------------------------
// A játék függvényei és változói (controls_init oldja fel, a végleges címeken)

static int (*nativeOnTouch)(void *env, void *obj, int action, int x, int y, int index);
static void (*CheckInputKey)(int keycode, int action, int repeats, int scancode);
static int (*AbsorbKey)(int keycode);
static void (*nativeOnKeyUp)(void *env, void *obj, int keycode);
static void *(*Application_GetInstance)(void);
static void *(*gxStateStack_CurrentState)(void *stack);
static int *isPressKey;
static int *joy_max_distance;   // AnalogJoystick::MAX_DISTANCE (logikai képpont)

static void *(*CLevel_GetLevel)(void);
static void *(*CLevel_GetPlayerComponent)(void *level);
static bool (*PC_IsInAimMode)(void *pc);
static bool (*PC_IsOnCannon)(void *pc);
static bool (*PC_IsOnHorse)(void *pc);
static bool (*PC_CanCallHorse)(void *pc);
static void (*PC_CallHorse)(void *pc);
static bool (*PC_CanUseVengeance)(void *pc);
static bool (*PC_CanUseInteractButton)(void *pc);
static void (*PC_IncrementGrenadeSelection)(void *pc, int step);
static void (*PC_EnterAimMode)(void *pc);

// Opcionálisak: ha bármelyik hiányzik, a Vita-féle tartalék működik.
static float *screen_scale_w, *screen_scale_h;   // s_screenScaleW/H
static int *logical_w, *logical_h;               // s_logicalScreenWidth/Height
static int (*Multi_GetFirstEnabled)(void *multi);
static void (*EventManager_raiseSync)(void *mgr, const void *event);
static void **event_manager;                     // EventManager::Singleton
static const void *vt_ev_stick_move;
static const void *vt_scheme, *vt_scheme4;
static const void *vt_want[HUD_COUNT];            // az elvárt osztály vtáblája mezőnként

static void *game_sym(const char *name) {
    void *p = (void *)so_symbol(&so_mod, name);
    if (!p)
        fatal_error("controls: hiányzó szimbólum: %s", name);
    return p;
}

static void *game_sym_opt(const char *name) {
    void *p = (void *)so_symbol(&so_mod, name);
    if (!p)
        log_error("controls: nincs meg %s – Vita-tartalék", name);
    return p;
}

// A vtábla-szimbólum (_ZTV…) helyett az objektumban tárolt mutató: két mutatónyival utána.
static const void *game_vtable(const char *name) {
    char *p = game_sym_opt(name);
    return p ? p + 8 : NULL;
}

// ---------------------------------------------------------------------------
// Koordináták

static float sc_w = 1.0f, sc_h = 1.0f;     // képernyő képpont / logikai képpont
static int lw = 480, lh = 320;             // logikai képernyő

static void update_scale(void) {
    if (screen_scale_w && screen_scale_h && *screen_scale_w >= 1.0f && *screen_scale_h >= 1.0f) {
        sc_w = *screen_scale_w;
        sc_h = *screen_scale_h;
    } else {
        sc_w = sc_h = 1.0f;
    }
    if (logical_w && logical_h && *logical_w > 0 && *logical_h > 0) {
        lw = *logical_w;
        lh = *logical_h;
    } else {
        lw = (int)(screen_width / sc_w);
        lh = (int)(screen_height / sc_h);
    }
}

// Vita (960×544) → képernyő → logikai
static inline float vx(float x) { return x * (float)screen_width / 960.0f; }
static inline float vy(float y) { return y * (float)screen_height / 544.0f; }
static inline float vlx(float x) { return vx(x) / sc_w; }
static inline float vly(float y) { return vy(y) / sc_h; }

// Logikai képpont → az a képernyőképpont, amelyet az appOnTouch (x / scale, csonkolva) pont erre
// a logikai képpontra képez vissza (a logikai képpont közepe).
static inline int l2sx(float lx) { return (int)((floorf(lx) + 0.5f) * sc_w); }
static inline int l2sy(float ly) { return (int)((floorf(ly) + 0.5f) * sc_h); }

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// A bal (mozgás) és a jobb (kamera) virtuális joystick Vita-helye és -sugara (tartalék).
#define LSTICK_X 121.0f
#define LSTICK_Y 425.0f
#define LSTICK_R 77.0f
#define RSTICK_X 662.0f
#define RSTICK_Y 237.0f

// A bal kar teljes kitérésénél a húzás a MAX_DISTANCE ennyiszerese: átlósan is telítődjön (futás).
#define LSTICK_OVERDRIVE 1.2f

// A jobb kar teljes kitérésénél a kamera ennyi logikai képpontnyi húzásnak megfelelő forgatást kap
// másodpercenként (cameraSensitivity 100-nál). A korábbi, kipróbált beállítás (0,2-es
// erősség, képkockánkénti húzás 30 fps-en) teljes kitérésnél 4 (x) / 5 (y) logikai képpontot adott
// képkockánként = 120/150 képpont/s; ennek a közepe. A görbe továbbra is négyzetes.
#define CAMERA_SPEED 135.0f

// Tartalék kamera-ujj (ha a játék eseménykezelője nem érhető el): ennyi logikai képpont után
// felemeljük és középen újra lenyomjuk (lassabban, mint a 200 ms-os dupla koppintás).
#define CAMERA_FINGER_RANGE 120.0f

static void touch(int action, int x, int y, int index) {
    nativeOnTouch(&jni, NULL, action, x, y, index);
}

// Egy virtuális ujj: ahol lenyomtuk, ott is engedjük fel (képernyő-koordináta).
typedef struct {
    bool held;
    int x, y;
} VTouch;

static void vtouch_press(VTouch *t, int index, int x, int y) {
    if (t->held)
        return;
    touch(TOUCH_DOWN, x, y, index);
    t->held = true;
    t->x = x;
    t->y = y;
}

static void vtouch_release(VTouch *t, int index) {
    if (!t->held)
        return;
    touch(TOUCH_UP, t->x, t->y, index);
    t->held = false;
}

// ---------------------------------------------------------------------------
// Logikai gombok: a Vita-kiosztás, a Switch-gombok FIZIKAI helye szerint
// (Vita △ fent = Switch X, ○ jobbra = A, × lent = B, □ balra = Y).

enum {
    VB_AIM,       // ZL / L   – Vita L: célzás
    VB_FIRE,      // ZR / R   – Vita R: lövés / különleges támadás
    VB_INTERACT,  // X        – Vita △: interakció, célzáskor fegyverváltás, nyomva tartva ló hívása
    VB_ATTACK,    // Y        – Vita □: kardtámadás
    VB_JUMP,      // B        – Vita ×: ugrás
    VB_MAP,       // −        – Vita Select: térkép (a minimapra koppint)
    VB_COUNTER,   // A        – Vita ○: kontra
    VB_COMBO,     // virtuális: Fire + Attack együtt – „Vengeance” (a Vitán az L3-sor)
    VB_COUNT
};

static const u64 vb_hid[VB_COUNT] = {
    [VB_AIM]      = HidNpadButton_ZL | HidNpadButton_L,
    [VB_FIRE]     = HidNpadButton_ZR | HidNpadButton_R,
    [VB_INTERACT] = HidNpadButton_X,
    [VB_ATTACK]   = HidNpadButton_Y,
    [VB_JUMP]     = HidNpadButton_B,
    [VB_MAP]      = HidNpadButton_Minus,
    [VB_COUNTER]  = HidNpadButton_A,
    [VB_COMBO]    = 0,
};

#define VB_BIT(i) (1u << (i))

// A D-pad a bal joystickot húzza a négy irányba (a Vitán is így).
static const u64 dpad_hid[4] = { HidNpadButton_Up, HidNpadButton_Down, HidNpadButton_Left, HidNpadButton_Right };
static const float dpad_dir[4][2] = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } };

// ---------------------------------------------------------------------------
// Állapot

static PadState pad;
static u32 vb_prev;                    // az előző képkocka logikai gombjai
static u64 hid_prev;                   // az előző képkocka nyers gombjai

static VTouch vbtn[VB_COUNT];
static VTouch vdpad[4];
static bool combo_active;
static uint64_t interact_down_ms;      // a ló hívásához: mióta van lenyomva az X (0 = nincs)

static bool l_active;                  // bal joystick lenyomva
static int l_last_x, l_last_y;         // utolsó helye (képernyő)
static uint64_t l_down_ms;
static bool l_retried;                 // a joystick nem vette át a lenyomást → egyszer újrapróbáltuk

static bool cam_held;                  // tartalék kamera-ujj
static float cam_fx, cam_fy;           // helye, logikai
static float cam_ox, cam_oy;
static int cam_sx, cam_sy;

static bool menu_mode;                 // menü (fókusz-navigáció vagy szabad kurzor)
static bool cursor_forced;             // R3-mal bekapcsolt szabad kurzor (állapotváltáskor kikapcsol)
static bool menu_cursor;               // a szabad kurzor van használatban (nincs fókuszálható gomb / R3)
static void *cur_state;                // a játék legutóbb látott gxGameState-je
static float cur_x, cur_y;
static VTouch cur_touch;
static uint64_t cur_active_ms;         // a kurzor utolsó mozgatása/használata
static uint64_t last_poll_ms;

// Menü-navigáció (lásd „Menük” lent)
#define MENU_MAX 64
typedef struct {
    void *obj;
    short x0, y0, x1, y1;              // logikai képpont
    bool tap;                          // A-ra koppintunk-e rá (a csúszkára nem: középre ugrana)
} MenuBtn;
static MenuBtn mbtn[MENU_MAX];         // a mostani állapot fókuszálható gombjai
static int mbtn_count;
static int focus_idx = -1;             // a fókuszban lévő gomb indexe (mbtn), -1: nincs
static void *focus_obj;                // az előző képkocka fókuszgombja (ha több gombon is fókusz van)
static VTouch focus_touch;             // az A-val lenyomott gomb
static int nav_dir = -1;               // a nyomva tartott irány (0 fel, 1 le, 2 balra, 3 jobbra)
static uint64_t nav_next_ms;           // a következő ismétlés ideje
static void *rule_pending;             // a játék szabálya szerint lépni kellett volna erről a gombról
static int rule_pending_dir;

typedef struct {
    bool used;
    u32 finger;
    int x, y;
} TouchSlot;
static TouchSlot tslot[REAL_TOUCH_MAX];

// ---------------------------------------------------------------------------
// Diagnosztika (csak debug-build): a felismert HUD, a változásai és a koppintások

#ifdef DEBUG
static const char *const hud_name[HUD_COUNT] = {
    "move", "rotate", "shoot", "jump", "second", "counter", "horse", "grenade", "minimap",
};
static const char *const vb_name[VB_COUNT] = {
    "aim", "fire", "interact", "attack", "jump", "map", "counter", "combo",
};
#define DEBUG_LINE_LIMIT 400   // a változás- és koppintásnapló felső korlátja
static int debug_lines;
static HudCtl hud_logged[HUD_COUNT];

static void hud_log_ctl(int i, const char *prefix) {
    const HudCtl *c = &hud[i];
    if (!c->obj) {
        log_info("controls: %s%-7s – nincs (Vita-tartalék)", prefix, hud_name[i]);
        return;
    }
    int cx = (c->x0 + c->x1) / 2, cy = (c->y0 + c->y1) / 2;
    log_info("controls: %s%-7s %p en%d rect (%d,%d)-(%d,%d) közép (%d,%d) = képernyő (%d,%d) elem %d", prefix,
             hud_name[i], c->obj, c->enabled, c->x0, c->y0, c->x1, c->y1, cx, cy, l2sx(cx), l2sy(cy), c->elem);
}

static void hud_dump(void) {
    float *walk_scale = (float *)so_symbol(&so_mod, "_ZN13CGameSettings19s_walkJoystickScaleE");
    float *walk_dist = (float *)so_symbol(&so_mod, "_ZN13CGameSettings27s_walkJoystickDistanceScaleE");
    float *walk_dtap = (float *)so_symbol(&so_mod, "_ZN12WalkJoystick15s_DoubleTapTimeE");
    int *rot_maxd = (int *)so_symbol(&so_mod, "_ZN24FullScreenRotateJoystick12MAX_DISTANCEE");
    log_info("controls: HUD: %s @%p (CLevel %p); logikai %dx%d, skála %.4f x %.4f; MAX_DISTANCE %d "
             "(bal kar teljes húzás %.1f log. px), forgató MAX_DISTANCE %d; walkJoystickScale %.2f, "
             "DistanceScale %.2f, dupla koppintás %.0f ms",
             *(void **)hud_scheme == vt_scheme ? "ControlScheme" : "ControlScheme4", hud_scheme, hud_level,
             lw, lh, sc_w, sc_h, joy_max_distance ? *joy_max_distance : -1,
             joy_max_distance ? *joy_max_distance * LSTICK_OVERDRIVE : -1.0f, rot_maxd ? *rot_maxd : -1,
             walk_scale ? *walk_scale : -1.0f, walk_dist ? *walk_dist : -1.0f, walk_dtap ? *walk_dtap : -1.0f);
    for (int i = 0; i < HUD_COUNT; i++) {
        hud_log_ctl(i, "");
        hud_logged[i] = hud[i];
        const HudCtl *c = &hud[i];
        if (!c->obj || vt_want[i] != vt_want[HUD_SHOOT])
            continue;
        const uint8_t *b = c->obj;
        const uint8_t *e = *(uint8_t *const *)(b + MULTI_ELEM_BEGIN);
        const uint8_t *end = *(uint8_t *const *)(b + MULTI_ELEM_END);
        for (int k = 0; e && e + MULTI_ELEM_SIZE <= end && k < 16; k++, e += MULTI_ELEM_SIZE) {
            const int *w = (const int *)e;
            log_info("controls:     [%d] keret %d prioritás %d események %d/%d/%d/%d hosszú %d %s", k, w[0], w[3],
                     w[4], w[5], w[6], w[7], w[8], e[0x3c] ? "engedélyezve" : "");
        }
    }
}

// Csak a változásokat: melyik gomb mikor kapcsol be/ki, melyik elemre vált, hova kerül.
static void hud_log_changes(void) {
    for (int i = 0; i < HUD_COUNT && debug_lines < DEBUG_LINE_LIMIT; i++) {
        const HudCtl *a = &hud[i], *b = &hud_logged[i];
        if (a->obj == b->obj && a->enabled == b->enabled && a->elem == b->elem && a->x0 == b->x0 &&
            a->y0 == b->y0 && a->x1 == b->x1 && a->y1 == b->y1)
            continue;
        hud_log_ctl(i, "változás: ");
        hud_logged[i] = *a;
        debug_lines++;
    }
}

static void log_tap(int vb, int target, int sx, int sy, bool ok) {
    if (debug_lines >= DEBUG_LINE_LIMIT)
        return;
    debug_lines++;
    if (target < 0)
        log_info("controls: %s → Vita-pont (%d,%d)", vb_name[vb], sx, sy);
    else if (ok)
        log_info("controls: %s → %s elem %d, képernyő (%d,%d)", vb_name[vb], hud_name[target], hud[target].elem,
                 sx, sy);
    else
        log_info("controls: %s → %s most nem fogad érintést (kihagyva)", vb_name[vb], hud_name[target]);
}
#else
#define hud_dump() do {} while (0)
#define hud_log_changes() do {} while (0)
#define log_tap(vb, target, sx, sy, ok) do {} while (0)
#endif

// ---------------------------------------------------------------------------
// A HUD beolvasása (képkockánként, csak játékmenetben)

static void hud_read_ctl(HudCtl *c, void *obj, const void *want_vt) {
    c->obj = NULL;
    c->enabled = false;
    c->pressed = false;
    c->elem = -1;
    if (!obj || !want_vt || *(void **)obj != want_vt)
        return;
    const uint8_t *b = obj;
    const int16_t *r = (const int16_t *)(b + BTN_RECT);
    if (r[2] <= r[0] || r[3] <= r[1])
        return;   // még nincs érintési területe
    c->obj = obj;
    c->x0 = r[0];
    c->y0 = r[1];
    c->x1 = r[2];
    c->y1 = r[3];
    c->enabled = b[BTN_ENABLED] != 0;
    c->pressed = b[BTN_PRESSED] != 0;
    if (want_vt == vt_want[HUD_SHOOT] && Multi_GetFirstEnabled)
        c->elem = Multi_GetFirstEnabled(obj);
}

// Igazat ad, ha a séma most cserélődött (új pálya, más irányítási séma).
static bool hud_update(void) {
    hud_valid = false;
    void *level = CLevel_GetLevel();
    void *iface = level ? *(void **)((char *)level + LEVEL_INTERFACE) : NULL;
    void *scheme = iface ? *(void **)((char *)iface + IFACE_SCHEME) : NULL;
    if (!scheme || !vt_scheme || (*(void **)scheme != vt_scheme && *(void **)scheme != vt_scheme4))
        return false;

    for (int i = 0; i < HUD_MINIMAP; i++)
        hud_read_ctl(&hud[i], *(void **)((char *)scheme + scheme_off[i]), vt_want[i]);
    hud_read_ctl(&hud[HUD_MINIMAP], *(void **)((char *)level + LEVEL_MINIMAP), vt_want[HUD_MINIMAP]);
    hud_valid = true;

    if (scheme == hud_scheme && level == hud_level) {
        hud_log_changes();
        return false;
    }
    hud_scheme = scheme;
    hud_level = level;
    hud_dump();
    return true;
}

// A gomb érintési pontja (képernyő), ha most fogad érintést.
static bool hud_point(int t, int *sx, int *sy) {
    const HudCtl *c = &hud[t];
    if (!c->enabled)
        return false;
    *sx = l2sx((c->x0 + c->x1) / 2);
    *sy = l2sy((c->y0 + c->y1) / 2);
    return true;
}

// ---------------------------------------------------------------------------
// Segédek

// Sugárirányú, skálázott holtjáték (http://www.third-helix.com/2013/04/12/doing-thumbstick-dead-zones-right.html)
static void coord_normalize(float *x, float *y, float dead_zone, float maximum) {
    float mag = sqrtf(*x * *x + *y * *y);
    if (mag >= dead_zone && mag > 0.0f) {
        float scale = maximum / mag * (mag - dead_zone) / (maximum - dead_zone);
        *x *= scale;
        *y *= scale;
    } else {
        *x = 0.0f;
        *y = 0.0f;
    }
}

// Xperia Play-gombesemény a játéknak (CheckInputKey). Az isPressKey-t NEM írjuk: azt Androidon csak
// az appOnKeyUp állítja (Vissza/Menü felengedésekor), és a játék fogyasztja el – egy itt beírt D-pad-
// vagy Vissza-kód „beragadt” lenyomásként maradt benne (menübeli hiba).
static void send_key(int code, bool down) {
    if (!AbsorbKey(code))   // csak a játék által ismert billentyűk
        return;
    CheckInputKey(code, down ? 1 : 0, 0, code);
}

// Android Vissza gomb: a Java-oldal nativeOnKeyUp(keycode) → appOnKeyUp → isPressKey = 4.
static void send_back(void) {
    nativeOnKeyUp(&jni, NULL, AKEYCODE_BACK);
}

// Szünetmenü (GS_InGameMenu): az Android BACK (Handle_dv_key(4)) az egész menüt bezárja, az alfüleken
// (Options → Audio/Controls/Advanced …) viszont a képernyőn egy „Back” gomb lép egy szinttel vissza.
// A GS_InGameMenu::Update ezt az ágat az is_open_cf_main_menu_screen globálisra is lefuttatja (a gomb
// koppintásával azonos: beállítások mentése, a szülő fül), ha a +0x158-as Back gomb engedélyezett
// (IsEnabled = +0x15; csak az alfüleken az). Ha a +0x4c-s gomb az engedélyezett, ugyanerre a
// „kilépés a főmenübe?” megerősítést nyitná – ezért csak élő Back gombnál állítjuk.
#define INGAME_BACK_BTN 0x158
static const void *vt_ingame_menu;
static int *open_cf_main_menu;

static bool ingame_menu_back(void *state) {
    if (!state || !vt_ingame_menu || !open_cf_main_menu || *(const void **)state != vt_ingame_menu)
        return false;
    const uint8_t *back = *(uint8_t **)((char *)state + INGAME_BACK_BTN);
    if (!back || !back[BTN_ENABLED])
        return false;
    *open_cf_main_menu = 1;
    return true;
}

static void *player_component(void) {
    void *level = CLevel_GetLevel();
    return level ? CLevel_GetPlayerComponent(level) : NULL;
}

// Igaz, ha nem a játékmenet fut (menü, töltés, halál-képernyő, történet-képernyő…).
static bool game_in_menu(void *app) {
    void *state = gxStateStack_CurrentState((char *)app + 4);   // Application + 4 = gxStateStack
    if (!state)
        return false;
    bool (*is_kind_of)(void *self, int kind) = (*(void ***)state)[2];   // vtable[2] = isKindOf
    return !is_kind_of(state, GS_KIND_GAMEPLAY);
}

// A játék saját kamera-eseménye, ahogy a FullScreenRotateJoystick::RaiseMoveEvent küldi:
// EvStickMove { vtábla, azonosító, float dx, float dy, int x, int y } (GetSize = 24).
typedef struct {
    const void *vtable;
    int id;
    float x, y;
    int px, py;
} EvStickMove;

static bool raise_stick_move(int id, float x, float y) {
    if (!EventManager_raiseSync || !event_manager || !*event_manager || !vt_ev_stick_move)
        return false;
    EvStickMove ev = { vt_ev_stick_move, id, x, y, -1, -1 };
    EventManager_raiseSync(*event_manager, &ev);
    return true;
}

static void camera_finger_release(void) {
    if (cam_held) {
        touch(TOUCH_UP, cam_sx, cam_sy, PTR_RSTICK);
        cam_held = false;
    }
}

// Tartalék: egy nyomva tartott ujj húzása a képernyő jobb felén (a FullScreenRotateJoystick a
// lépésközi elmozdulásból forgat). Csak akkor emeljük fel, ha messzire ért.
static void camera_finger_drag(float dx, float dy) {
    if (cam_held && (fabsf(cam_fx + dx - cam_ox) > CAMERA_FINGER_RANGE ||
                     fabsf(cam_fy + dy - cam_oy) > CAMERA_FINGER_RANGE))
        camera_finger_release();
    if (!cam_held) {
        cam_ox = cam_fx = vlx(RSTICK_X);
        cam_oy = cam_fy = vly(RSTICK_Y);
        cam_sx = l2sx(cam_fx);
        cam_sy = l2sy(cam_fy);
        touch(TOUCH_DOWN, cam_sx, cam_sy, PTR_RSTICK);
        cam_held = true;
    }
    cam_fx += dx;
    cam_fy += dy;
    int sx = l2sx(cam_fx), sy = l2sy(cam_fy);
    if (sx != cam_sx || sy != cam_sy) {
        touch(TOUCH_MOVE, sx, sy, PTR_RSTICK);
        cam_sx = sx;
        cam_sy = sy;
    }
}

// Minden virtuális érintés és billentyű elengedése (módváltáskor, HUD-cserekor).
static void release_all_virtual(void) {
    for (int i = 0; i < VB_COUNT; i++)
        vtouch_release(&vbtn[i], PTR_BUTTON + i);
    for (int i = 0; i < 4; i++)
        vtouch_release(&vdpad[i], PTR_DPAD + i);
    combo_active = false;
    interact_down_ms = 0;
    if (l_active) {
        touch(TOUCH_UP, l_last_x, l_last_y, PTR_LSTICK);
        l_active = false;
    }
    camera_finger_release();
    vtouch_release(&cur_touch, PTR_CURSOR);
    vtouch_release(&focus_touch, PTR_FOCUS);
}

// ---------------------------------------------------------------------------
// Valódi érintőképernyő (kézi mód; dokkolva nincs érintés). A HID 1280×720-ban ad koordinátát.

static void poll_touchscreen(void) {
    HidTouchScreenState st;
    if (hidGetTouchScreenStates(&st, 1) == 0)
        return;

    // felengedett ujjak
    for (int s = 0; s < REAL_TOUCH_MAX; s++) {
        if (!tslot[s].used)
            continue;
        bool present = false;
        for (int i = 0; i < st.count; i++)
            if (st.touches[i].finger_id == tslot[s].finger)
                present = true;
        if (!present) {
            touch(TOUCH_UP, tslot[s].x, tslot[s].y, s);
            tslot[s].used = false;
        }
    }

    // új és mozgó ujjak
    for (int i = 0; i < st.count && i < 16; i++) {
        const HidTouchState *t = &st.touches[i];
        int x = (int)(t->x * (u32)screen_width / 1280u);
        int y = (int)(t->y * (u32)screen_height / 720u);
        int s, free_slot = -1;
        for (s = 0; s < REAL_TOUCH_MAX; s++) {
            if (tslot[s].used && tslot[s].finger == t->finger_id)
                break;
            if (!tslot[s].used && free_slot < 0)
                free_slot = s;
        }
        if (s < REAL_TOUCH_MAX) {
            if (tslot[s].x != x || tslot[s].y != y)
                touch(TOUCH_MOVE, x, y, s);
        } else if (free_slot >= 0) {
            s = free_slot;
            tslot[s].used = true;
            tslot[s].finger = t->finger_id;
            touch(TOUCH_DOWN, x, y, s);
        } else {
            continue;   // több mint 10 ujj: a többit eldobjuk
        }
        tslot[s].x = x;
        tslot[s].y = y;
    }
}

// ---------------------------------------------------------------------------
// Menük
//
// A játékban megvan az Xperia Play-es menünavigáció, csak Androidon (érintéssel) nem látszott:
//  - CButton+0x74 (bájt) = fókuszban. A CButtonSpr/CButtonText/CButtonAnim::Draw kiemeli (CButtonSpr-nél
//    a felirat színe), de csak ha a dpad_open nem 0 – a patch.c a játékbeli menü (GS_InGameMenu) és a
//    térkép rajzolása alatt 0-ra veszi, ezért saját keretet is rajzolunk (gl.c).
//  - CButton::AddRule(GAME_ACTIONS, CButton*) navigációs szabályokat épít (+0x5c akciók, +0x68
//    célgombok; a GS_BaseMenu::SetupMenuButtons, GS_MainMenu/GS_InGameMenu/GS_Confirm/… Create-je).
//    A CButton::Update, ha fókuszban van és a menu_action globális egy szabály akciója, a fókuszt a
//    célgombra teszi (és hangot ad); 49 = kiválasztás. A menu_action-t a runAction(45..53) írja
//    (a CheckInputKey → CheckActions → control0-kombó útján), az appUpdate a frissítés végén 58-ra
//    (semmi) állítja. Mi közvetlenül írjuk – a runAction ezekre az akciókra pontosan csak ezt teszi.
//    45 fel, 46 le, 47 balra, 48 jobbra, 49 kiválasztás, 50 vissza, 52/53 előző/következő fül
//    (CTabControl::Update), 58 semmi.
//  - Az aktív állapot (gxGameState+4) körkörös listában tartja a feliratkozott (engedélyezett)
//    érintésfogadókat: { next, prev, TouchScreenBase* } – innen gyűjtjük a képernyő gombjait.
// Ahol a fókuszban lévő gombnak van szabálya az irányra, a játék lép (ha a következő képkockában
// mégsem lépett, mi lépünk); ahol nincs, a képernyőn az adott irányba eső legközelebbi gombra
// lépünk. Az A a fókuszban lévő gomb közepére koppint (felengedés ugyanott), a B az Android-Vissza.
// Szabad kurzor csak ott, ahol nincs fókuszálható gomb, vagy R3-ra.

#define STATE_TOUCH_LIST 0x04     // gxGameState: a feliratkozott érintésfogadók listája (őrszem)
#define BTN_FOCUS        0x74
#define BTN_RULE_ACT     0x5c     // std::vector<GAME_ACTIONS>: begin, end (+0x60)
#define BTN_RULE_DST     0x68     // std::vector<CButton*>: begin
// CTab (fül-lap): a gyerekgombjai (+0xb4 std::vector<CButton*> begin, +0xb8 end) közül a +0xcc indexűt
// a CTab::Update minden képkockában fókuszba teszi; az indexet a CTab::UpdateMenuAction lépteti a fel/le
// menü-akcióra (körbefordulva). Ilyen gombnál a fel/le irányt a játéknak adjuk, mert a mi
// áthelyezésünket a következő képkockában visszaírná (pl. a szünetmenü Advanced fülének kapcsolói).
#define TAB_CHILDREN     0xb4
#define TAB_CLASS        4        // a touch_class[] CTab-sora

enum { MA_UP = 45, MA_DOWN = 46, MA_LEFT = 47, MA_RIGHT = 48, MA_TAB_PREV = 52, MA_TAB_NEXT = 53 };
static const int dir_action[4] = { MA_UP, MA_DOWN, MA_LEFT, MA_RIGHT };

static int *menu_action;          // a játék menu_action globálisa

// Az érintésfogadó osztályok (a vtáblák alapján mind a CButtonból származik; a libPirates.so összes
// ilyen osztálya). Fókuszálni csak a gombokat és a csúszkát fókuszáljuk; a listák, joystickok nem.
// A csúszkára nem koppintunk (középre ugrana): azt a játék a fókusz mellett a balra/jobbra
// menü-akcióval állítja.
// A drawn jelzésű osztályok Draw(int)-jét a patch.c figyeli: ezekből csak a ténylegesen kirajzoltakra
// lépünk (lásd controls_note_drawn). A sima CButton Draw-ja üres (láthatatlan érintési zóna), arra nem
// szűrhetünk.
static const struct {
    const char *sym, *name;
    bool focusable, tap, drawn;
} touch_class[] = {
    { "_ZTV7CButton", "CButton", true, true, false },
    { "_ZTV10CButtonSpr", "CButtonSpr", true, true, true },
    { "_ZTV11CButtonText", "CButtonText", true, true, true },
    { "_ZTV11CButtonAnim", "CButtonAnim", true, true, true },
    { "_ZTV4CTab", "CTab", true, true, true },
    { "_ZTV14CDialogTextBox", "CDialogTextBox", true, true, true },
    { "_ZTV7CSlider", "CSlider", true, false, true },
    { "_ZTV10CScrollBar", "CScrollBar", false },
    { "_ZTV11CTabControl", "CTabControl", false },
    { "_ZTV11CMapDisplay", "CMapDisplay", false },
    { "_ZTV14CQuestsDisplay", "CQuestsDisplay", false },
    { "_ZTV17CInventoryDisplay", "CInventoryDisplay", false },
    { "_ZTV16CUpgradesDisplay", "CUpgradesDisplay", false },
    { "_ZTV18CStatisticsDisplay", "CStatisticsDisplay", false },
    { "_ZTV12CButtonShoot", "CButtonShoot", false },
    { "_ZTV12CButtonSweep", "CButtonSweep", false },
    { "_ZTV15CButtonSprMulti", "CButtonSprMulti", false },
    { "_ZTV14CButtonMinimap", "CButtonMinimap", false },
    { "_ZTV14AnalogJoystick", "AnalogJoystick", false },
    { "_ZTV12WalkJoystick", "WalkJoystick", false },
    { "_ZTV24FullScreenRotateJoystick", "FullScreenRotateJoystick", false },
};
#define TOUCH_CLASS_COUNT ((int)(sizeof(touch_class) / sizeof(touch_class[0])))
static const void *touch_class_vt[TOUCH_CLASS_COUNT];

static int touch_class_of(const void *obj) {
    const void *vt = *(void *const *)obj;
    for (int i = 0; i < TOUCH_CLASS_COUNT; i++)
        if (touch_class_vt[i] && touch_class_vt[i] == vt)
            return i;
    return -1;
}

// Kirajzolt gombok. A játékmenü más füleinek gombjai engedélyezettek és „láthatók” maradnak, csak
// nem rajzolódnak ki (a szünetmenü Options fülén pl. egy üres helyen álló, a játék által fókuszáltnak
// jelölt gomb) – ezekre nem szabad lépni. A patch.c a gombosztályok Draw(int)-jéből hívja a
// controls_note_drawn-t; a menü csak a legutóbbi néhány képkockában kirajzolt gombokat veszi fel.
// Amíg az új állapot egyetlen figyelt gombot sem rajzolt (az első képkocka előtt), nem szűrünk.
#define DRAWN_SLOTS 512
#define DRAWN_FRAMES 8            // ennyi képkockán belül kirajzolt gomb számít láthatónak
static struct { const void *obj; uint32_t frame; } drawn[DRAWN_SLOTS];
static uint32_t draw_frame = DRAWN_FRAMES + 1;   // a controls_poll lépteti
static uint32_t state_draws;                     // figyelt Draw-hívások a mostani állapot óta

void controls_note_drawn(const void *obj) {
    state_draws++;
    unsigned h = ((uintptr_t)obj >> 4) % DRAWN_SLOTS;
    int free_slot = -1;
    for (int i = 0; i < 16; i++) {
        unsigned k = (h + i) % DRAWN_SLOTS;
        if (drawn[k].obj == obj) {
            drawn[k].frame = draw_frame;
            return;
        }
        if (free_slot < 0 && draw_frame - drawn[k].frame > DRAWN_FRAMES)
            free_slot = (int)k;
    }
    if (free_slot < 0)
        free_slot = (int)h;   // tele a környék: a legrégebbi helyett az elsőt írjuk felül
    drawn[free_slot].obj = obj;
    drawn[free_slot].frame = draw_frame;
}

static bool drawn_recently(const void *obj) {
    if (!state_draws)
        return true;
    unsigned h = ((uintptr_t)obj >> 4) % DRAWN_SLOTS;
    for (int i = 0; i < 16; i++) {
        unsigned k = (h + i) % DRAWN_SLOTS;
        if (drawn[k].obj == obj)
            return draw_frame - drawn[k].frame <= DRAWN_FRAMES;
    }
    return false;
}

// A gomb szabálya az akcióra: a célgomb, vagy NULL.
static void *button_rule(const void *obj, int action) {
    const uint8_t *b = obj;
    const int *act = *(int *const *)(b + BTN_RULE_ACT);
    const int *act_end = *(int *const *)(b + BTN_RULE_ACT + 4);
    void *const *dst = *(void *const *const *)(b + BTN_RULE_DST);
    if (!act || !dst || act_end < act || act_end - act > 32)
        return NULL;
    for (int i = 0; act + i < act_end; i++)
        if (act[i] == action)
            return dst[i];
    return NULL;
}

// Az a CTab az állapot érintésfogadói közül, amelynek obj a gyereke (vagy NULL).
static void *tab_of(void *state, const void *obj) {
    char *sentinel = (char *)state + STATE_TOUCH_LIST;
    void **node = *(void ***)sentinel;
    for (int n = 0; node && (char *)node != sentinel && n < 512; node = (void **)node[0], n++) {
        uint8_t *t = node[2];
        if (!t || t == obj || touch_class_of(t) != TAB_CLASS)
            continue;
        void *const *c = *(void *const *const *)(t + TAB_CHILDREN);
        void *const *c_end = *(void *const *const *)(t + TAB_CHILDREN + 4);
        if (!c || c_end < c || c_end - c > 64)
            continue;
        for (; c < c_end; c++)
            if (*c == obj)
                return t;
    }
    return NULL;
}

#ifdef DEBUG
static int menu_log_lines;
static uint32_t menu_log_sig;

// Az állapot összes érintésfogadója (a fókuszálhatatlanok is), ha a halmaz megváltozott.
static void menu_dump(void *state, uint32_t sig, int listeners) {
    if (sig == menu_log_sig || menu_log_lines >= 800)
        return;
    menu_log_sig = sig;
    log_info("controls: menü: állapot %p (vtábla 0x%x), %d érintésfogadó, %d fókuszálható", state,
             (unsigned)((uintptr_t)*(void **)state - so_mod.base - 8), listeners, mbtn_count);
    char *sentinel = (char *)state + STATE_TOUCH_LIST;
    void **node = *(void ***)sentinel;
    for (int n = 0; node && (char *)node != sentinel && n < 512; node = (void **)node[0], n++) {
        const uint8_t *b = node[2];
        if (!b)
            continue;
        int cls = touch_class_of(b);
        const int16_t *r = (const int16_t *)(b + BTN_RECT);
        char rules[96] = "";
        int len = 0;
        for (int a = 45; a <= 53 && cls >= 0; a++) {
            void *t = button_rule(b, a);
            if (t && len < (int)sizeof(rules) - 16)
                len += snprintf(rules + len, sizeof(rules) - len, " %d→%p", a, t);
        }
        log_info("controls:   %p %s rect (%d,%d)-(%d,%d) en%d vis%d rajz%d fókusz%d%s%s", (void *)b,
                 cls >= 0 ? touch_class[cls].name : "?", r[0], r[1], r[2], r[3], b[BTN_ENABLED], b[BTN_VISIBLE],
                 cls >= 0 && touch_class[cls].drawn ? drawn_recently(b) : -1,
                 cls >= 0 ? b[BTN_FOCUS] : 0, rules[0] ? " szabályok:" : "", rules);
        if (++menu_log_lines >= 800)
            break;
    }
}

static void menu_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void menu_log(const char *fmt, ...) {
    if (menu_log_lines >= 800)
        return;
    menu_log_lines++;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_info("controls: menü: %s", buf);
}
#else
#define menu_dump(state, sig, listeners) ((void)(state), (void)(sig), (void)(listeners))
#define menu_log(...) do {} while (0)
#endif

// A mostani állapot fókuszálható gombjai (engedélyezett, látható, a képernyőn lévő).
static void menu_scan(void *state) {
    mbtn_count = 0;
    focus_idx = -1;
    if (!state || !menu_action)
        return;
    uint32_t sig = (uint32_t)(uintptr_t)state;
    int listeners = 0;
    char *sentinel = (char *)state + STATE_TOUCH_LIST;
    void **node = *(void ***)sentinel;
    for (int n = 0; node && (char *)node != sentinel && n < 512; node = (void **)node[0], n++) {
        void *obj = node[2];
        if (!obj)
            continue;
        listeners++;
        sig = sig * 31 + (uint32_t)(uintptr_t)obj;
        int cls = touch_class_of(obj);
        if (cls < 0 || !touch_class[cls].focusable)
            continue;
        const uint8_t *b = obj;
        const int16_t *r = (const int16_t *)(b + BTN_RECT);
        if (!b[BTN_ENABLED] || !b[BTN_VISIBLE] || r[2] <= r[0] || r[3] <= r[1])
            continue;
        if (r[2] <= 0 || r[3] <= 0 || r[0] >= lw || r[1] >= lh)
            continue;   // a képernyőn kívül (görgetett lista)
        if (touch_class[cls].drawn && !drawn_recently(obj))
            continue;   // nem rajzolódik ki (pl. egy másik fül gombja)
        sig = sig * 31 + 1;   // a debug-naplóhoz: a kirajzolt gombok halmaza is a képernyő része
        if (mbtn_count >= MENU_MAX)
            break;
        MenuBtn *m = &mbtn[mbtn_count];
        m->obj = obj;
        m->x0 = r[0];
        m->y0 = r[1];
        m->x1 = r[2];
        m->y1 = r[3];
        m->tap = touch_class[cls].tap;
        // Több fókuszált gomb is lehet (pl. egy fül-lap a saját gyerekét fókuszálja, mi egy másikat):
        // az előző képkockában fókuszban lévő marad, különben az első.
        if (b[BTN_FOCUS] && (focus_idx < 0 || obj == focus_obj))
            focus_idx = mbtn_count;
        mbtn_count++;
    }
    focus_obj = focus_idx >= 0 ? mbtn[focus_idx].obj : NULL;
    menu_dump(state, sig, listeners);
}

// A fókusz áthelyezése: az állapot minden ismert gombjáról levesszük (a nem fókuszálhatóról is,
// hogy ne maradjon két fókusz), és a célgombra tesszük.
static void set_focus(int idx) {
    char *sentinel = (char *)cur_state + STATE_TOUCH_LIST;
    void **node = *(void ***)sentinel;
    for (int n = 0; node && (char *)node != sentinel && n < 512; node = (void **)node[0], n++)
        if (node[2] && touch_class_of(node[2]) >= 0)
            ((uint8_t *)node[2])[BTN_FOCUS] = 0;
    ((uint8_t *)mbtn[idx].obj)[BTN_FOCUS] = 1;
    focus_idx = idx;
    focus_obj = mbtn[idx].obj;
}

// A from gombtól az adott irányba eső legközelebbi gomb (a középpontok alapján, ~60°-os kúpban).
static int nearest_button(int from, int dir) {
    const MenuBtn *f = &mbtn[from];
    float cx = (f->x0 + f->x1) * 0.5f, cy = (f->y0 + f->y1) * 0.5f;
    float dx = dpad_dir[dir][0], dy = dpad_dir[dir][1];
    int best = -1;
    float best_score = 1e9f;
    for (int i = 0; i < mbtn_count; i++) {
        if (i == from)
            continue;
        float vx_ = (mbtn[i].x0 + mbtn[i].x1) * 0.5f - cx, vy_ = (mbtn[i].y0 + mbtn[i].y1) * 0.5f - cy;
        float along = vx_ * dx + vy_ * dy;
        float perp = fabsf(vx_ * dy - vy_ * dx);
        if (along < 2.0f || perp > along * 1.8f)
            continue;
        float score = along + perp * 2.0f;
        if (score < best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}

// Alapértelmezett fókusz: a legfelső (azonos sorban a bal szélső) gomb.
static void focus_default(void) {
    int best = -1;
    for (int i = 0; i < mbtn_count; i++) {
        if (best < 0 || mbtn[i].y0 + mbtn[i].y1 < mbtn[best].y0 + mbtn[best].y1 - 4 ||
            (abs((mbtn[i].y0 + mbtn[i].y1) - (mbtn[best].y0 + mbtn[best].y1)) <= 4 && mbtn[i].x0 < mbtn[best].x0))
            best = i;
    }
    if (best >= 0) {
        set_focus(best);
        menu_log("alapfókusz → %p", mbtn[best].obj);
    }
}

static void nav_move(int dir) {
    rule_pending = NULL;
    if (focus_idx < 0) {
        focus_default();
        return;
    }
    void *obj = mbtn[focus_idx].obj;
    void *target = button_rule(obj, dir_action[dir]);
    if (target) {
        // a játék saját szabálya (hanggal, a menü saját logikájával együtt)
        *menu_action = dir_action[dir];
        rule_pending = obj;
        rule_pending_dir = dir;
        menu_log("%d: szabály %p → %p", dir_action[dir], obj, target);
        return;
    }
    if (dir <= 1 && tab_of(cur_state, obj)) {
        // fül-lap: a játék lépteti a saját indexét (ha mégsem lépne, a rule_pending miatt mi lépünk)
        *menu_action = dir_action[dir];
        rule_pending = obj;
        rule_pending_dir = dir;
        menu_log("%d: fül-lap %p, a játéknak adva", dir_action[dir], obj);
        return;
    }
    int t = nearest_button(focus_idx, dir);
    if (t >= 0) {
        set_focus(t);
        menu_log("%d: geometria %p → %p", dir_action[dir], obj, mbtn[t].obj);
    } else {
        *menu_action = dir_action[dir];   // nincs ott gomb: a játék kezelje (lista, csúszka)
        menu_log("%d: nincs gomb arra, a játéknak adva", dir_action[dir]);
    }
}

// Szabad kurzor (tartalék).
static void poll_cursor(u64 down, u64 up, float lx, float ly, uint64_t now, float dt) {
    if (lx != 0.0f || ly != 0.0f) {
        // teljes kitérésnél kb. 1,1 mp alatt ér át a képernyőn; négyzetes görbe a finom célzáshoz
        float mag = sqrtf(lx * lx + ly * ly);
        float speed = 1150.0f * (float)screen_width / 1280.0f * mag;
        cur_x += lx * speed * dt;
        cur_y += ly * speed * dt;
        if (cur_x < 0) cur_x = 0;
        if (cur_y < 0) cur_y = 0;
        if (cur_x > screen_width - 1) cur_x = screen_width - 1;
        if (cur_y > screen_height - 1) cur_y = screen_height - 1;
        cur_active_ms = now;
        if (cur_touch.held) {
            touch(TOUCH_MOVE, (int)cur_x, (int)cur_y, PTR_CURSOR);
            cur_touch.x = (int)cur_x;
            cur_touch.y = (int)cur_y;
        }
    }

    if (down & HidNpadButton_A) {
        vtouch_press(&cur_touch, PTR_CURSOR, (int)cur_x, (int)cur_y);
        cur_active_ms = now;
    }
    if (up & HidNpadButton_A)
        vtouch_release(&cur_touch, PTR_CURSOR);

    // a D-pad itt is a játék menü-akcióit adja (listák, csúszkák)
    for (int i = 0; i < 4; i++)
        if ((down & dpad_hid[i]) && menu_action)
            *menu_action = dir_action[i];
}

static void poll_menu(u64 buttons, u64 down, u64 up, HidAnalogStickState ls, uint64_t now, float dt) {
    float lx = ls.x / (float)JOYSTICK_MAX;
    float ly = -ls.y / (float)JOYSTICK_MAX;    // a HID-ben a felfelé pozitív, a képernyőn lefelé
    coord_normalize(&lx, &ly, settings.left_deadzone, 1.0f);

    menu_scan(cur_state);
    bool use_cursor = cursor_forced || mbtn_count == 0;
    if (use_cursor != menu_cursor) {
        vtouch_release(&cur_touch, PTR_CURSOR);
        vtouch_release(&focus_touch, PTR_FOCUS);
        menu_cursor = use_cursor;
        nav_dir = -1;
        rule_pending = NULL;
        menu_log("%s", use_cursor ? "szabad kurzor" : "fókusz-navigáció");
    }

    if (up & HidNpadButton_B) {
        if (ingame_menu_back(cur_state))
            menu_log("B: a szünetmenü Back gombja");
        else
            send_back();
    }
    // L/R (ZL/ZR): előző/következő fül (CTabControl)
    if (menu_action && (down & (HidNpadButton_L | HidNpadButton_ZL)))
        *menu_action = MA_TAB_PREV;
    if (menu_action && (down & (HidNpadButton_R | HidNpadButton_ZR)))
        *menu_action = MA_TAB_NEXT;

    if (use_cursor) {
        poll_cursor(down, up, lx, ly, now, dt);
        return;
    }

    // A játék szabálya szerint lépnie kellett volna, de a fókusz maradt (a gomb Update-je nem fut,
    // vagy a célgomb nem fókuszálható): akkor lépünk mi.
    if (rule_pending) {
        void *was = rule_pending;
        rule_pending = NULL;
        if (focus_idx >= 0 && mbtn[focus_idx].obj == was) {
            int t = nearest_button(focus_idx, rule_pending_dir);
            if (t >= 0) {
                set_focus(t);
                menu_log("a szabály nem lépett, geometria → %p", mbtn[t].obj);
            }
        }
    }

    // Irány: D-pad, vagy a bal kar (fő tengelye, fél kitérés felett). Nyomva tartva ismétel.
    int dir = -1;
    for (int i = 0; i < 4 && dir < 0; i++)
        if (buttons & dpad_hid[i])
            dir = i;
    if (dir < 0 && fmaxf(fabsf(lx), fabsf(ly)) > 0.5f)
        dir = fabsf(ly) >= fabsf(lx) ? (ly < 0 ? 0 : 1) : (lx < 0 ? 2 : 3);
    if (dir != nav_dir) {
        nav_dir = dir;
        if (dir >= 0) {
            nav_move(dir);
            nav_next_ms = now + 400;
        }
    } else if (dir >= 0 && now >= nav_next_ms) {
        nav_move(dir);
        nav_next_ms = now + 150;
    }

    // A: koppintás a fókuszban lévő gomb közepére; felengedés ugyanott.
    if (down & HidNpadButton_A) {
        if (focus_idx < 0) {
            focus_default();
        } else if (mbtn[focus_idx].tap) {
            const MenuBtn *m = &mbtn[focus_idx];
            int sx = l2sx((m->x0 + m->x1) / 2), sy = l2sy((m->y0 + m->y1) / 2);
            vtouch_press(&focus_touch, PTR_FOCUS, sx, sy);
            menu_log("A → %p, képernyő (%d,%d)", m->obj, sx, sy);
        }
    }
    if (up & HidNpadButton_A)
        vtouch_release(&focus_touch, PTR_FOCUS);
}

// ---------------------------------------------------------------------------
// Játékmenet (a Vita reimpl/controls.c logikája, a célpontok a HUD-ból)

// Melyik HUD-gombot nyomja a logikai gomb (a Vita-pontok a 480×320-as logikai képernyőn pont
// ezekre a gombokra estek; ágyún/lóháton/célzáskor a Vita is máshova koppintott).
static int vb_target(int vb, bool aim, bool cannon, bool horse) {
    switch (vb) {
    case VB_AIM:      return aim ? HUD_SHOOT : HUD_SECOND;            // célzás / célzás vége (PistolSheath)
    case VB_FIRE:     return cannon ? HUD_SHOOT : HUD_SECOND;         // lövés; ágyún CannonShoot
    case VB_INTERACT: return cannon ? HUD_SECOND : (horse ? HUD_SHOOT : HUD_COUNTER);   // CannonExit / GetOffHorse / Contextual
    case VB_ATTACK:   return horse ? HUD_COUNTER : HUD_SHOOT;
    case VB_JUMP:     return HUD_JUMP;
    case VB_MAP:      return HUD_MINIMAP;
    case VB_COUNTER:  return HUD_COUNTER;
    default:          return HUD_COUNTER;                              // VB_COMBO: Vengeance
    }
}

// A Vita-port pontjai (960×544), tartaléknak.
static void vita_button_pos(int vb, bool aim, bool cannon, bool horse, float *x, float *y) {
    float px, py;
    switch (vb) {
    case VB_AIM:      px = aim ? 863 : 720;  py = aim ? 486 : 485; break;
    case VB_FIRE:     px = cannon ? 806 : 720; py = cannon ? 484 : 485; break;
    case VB_INTERACT:
        if (cannon)     { px = 694; py = 489; }   // ágyún a △ ugyanaz, mint az L
        else if (horse) { px = 862; py = 486; }   // lóháton a △ és a □ helyet cserél
        else            { px = 880; py = 315; }
        break;
    case VB_ATTACK:   px = horse ? 880 : 862; py = horse ? 315 : 486; break;
    case VB_JUMP:     px = 770; py = 381; break;
    case VB_MAP:      px = 70;  py = 86;  break;
    case VB_COUNTER:  px = 913; py = 355; break;
    default:          px = 880; py = 315; break;  // VB_COMBO
    }
    *x = vx(px);
    *y = vy(py);
}

// Lenyomja a logikai gombhoz tartozó HUD-gombot. Ha a gomb most nem fogad érintést (tiltva), nem
// koppintunk: oda a kamera-joystick kapná (képernyő-koppintás, dupla koppintás).
static void vb_press(int vb, bool aim, bool cannon, bool horse) {
    int t = vb_target(vb, aim, cannon, horse);
    int sx, sy;
    if (hud_valid && hud[t].obj) {
        bool ok = hud_point(t, &sx, &sy);
        log_tap(vb, t, sx, sy, ok);
        if (!ok)
            return;
    } else {
        float fx, fy;
        vita_button_pos(vb, aim, cannon, horse, &fx, &fy);
        sx = (int)fx;
        sy = (int)fy;
        log_tap(vb, -1, sx, sy, true);
    }
    vtouch_press(&vbtn[vb], PTR_BUTTON + vb, sx, sy);
}

// A bal joystick lenyomási pontja és teljes húzása (logikai képpont). Hamis, ha a mozgás tiltva van.
static bool stick_geometry(float *ox, float *oy, float *reach) {
    int maxd = joy_max_distance ? *joy_max_distance : 0;
    *reach = maxd > 0 ? (float)maxd * LSTICK_OVERDRIVE : vlx(LSTICK_R);
    if (hud_valid && hud[HUD_MOVE].obj) {
        const HudCtl *c = &hud[HUD_MOVE];
        if (!c->enabled)
            return false;
        // A WalkJoystick középpontja a lenyomás helye (lebegő). A területe közepén nyomjuk le, de úgy,
        // hogy a teljes húzás is a képernyőn maradjon – és a lenyomás a területén belül.
        float x = (c->x0 + c->x1) * 0.5f, y = (c->y0 + c->y1) * 0.5f;
        x = clampf(x, *reach, lw - 1 - *reach);
        y = clampf(y, *reach, lh - 1 - *reach);
        *ox = clampf(x, c->x0, c->x1);
        *oy = clampf(y, c->y0, c->y1);
        return true;
    }
    *ox = vlx(LSTICK_X);
    *oy = vly(LSTICK_Y);
    return true;
}

static void poll_gameplay(u32 vb_now, u64 down, u64 up, HidAnalogStickState ls, HidAnalogStickState rs,
                          uint64_t now, float dt) {
    u32 vb_down = vb_now & ~vb_prev;
    u32 vb_up = ~vb_now & vb_prev;

    void *pc = player_component();
    bool aim = pc && PC_IsInAimMode(pc);
    bool cannon = pc && PC_IsOnCannon(pc);
    bool horse = pc && PC_IsOnHorse(pc);
    bool can_call_horse = pc && PC_CanCallHorse(pc);
    bool vengeance = pc && PC_CanUseVengeance(pc) && !PC_CanUseInteractButton(pc);

    // R + □ együtt: „Vengeance”-kombó (a Vitán is). Amíg tart, a két gomb külön nem koppint.
    bool fire_held = vb_now & VB_BIT(VB_FIRE), attack_held = vb_now & VB_BIT(VB_ATTACK);
    if (fire_held && attack_held && !combo_active) {
        vb_press(VB_COMBO, aim, cannon, horse);
        combo_active = true;
    }

    // Ló hívása: az X nyomva tartása (a Vitán 15 képkocka ≈ 0,5 mp), lenyomásonként egyszer. A HUD
    // „Horse” gombja (69-es esemény) a játékban pontosan ezt hívja: CanCallHorse → CallHorse.
    if (vb_down & VB_BIT(VB_INTERACT))
        interact_down_ms = now;
    if (vb_up & VB_BIT(VB_INTERACT))
        interact_down_ms = 0;
    if (interact_down_ms && now - interact_down_ms >= 500 && can_call_horse) {
        PC_CallHorse(pc);
        interact_down_ms = 0;
    }

    for (int i = 0; i < VB_COMBO; i++) {
        bool combo_part = combo_active && (i == VB_FIRE || i == VB_ATTACK);
        if (vb_down & VB_BIT(i)) {
            if (combo_part || (vengeance && (i == VB_COUNTER || i == VB_INTERACT))) {
                // kombó alatt / bosszú-lehetőségnél (a ○ és △ helyén a bosszú-gomb van) nem koppint
            } else if (aim && i == VB_INTERACT) {
                // célzás közben a △ a következő gránát/fegyver (a Vitán is)
                PC_IncrementGrenadeSelection(pc, 1);
                PC_EnterAimMode(pc);
            } else {
                vb_press(i, aim, cannon, horse);
            }
        }
        if ((vb_up & VB_BIT(i)) && !combo_part)
            vtouch_release(&vbtn[i], PTR_BUTTON + i);
    }

    if (combo_active && !(fire_held && attack_held)) {
        vtouch_release(&vbtn[VB_COMBO], PTR_BUTTON + VB_COMBO);
        combo_active = false;
        // ami a kombó előtt lenyomva maradt és azóta felengedték, azt most engedjük el
        if (!fire_held)
            vtouch_release(&vbtn[VB_FIRE], PTR_BUTTON + VB_FIRE);
        if (!attack_held)
            vtouch_release(&vbtn[VB_ATTACK], PTR_BUTTON + VB_ATTACK);
    }

    // --- bal kar és D-pad: a WalkJoystick húzása ---

    float ox = 0.0f, oy = 0.0f, reach = 0.0f;
    bool can_move = stick_geometry(&ox, &oy, &reach) && !cannon;   // ágyún a bal kar nem mozgat

    // D-pad: lenyomás a joystick pontján, húzás teljes kitérésig, felengedés ott (mint a Vitán)
    for (int i = 0; i < 4; i++) {
        if ((down & dpad_hid[i]) && !vdpad[i].held && can_move) {
            vtouch_press(&vdpad[i], PTR_DPAD + i, l2sx(ox), l2sy(oy));
            touch(TOUCH_MOVE, l2sx(ox + dpad_dir[i][0] * reach), l2sy(oy + dpad_dir[i][1] * reach), PTR_DPAD + i);
        }
        if (up & dpad_hid[i])
            vtouch_release(&vdpad[i], PTR_DPAD + i);
    }

    float lx = ls.x / (float)JOYSTICK_MAX;
    float ly = -ls.y / (float)JOYSTICK_MAX;          // a HID-ben a felfelé pozitív
    float rx = rs.x / (float)JOYSTICK_MAX;
    float ry = -rs.y / (float)JOYSTICK_MAX;
    coord_normalize(&lx, &ly, settings.left_deadzone, 1.0f);
    coord_normalize(&rx, &ry, settings.right_deadzone, 1.0f);

    // Bal kar: a WalkJoystick tengelyenként (ujj − lenyomás) / MAX_DISTANCE-et ad (±1-re vágva), ebből
    // dönt a séta és a futás között. A kitéréssel arányosan húzunk, logikai képpontban.
    // Ha a joystick nem vette át a lenyomást (pl. a játékos épp olyan állapotban volt, amelyben a
    // joystick nem fogad érintést), egyszer újra lenyomjuk.
    if (l_active && hud_valid && hud[HUD_MOVE].obj && !hud[HUD_MOVE].pressed && !l_retried &&
        now - l_down_ms > 300) {
        touch(TOUCH_UP, l_last_x, l_last_y, PTR_LSTICK);
        l_active = false;
        l_retried = true;
    }
    if ((lx != 0.0f || ly != 0.0f) && can_move) {
        if (!l_active) {
            touch(TOUCH_DOWN, l2sx(ox), l2sy(oy), PTR_LSTICK);
            l_down_ms = now;
        }
        int tx = l2sx(ox + reach * lx), ty = l2sy(oy + reach * ly);
        touch(TOUCH_MOVE, tx, ty, PTR_LSTICK);
        l_active = true;
        l_last_x = tx;
        l_last_y = ty;
    } else {
        if (l_active) {
            touch(TOUCH_UP, l_last_x, l_last_y, PTR_LSTICK);
            l_active = false;
        }
        l_retried = false;
    }

    // --- jobb kar: kamera ---
    // Négyzetes görbe (finom közép), időarányos (fps-től független), config.txt: cameraSensitivity (%).
    // Elsődlegesen a játék saját forgatás-eseménye; ha a forgatás tiltva van, semmi.
    float rmag = sqrtf(rx * rx + ry * ry);
    bool rotate_ok = !(hud_valid && hud[HUD_ROTATE].obj && !hud[HUD_ROTATE].enabled);
    if (rmag > 0.0f && rotate_ok) {
        float k = CAMERA_SPEED * (float)settings.camera_sensitivity / 100.0f * rmag * dt;
        if (!raise_stick_move(EV_ROTATE_CAMERA, rx * k, ry * k))
            camera_finger_drag(rx * k, ry * k);
    } else {
        camera_finger_release();
    }
}

// ---------------------------------------------------------------------------

void controls_init(void) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();

    nativeOnTouch = game_sym("Java_com_gameloft_android_ANMP_GloftSDHM_GameGLSurfaceView_nativeOnTouch");
    CheckInputKey = game_sym("_Z13CheckInputKeyi17EVENT_ACTION_TYPEii");
    AbsorbKey = game_sym("_Z9AbsorbKeyi");
    Application_GetInstance = game_sym("_ZN11Application11GetInstanceEv");
    gxStateStack_CurrentState = game_sym("_ZN12gxStateStack12CurrentStateEv");
    isPressKey = game_sym("isPressKey");
    nativeOnKeyUp = game_sym("Java_com_gameloft_android_ANMP_GloftSDHM_Game_nativeOnKeyUp");

    CLevel_GetLevel = game_sym("_ZN6CLevel8GetLevelEv");
    CLevel_GetPlayerComponent = game_sym("_ZN6CLevel18GetPlayerComponentEv");
    PC_IsInAimMode = game_sym("_ZN15PlayerComponent11IsInAimModeEv");
    PC_IsOnCannon = game_sym("_ZN15PlayerComponent10IsOnCannonEv");
    PC_IsOnHorse = game_sym("_ZN15PlayerComponent9IsOnHorseEv");
    PC_CanCallHorse = game_sym("_ZN15PlayerComponent12CanCallHorseEv");
    PC_CallHorse = game_sym("_ZN15PlayerComponent9CallHorseEv");
    PC_CanUseVengeance = game_sym("_ZN15PlayerComponent15CanUseVengeanceEv");
    PC_CanUseInteractButton = game_sym("_ZN15PlayerComponent20CanUseInteractButtonEv");
    PC_IncrementGrenadeSelection = game_sym("_ZN15PlayerComponent25IncrementGrenadeSelectionEi");
    PC_EnterAimMode = game_sym("_ZN15PlayerComponent12EnterAimModeEv");
    joy_max_distance = game_sym("_ZN14AnalogJoystick12MAX_DISTANCEE");

    screen_scale_w = game_sym_opt("s_screenScaleW");
    screen_scale_h = game_sym_opt("s_screenScaleH");
    logical_w = game_sym_opt("s_logicalScreenWidth");
    logical_h = game_sym_opt("s_logicalScreenHeight");
    Multi_GetFirstEnabled = game_sym_opt("_ZN15CButtonSprMulti15GetFirstEnabledEv");
    EventManager_raiseSync = game_sym_opt("_ZN12EventManager9raiseSyncERK6IEvent");
    event_manager = game_sym_opt("_ZN12EventManager9SingletonE");
    vt_ev_stick_move = game_vtable("_ZTV11EvStickMove");
    vt_scheme = game_vtable("_ZTV13ControlScheme");
    vt_scheme4 = game_vtable("_ZTV14ControlScheme4");
    const void *vt_multi = game_vtable("_ZTV15CButtonSprMulti");
    vt_want[HUD_MOVE] = game_vtable("_ZTV12WalkJoystick");
    vt_want[HUD_ROTATE] = game_vtable("_ZTV24FullScreenRotateJoystick");
    vt_want[HUD_SHOOT] = vt_want[HUD_JUMP] = vt_want[HUD_SECOND] = vt_want[HUD_COUNTER] = vt_want[HUD_HORSE] = vt_multi;
    vt_want[HUD_GRENADE] = game_vtable("_ZTV12CButtonSweep");
    vt_want[HUD_MINIMAP] = game_vtable("_ZTV14CButtonMinimap");
    if (!screen_scale_w || !screen_scale_h)
        vt_scheme = NULL;   // skála nélkül a HUD-koordináták nem válthatók át: Vita-tartalék

    menu_action = game_sym_opt("menu_action");   // nélküle csak a szabad kurzor marad
    vt_ingame_menu = game_vtable("_ZTV13GS_InGameMenu");
    open_cf_main_menu = game_sym_opt("is_open_cf_main_menu_screen");
    for (int i = 0; i < TOUCH_CLASS_COUNT; i++)
        touch_class_vt[i] = game_vtable(touch_class[i].sym);

    update_scale();
    cur_x = screen_width / 2.0f;
    cur_y = screen_height / 2.0f;
    last_poll_ms = time_ms();
    log_info("controls: kész");
}

#ifdef DEBUG
// autoinput (csak debug): sdmc:/switch/backstab/autoinput – időzített gombnyomások a teszthez, hogy pl. a mentés
// betöltése ne kívánjon billentyűt az Eden-ablakba. Tokenek: „<mp>:<gomb>”, az mp az előző lépéshez képest (az
// elsőé a játék indulásától, a bevezető videó után); gomb: A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT, vagy
// SHOT (képkocka-mentés: shots/auto_<n>.rgba). Pl. „40:SHOT 1:A 8:A 4:A 4:DOWN 1:DOWN 1:A”.
static const struct { const char *name; u64 bit; } ai_names[] = {
    {"A", HidNpadButton_A}, {"B", HidNpadButton_B}, {"X", HidNpadButton_X}, {"Y", HidNpadButton_Y},
    {"L", HidNpadButton_L}, {"R", HidNpadButton_R}, {"ZL", HidNpadButton_ZL}, {"ZR", HidNpadButton_ZR},
    {"PLUS", HidNpadButton_Plus}, {"MINUS", HidNpadButton_Minus}, {"UP", HidNpadButton_Up},
    {"DOWN", HidNpadButton_Down}, {"LEFT", HidNpadButton_Left}, {"RIGHT", HidNpadButton_Right}, {"SHOT", 0},
};
static struct { float delay; int name; } ai_step[64];
static int ai_n = -1, ai_pos;
static uint64_t ai_t0, ai_release;
static u64 ai_held;

static void ai_load(void) {
    ai_n = 0;
    FILE *f = fopen(DATA_PATH "autoinput", "r");
    if (!f)
        return;
    char tok[32], name[16];
    float d;
    while (ai_n < 64 && fscanf(f, "%31s", tok) == 1) {
        if (sscanf(tok, "%f:%15s", &d, name) != 2)
            continue;
        for (int i = 0; i < (int)(sizeof(ai_names) / sizeof(ai_names[0])); i++)
            if (!strcmp(name, ai_names[i].name)) {
                ai_step[ai_n].delay = d;
                ai_step[ai_n++].name = i;
            }
    }
    fclose(f);
    log_info("[autoinput] %d lépés", ai_n);
}

// a most „lenyomott” gombok (150 ms-ig tart egy nyomás)
static u64 autoinput(uint64_t now) {
    if (ai_n < 0) {
        ai_load();
        ai_t0 = now;
    }
    if (ai_held && now >= ai_release)
        ai_held = 0;
    if (!ai_held && ai_pos < ai_n && now - ai_t0 >= (uint64_t)(ai_step[ai_pos].delay * 1000.0f)) {
        int k = ai_step[ai_pos].name;
        log_info("[autoinput] %d/%d: %s", ai_pos + 1, ai_n, ai_names[k].name);
        if (ai_names[k].bit) {
            ai_held = ai_names[k].bit;
            ai_release = now + 150;
        } else {
            char shot[32];
            snprintf(shot, sizeof(shot), "auto_%02d", ai_pos + 1);
            gl_debug_shot(shot);
        }
        ai_t0 = now;
        ai_pos++;
    }
    return ai_held;
}
#endif

void controls_poll(void) {
    uint64_t now = time_ms();
    draw_frame++;
    float dt = (now - last_poll_ms) / 1000.0f;
    if (dt > 0.1f)
        dt = 0.1f;   // hosszú töltés után ne ugorjon el a kurzor / a kamera
    last_poll_ms = now;

    padUpdate(&pad);
    u64 buttons = padGetButtons(&pad);
#ifdef DEBUG
    if (Application_GetInstance())
        buttons |= autoinput(now);
#endif
    u64 down = buttons & ~hid_prev;
    u64 up = ~buttons & hid_prev;
    hid_prev = buttons;
    HidAnalogStickState ls = padGetStickPos(&pad, 0);
    HidAnalogStickState rs = padGetStickPos(&pad, 1);

    u32 vb_now = 0;
    for (int i = 0; i < VB_COUNT; i++)
        if (buttons & vb_hid[i])
            vb_now |= VB_BIT(i);

    poll_touchscreen();

    void *app = Application_GetInstance();
    if (!app) {
        vb_prev = vb_now;
        return;
    }
    update_scale();

    // + = START (szünet/menü) minden módban, a Vitán is így
    if (down & HidNpadButton_Plus) {
        send_key(AKEYCODE_BUTTON_START, true);
        *isPressKey = AKEYCODE_BUTTON_START;   // a Vitán is így nyílik a szünetmenü
    }
    if (up & HidNpadButton_Plus)
        send_key(AKEYCODE_BUTTON_START, false);

    bool in_menu = game_in_menu(app);
    // R3: szabad kurzor be/ki (játék közben és menüben is); új képernyőn alapból kikapcsol.
    void *state = gxStateStack_CurrentState((char *)app + 4);
    if (state != cur_state) {
        cur_state = state;
        cursor_forced = false;
        state_draws = 0;
    }
    if (down & HidNpadButton_StickR) {
        cursor_forced = !cursor_forced;
        cur_active_ms = now;
    }

    bool want_menu = in_menu || cursor_forced;
    if (want_menu != menu_mode) {
        release_all_virtual();
        menu_mode = want_menu;
        if (menu_mode)
            cur_active_ms = cursor_forced ? now : 0;
    }

    if (menu_mode) {
        poll_menu(buttons, down, up, ls, now, dt);
    } else {
        // Új pálya / más séma: a régi joystickra szóló ujjakat elengedjük, a következő képkockában
        // az új HUD-ra nyomunk le.
        if (hud_update())
            release_all_virtual();
        poll_gameplay(vb_now, down, up, ls, rs, now, dt);
    }

    vb_prev = vb_now;
}

bool controls_focus_rect(int *x, int *y, int *w, int *h) {
    if (!menu_mode || menu_cursor || focus_idx < 0 || focus_idx >= mbtn_count)
        return false;
    const MenuBtn *m = &mbtn[focus_idx];
    *x = (int)(m->x0 * sc_w);
    *y = (int)(m->y0 * sc_h);
    *w = (int)((m->x1 - m->x0) * sc_w);
    *h = (int)((m->y1 - m->y0) * sc_h);
    return *w > 0 && *h > 0;
}

bool controls_cursor(int *x, int *y) {
    if (!menu_mode || !menu_cursor)
        return false;
    // Dokkolva mindig látszik; kézi módban (ahol ujjal is lehet) csak ha a kart használták az utóbbi 4 mp-ben.
    bool docked = appletGetOperationMode() == AppletOperationMode_Console;
    if (!docked && (cur_active_ms == 0 || time_ms() - cur_active_ms > 4000))
        return false;
    *x = (int)cur_x;
    *y = (int)cur_y;
    return true;
}
