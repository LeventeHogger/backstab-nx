/*
 * jni_impl.c – a FalsoJNI metódus- és mezőtáblái (a játék Java-oldala: Game/GameRenderer, AudioTrack)
 *
 * A backstab-vita loader/falsojni_impl.c portja. A metódusneveket a libPirates.so GetStaticMethodID/
 * GetMethodID-hívásai adják (a nevek a .rodata-ban: 0x674260–0x6743b0, AudioTrack: 0x68da00–0x68da6c).
 * A FalsoJNI-ben a jstring sima char* (NewStringUTF = strdup), a DeleteGlobalRef pedig free()-t hív
 * rá – ezért a visszaadott sztringek strdup-pal készülnek, nem literálok.
 *
 * Copyright (C) 2022-2023 Volodymyr Atamanenko (backstab-vita)
 * Copyright (C) 2026 Levi (Switch port)
 * MIT license, lásd LICENSE.
 */
#include <FalsoJNI/FalsoJNI_Impl.h>

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <switch.h>

#include "audio.h"
#include "game.h"
#include "util.h"

static jobject java_string(const char *s) {
    char *p = strdup(s);
    if (!p)
        fatal_error("Elfogyott a memória (JNI sztring).");
    return (jobject)p;
}

// --- eszközadatok ---

// kHz-ben, mint az Android cpuinfo_max_freq-je: a CGameConfig::CalculateDevicePower 1,2e6 / 1,0e6-tal veti össze
// (0x219400). ≥ 1,2e6 → CPU-profil 3 (magas geometria, nincs járókelő-korlát); a régi 1020.0 (MHz-nek szánva)
// 1 MHz-nek számított → profil 1, alacsony geometria és m_iNPC_Limit_Level = 1 (legfeljebb 2 járókelő).
// 1785 MHz a Switch CPU-jának legmagasabb órajele.
jfloat GetPhoneCPUFreq(jmethodID id, va_list args) {
    return 1785000.0f;
}

jint getManufacture(jmethodID id, va_list args) {
    // A Vita-port 10-et (Sony Ericsson) ad, ezzel tesztelték. A játék ezt csak gyártóspecifikus
    // kerülőutakra használja (pl. isHtcDevice: 3 vagy 6), a 10 egyikbe sem esik.
    return 10;
}

jint GetOSVersion(jmethodID id, va_list args) {
    return 10;   // Build.VERSION.SDK_INT: GINGERBREAD_MR1 (Android 2.3.3), mint a Vitán
}

jint GetPhoneLanguage(jmethodID id, va_list args) {
    // A játék kódjai: 0 en, 1 fr, 2 de, 3 es, 4 it, 5 pt, 6 ja, 7 ko
    static int cached = -1;
    if (cached >= 0)
        return cached;

    int game_lang = 0;
    u64 code = 0;
    SetLanguage lang = SetLanguage_ENUS;
    if (R_SUCCEEDED(setInitialize())) {
        if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &lang))) {
            switch (lang) {
                case SetLanguage_FR:
                case SetLanguage_FRCA:
                    game_lang = 1;
                    break;
                case SetLanguage_DE:
                    game_lang = 2;
                    break;
                case SetLanguage_ES:
                case SetLanguage_ES419:
                    game_lang = 3;
                    break;
                case SetLanguage_IT:
                    game_lang = 4;
                    break;
                case SetLanguage_PT:
                case SetLanguage_PTBR:
                    game_lang = 5;
                    break;
                case SetLanguage_JA:
                    game_lang = 6;
                    break;
                case SetLanguage_KO:
                    game_lang = 7;
                    break;
                default:   // angol, kínai, holland, orosz… → angol
                    game_lang = 0;
                    break;
            }
        }
        setExit();
    }
    log_info("GetPhoneLanguage: rendszernyelv %d → %d", (int)lang, game_lang);
    cached = game_lang;
    return game_lang;
}

jint GetTextureFormat(jmethodID id, va_list args) {
    // A játék (SetTextureFormat, 0x20cfa4) ebből választja a textúrafájlok kiterjesztését
    // (g_szTextureExtension, alap ".etc"): 1 → ".pvr", 2 → ".atc", 3 → ".dds", más → marad ".etc".
    // A letöltött adatcsomag (*.gla) viszont CSAK .pvr fájlokat tartalmaz (PVR v2 konténer:
    // 452× PVRTC4, 44× ETC1, 7× RGBA8888) – .etc/.atc/.dds egy sincs, azokkal semmi nem töltődne be.
    // Ezért 1 marad, mint a Vitán. A nouveau a PVRTC-t nem ismeri (nincs GL_IMG_texture_compression_pvrtc),
    // de a glitch-motor ilyenkor maga bontja ki szoftveresen (PVRTCDecompress a pixel_format::convert-ben,
    // napló: "slow path pixel format conversion"); az ETC1-et a Mesa natívan kezeli.
#ifdef DEBUG
    if (screen_width == 1280)   // ref800 (main.c): az eredeti Gameloft-csomag sprite-jai DXT-sek (.dds)
        return 3;
#endif
    return 1;
}

jint isWifiEnabled(jmethodID id, va_list args) {
    return 0;
}

jlong GetCurrentTime(jmethodID id, va_list args) {
    // falióra ms-ban (a Vitán is gettimeofday) – a játék a DRM/időzítéshez valós időt vár
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (jlong)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

jobject GetPhoneCPUName(jmethodID id, va_list args) {
    return java_string("ARMv7 Processor rev 1 (v7l)");
}

jobject GetPhoneGPUName(jmethodID id, va_list args) {
    return java_string("NVIDIA Tegra X1");
}

jobject GetPhoneManufacturer(jmethodID id, va_list args) {
    return java_string("Nintendo");
}

jobject GetPhoneModel(jmethodID id, va_list args) {
    return java_string("Switch");
}

// --- életciklus, egyebek ---

void Exit(jmethodID id, va_list args) {
    log_info("Exit() a játéktól – kilépés");
    audio_exit();
    exit(0);
}

void GameTracking(jmethodID id, va_list args) {
}

void GC(jmethodID id, va_list args) {
}

void launchGLLive(jmethodID id, va_list args) {
}

void launchIGP(jmethodID id, va_list args) {
}

void notifyTrophy(jmethodID id, va_list args) {
    int trophy = va_arg(args, int);
    log_info("notifyTrophy(%d)", trophy);
}

void openBrowser(jmethodID id, va_list args) {
}

void Pause(jmethodID id, va_list args) {
}

void PrintDebug(jmethodID id, va_list args) {
    const char *str = va_arg(args, const char *);
    log_info("[PrintDebug] %s", str ? str : "(null)");
}

void sendAppToBackground(jmethodID id, va_list args) {
}

// ---------------------------------------------------------------------------
// FalsoJNI-táblák

NameToMethodID nameToMethodId[] = {
    { 1, "Exit", METHOD_TYPE_VOID },                          // ()V
    { 2, "openBrowser", METHOD_TYPE_VOID },                   // (Ljava/lang/String;)V
    { 3, "isWifiEnabled", METHOD_TYPE_INT },                  // ()I
    { 4, "Pause", METHOD_TYPE_VOID },                         // ()V
    { 5, "GetPhoneLanguage", METHOD_TYPE_INT },               // ()I
    { 6, "launchGLLive", METHOD_TYPE_VOID },                  // (I)V
    { 7, "getManufacture", METHOD_TYPE_INT },                 // ()I
    { 8, "notifyTrophy", METHOD_TYPE_VOID },                  // (I)V
    { 9, "launchIGP", METHOD_TYPE_VOID },                     // (I)V
    { 10, "GetCurrentTime", METHOD_TYPE_LONG },               // ()J
    { 11, "GetTextureFormat", METHOD_TYPE_INT },              // ()I
    { 12, "PrintDebug", METHOD_TYPE_VOID },                   // (Ljava/lang/String;)V
    { 13, "GetPhoneManufacturer", METHOD_TYPE_OBJECT },       // ()Ljava/lang/String;
    { 14, "GetPhoneModel", METHOD_TYPE_OBJECT },              // ()Ljava/lang/String;
    { 15, "GetPhoneCPUName", METHOD_TYPE_OBJECT },            // ()Ljava/lang/String;
    { 16, "GetPhoneCPUFreq", METHOD_TYPE_FLOAT },             // ()F
    { 17, "GetPhoneGPUName", METHOD_TYPE_OBJECT },            // ()Ljava/lang/String;
    { 18, "GC", METHOD_TYPE_VOID },                           // (I)V
    { 19, "GetOSVersion", METHOD_TYPE_INT },                  // ()I
    { 20, "GameTracking", METHOD_TYPE_VOID },                 // ()V
    { 21, "sendAppToBackground", METHOD_TYPE_VOID },          // ()V
    // android.media.AudioTrack (a FalsoJNI a konstruktort "<osztály>/<init>" néven keresi)
    { 22, "android/media/AudioTrack/<init>", METHOD_TYPE_OBJECT }, // (IIIIII)V
    { 23, "getMinBufferSize", METHOD_TYPE_INT },              // (III)I, statikus
    { 24, "play", METHOD_TYPE_VOID },                         // ()V
    { 25, "pause", METHOD_TYPE_VOID },                        // ()V
    { 26, "stop", METHOD_TYPE_VOID },                         // ()V
    { 27, "release", METHOD_TYPE_VOID },                      // ()V
    { 28, "write", METHOD_TYPE_INT },                         // ([BII)I
};

MethodsBoolean methodsBoolean[] = {};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {
    { 16, GetPhoneCPUFreq },
};
MethodsInt methodsInt[] = {
    { 3, isWifiEnabled },
    { 5, GetPhoneLanguage },
    { 7, getManufacture },
    { 11, GetTextureFormat },
    { 19, GetOSVersion },
    { 23, audioTrack_getMinBufferSize },
    { 28, audioTrack_write },
};
MethodsLong methodsLong[] = {
    { 10, GetCurrentTime },
};
MethodsObject methodsObject[] = {
    { 13, GetPhoneManufacturer },
    { 14, GetPhoneModel },
    { 15, GetPhoneCPUName },
    { 17, GetPhoneGPUName },
    { 22, audioTrack_init },
};
MethodsShort methodsShort[] = {};
MethodsVoid methodsVoid[] = {
    { 1, Exit },
    { 2, openBrowser },
    { 4, Pause },
    { 6, launchGLLive },
    { 8, notifyTrophy },
    { 9, launchIGP },
    { 12, PrintDebug },
    { 18, GC },
    { 20, GameTracking },
    { 21, sendAppToBackground },
    { 24, audioTrack_play },
    { 25, audioTrack_pause },
    { 26, audioTrack_stop },
    { 27, audioTrack_release },
};

NameToFieldID nameToFieldId[] = {};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {};
FieldsObject fieldsObject[] = {};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
