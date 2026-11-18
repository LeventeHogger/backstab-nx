/*
 * audio.c – android.media.AudioTrack → libnx audout (48 kHz, sztereó, s16)
 *
 * A játék (vox::DriverAndroid, libPirates.so 0x62578c/0x625470) saját szálat indít ("VoxThread"), és abban:
 *   getMinBufferSize(44100, CHANNEL_OUT_STEREO=12, ENCODING_PCM_16BIT=2) → min (bájt)
 *   new AudioTrack(STREAM_MUSIC=3, 44100, 12, 2, min, MODE_STREAM=1); play(); NewByteArray(min)
 *   ciklus: _FillBuffer(n keret) → write(tömb, 0, n*4), ahol n = min(min/4, 1024) keret,
 *           szünetben (Suspend → pause()) a write helyett usleep(n * 22,7 µs)
 *   a végén stop(), release().
 * A ciklus maga nem alszik, tehát a write-nak kell blokkolnia (mint Androidon a MODE_STREAM),
 * különben a szál 100%-on pörög és a hang előreszalad. A pause/play a fő szálról jön (Suspend/Resume).
 *
 * Felépítés: nincs külön kimeneti szál – a write a hívó szálán újramintavételez 48 kHz-re, és a
 * kimeneti puffereket tölti; ha mind a kimeneti sorban van, a puffer-eseményre vár (nincs pörgés).
 *
 * FONTOS: a libnx32 AudioOutBuffer-struktúrája 32 bites mutatókkal 32 bájtos, a szolgáltatás viszont
 * 0x28 bájtos, csupa u64-es struktúrát vár, és a GetReleasedAudioOutBuffer u64-es címkéket ír vissza
 * (a libnx32 4 bájtos puffert ad neki). Ezért az Append/GetReleased/RegisterBufferEvent hívásokat itt
 * közvetlenül, a helyes elrendezéssel küldjük (az IAudioOut session-t a libnx-től kérjük el).
 *
 * Copyright (C) 2026 Levi (Switch port), MIT license.
 */
#include "audio.h"
#include "port.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "game.h"
#include "util.h"

#define OUT_RATE      48000
#define OUT_FRAMES    1024                       // keret / kimeneti puffer (21,3 ms)
#define OUT_BUF_SIZE  (OUT_FRAMES * 2 * 2)       // 4 KiB: a 0x1000 többszöröse, ahogy az audout kéri
#define OUT_BUFS      4                          // legfeljebb ~85 ms késleltetés
#define WAIT_SLICE_NS 100000000ULL               // 100 ms: ilyen lépésekben figyeljük a leállítást

// Az IAudioOut által várt pufferleíró (a 64 bites libnx AudioOutBuffer-e).
typedef struct {
    u64 next;
    u64 buffer;
    u64 buffer_size;
    u64 data_size;
    u64 data_offset;
} AoBuffer;
_Static_assert(sizeof(AoBuffer) == 0x28, "AoBuffer: 0x28 bájt kell");

// --- kimenet (csak g_write_mutex alatt) ---
static bool g_ok;                                // az audout él
static AoBuffer g_bufs[OUT_BUFS];
static s16 *g_mem[OUT_BUFS];
static bool g_queued[OUT_BUFS];                  // a szolgáltatás sorában van
static int g_fill = -1;                          // a most töltött puffer indexe
static u32 g_fill_frames;
static Event g_buf_event;
static Mutex g_write_mutex;

// --- az AudioTrack állapota (g_state_mutex alatt) ---
enum { TRACK_NONE, TRACK_STOPPED, TRACK_PLAYING, TRACK_PAUSED };
static int g_track = TRACK_NONE;
static bool g_focus_paused, g_manual_paused, g_shutdown;
static int g_in_rate = 44100, g_in_channels = 2, g_in_format = 2;   // ENCODING_PCM_16BIT
static Mutex g_state_mutex;
static CondVar g_state_cv;

// --- újramintavételezés (g_write_mutex alatt) ---
static u64 g_pos;                                // 32.32 fixpontos hely; 0 = az előző hívás utolsó kerete
static u64 g_step;                               // in_rate / OUT_RATE, 32.32
static s16 g_prev[2];

static AppletHookCookie g_hook;
static bool g_hooked;

// ---------------------------------------------------------------------------
// IAudioOut-parancsok helyes (64 bites) elrendezéssel

static Result ao_register_event(Event *ev) {
    Handle h = INVALID_HANDLE;
    Result rc = serviceDispatch(audoutGetServiceSession_AudioOut(), 4,
        .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
        .out_handles = &h,
    );
    if (R_SUCCEEDED(rc))
        eventLoadRemote(ev, h, false);
    return rc;
}

static Result ao_append(AoBuffer *buf) {
    bool new_cmd = hosversionAtLeast(3, 0, 0);
    u64 tag = (u64)(uintptr_t)buf;
    return serviceDispatchIn(audoutGetServiceSession_AudioOut(), new_cmd ? 7 : 3, tag,
        .buffer_attrs = { (new_cmd ? SfBufferAttr_HipcAutoSelect : SfBufferAttr_HipcMapAlias) | SfBufferAttr_In },
        .buffers = { { buf, sizeof(*buf) } },
    );
}

static Result ao_get_released(u64 *tags, u32 max, u32 *count) {
    bool new_cmd = hosversionAtLeast(3, 0, 0);
    *count = 0;
    return serviceDispatchOut(audoutGetServiceSession_AudioOut(), new_cmd ? 8 : 5, *count,
        .buffer_attrs = { (new_cmd ? SfBufferAttr_HipcAutoSelect : SfBufferAttr_HipcMapAlias) | SfBufferAttr_Out },
        .buffers = { { tags, max * sizeof(u64) } },
    );
}

// A szolgáltatás által visszaadott pufferek felszabadítása; true, ha lett szabad.
static bool reclaim_released(void) {
    u64 tags[OUT_BUFS];
    u32 count = 0;
    bool any = false;
    if (R_FAILED(ao_get_released(tags, OUT_BUFS, &count)))
        return false;
    for (u32 t = 0; t < count && t < OUT_BUFS; t++) {
        for (int i = 0; i < OUT_BUFS; i++) {
            if (tags[t] == (u64)(uintptr_t)&g_bufs[i]) {
                g_queued[i] = false;
                any = true;
            }
        }
    }
    return any;
}

// Szabad kimeneti puffer (blokkol, amíg nincs); -1, ha leállítás jött közben.
static int acquire_buffer(void) {
    for (;;) {
        for (int i = 0; i < OUT_BUFS; i++)
            if (!g_queued[i])
                return i;
        // előbb töröljük az eseményt, aztán kérdezünk: így nem veszhet el jelzés
        eventClear(&g_buf_event);
        if (reclaim_released())
            continue;
        if (__atomic_load_n(&g_shutdown, __ATOMIC_ACQUIRE))
            return -1;
        eventWait(&g_buf_event, WAIT_SLICE_NS);
    }
}

// Egy 48 kHz-es sztereó keret a kimenetre; false, ha leállítás miatt nem ment.
static bool out_push(s16 l, s16 r) {
    if (g_fill < 0) {
        g_fill = acquire_buffer();
        if (g_fill < 0)
            return false;
        g_fill_frames = 0;
    }
    s16 *dst = g_mem[g_fill] + g_fill_frames * 2;
    dst[0] = l;
    dst[1] = r;
    if (++g_fill_frames == OUT_FRAMES) {
        AoBuffer *b = &g_bufs[g_fill];
        b->data_size = OUT_BUF_SIZE;
        b->data_offset = 0;
        Result rc = ao_append(b);
        if (R_SUCCEEDED(rc))
            g_queued[g_fill] = true;
        else
            log_error("audio: AppendAudioOutBuffer: 0x%x", rc);
        g_fill = -1;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Bemeneti formátumok (Android AudioFormat/AudioFormat.CHANNEL_*)

static int frame_bytes(int channels, int format) {
    int sample = format == 3 ? 1 : format == 4 ? 4 : 2;   // PCM_8BIT / PCM_FLOAT / PCM_16BIT
    return channels * sample;
}

static int channels_from_config(int cfg) {
    switch (cfg) {
        case 2:  // CHANNEL_CONFIGURATION_MONO (régi)
        case 4:  // CHANNEL_OUT_MONO
            return 1;
        case 3:  // CHANNEL_CONFIGURATION_STEREO (régi)
        case 12: // CHANNEL_OUT_STEREO
            return 2;
        default: {
            int n = __builtin_popcount((unsigned)cfg);
            return n >= 1 ? n : 2;
        }
    }
}

static int normalize_format(int fmt) {
    return (fmt == 3 || fmt == 4) ? fmt : 2;              // DEFAULT(1) és ismeretlen → 16 bit
}

static inline s16 sample_at(const u8 *p, int format) {
    if (format == 3)
        return (s16)(((int)p[0] - 128) << 8);
    if (format == 4) {
        float f;
        memcpy(&f, p, 4);
        if (f > 1.0f) f = 1.0f;
        if (f < -1.0f) f = -1.0f;
        return (s16)(f * 32767.0f);
    }
    s16 v;
    memcpy(&v, p, 2);   // a bájttömbön belüli eltolás páratlan is lehet
    return v;
}

static inline void decode_frame(const u8 *src, u32 i, int ch, int fmt, int fb, s16 out[2]) {
    const u8 *p = src + i * fb;
    out[0] = sample_at(p, fmt);
    out[1] = ch > 1 ? sample_at(p + fb / ch, fmt) : out[0];   // több csatornából az első kettő
}

static void resampler_reset(int in_rate) {
    g_step = ((u64)in_rate << 32) / OUT_RATE;
    g_pos = 0;
    g_prev[0] = g_prev[1] = 0;
}

// Lineáris interpoláció in_rate → 48 kHz; a hívások között folytonos (g_pos, g_prev).
// Hamis, ha leállítás miatt félbemaradt.
static bool resample_push(const u8 *src, u32 nframes, int ch, int fmt) {
    int fb = frame_bytes(ch, fmt);
    for (;;) {
        u32 i = (u32)(g_pos >> 32);
        if (i >= nframes)
            break;
        s16 a[2], b[2];
        if (i == 0) {
            a[0] = g_prev[0];
            a[1] = g_prev[1];
        } else {
            decode_frame(src, i - 1, ch, fmt, fb, a);
        }
        decode_frame(src, i, ch, fmt, fb, b);
        s32 f = (s32)((u32)g_pos >> 17);   // 15 bites tört: (b-a)*f nem csordul túl s32-ben
        s16 l = (s16)(a[0] + (((s32)b[0] - a[0]) * f >> 15));
        s16 r = (s16)(a[1] + (((s32)b[1] - a[1]) * f >> 15));
        if (!out_push(l, r))
            return false;
        g_pos += g_step;
    }
    g_pos -= (u64)nframes << 32;
    decode_frame(src, nframes - 1, ch, fmt, fb, g_prev);
    return true;
}

// ---------------------------------------------------------------------------
// A bevezető videó hangja (video.c): 16 bites sztereó PCM tetszőleges mintavétellel, ugyanazon a kimeneten.
// A videó a játék hangja előtt szól, így a resampler állapotát utána a játék rátájára állítjuk vissza.

static int g_pcm_rate;

bool audio_pcm_write(const int16_t *frames, uint32_t nframes, int rate) {
    if (!nframes)
        return true;
    mutexLock(&g_write_mutex);
    if (g_pcm_rate != rate) {
        resampler_reset(rate);
        g_pcm_rate = rate;
    }
    bool ok = g_ok && resample_push((const u8 *)frames, nframes, 2, 2);
    mutexUnlock(&g_write_mutex);
    if (!ok)
        svcSleepThread((s64)nframes * 1000000000LL / (rate > 0 ? rate : 48000));
    return ok;
}

void audio_pcm_end(void) {
    mutexLock(&g_write_mutex);
    mutexLock(&g_state_mutex);
    int rate = g_in_rate;
    mutexUnlock(&g_state_mutex);
    resampler_reset(rate);
    g_pcm_rate = 0;
    mutexUnlock(&g_write_mutex);
}

// ---------------------------------------------------------------------------
// Fókusz (HOME-menü, alvás): a write megáll, amíg az alkalmazás nincs előtérben.

static void wake_writers(void) {
    mutexLock(&g_state_mutex);
    condvarWakeAll(&g_state_cv);
    mutexUnlock(&g_state_mutex);
}

static void applet_hook_cb(AppletHookType hook, void *param) {
    (void)param;
    if (hook != AppletHookType_OnFocusState)
        return;
    bool paused = appletGetFocusState() != AppletFocusState_InFocus;
    mutexLock(&g_state_mutex);
    if (g_focus_paused != paused)
        log_info("audio: %s (fókusz)", paused ? "szünet" : "folytatás");
    g_focus_paused = paused;
    condvarWakeAll(&g_state_cv);
    mutexUnlock(&g_state_mutex);
}

void audio_set_suspended(bool suspended) {
    mutexLock(&g_state_mutex);
    g_manual_paused = suspended;
    condvarWakeAll(&g_state_cv);
    mutexUnlock(&g_state_mutex);
}

// ---------------------------------------------------------------------------

void audio_init(void) {
    mutexInit(&g_write_mutex);
    mutexInit(&g_state_mutex);
    condvarInit(&g_state_cv);
    g_shutdown = false;
    resampler_reset(g_in_rate);

    appletHook(&g_hook, applet_hook_cb, NULL);
    g_hooked = true;

    Result rc = audoutInitialize();
    if (R_FAILED(rc)) {
        log_error("audio: audoutInitialize: 0x%x – hang nélkül fut tovább", rc);
        return;
    }
    if (audoutGetSampleRate() != OUT_RATE || audoutGetChannelCount() != 2)
        log_error("audio: váratlan kimenet: %u Hz, %u csatorna", audoutGetSampleRate(), audoutGetChannelCount());

    rc = ao_register_event(&g_buf_event);
    if (R_FAILED(rc)) {
        log_error("audio: RegisterBufferEvent: 0x%x – hang nélkül fut tovább", rc);
        audoutExit();
        return;
    }

    for (int i = 0; i < OUT_BUFS; i++) {
        g_mem[i] = memalign(0x1000, OUT_BUF_SIZE);
        if (!g_mem[i]) {
            log_error("audio: nincs memória a kimeneti pufferekhez – hang nélkül fut tovább");
            for (int j = 0; j < i; j++) {
                free(g_mem[j]);
                g_mem[j] = NULL;
            }
            eventClose(&g_buf_event);
            audoutExit();
            return;
        }
        memset(g_mem[i], 0, OUT_BUF_SIZE);
        g_bufs[i] = (AoBuffer){
            .next = 0,
            .buffer = (u64)(uintptr_t)g_mem[i],
            .buffer_size = OUT_BUF_SIZE,
            .data_size = 0,
            .data_offset = 0,
        };
        g_queued[i] = false;
    }
    g_fill = -1;

    rc = audoutStartAudioOut();
    if (R_FAILED(rc)) {
        log_error("audio: audoutStartAudioOut: 0x%x – hang nélkül fut tovább", rc);
        for (int i = 0; i < OUT_BUFS; i++) {
            free(g_mem[i]);
            g_mem[i] = NULL;
        }
        eventClose(&g_buf_event);
        audoutExit();
        return;
    }
    g_ok = true;
    log_info("audio: audout kész (%u Hz, %d×%d keret)", audoutGetSampleRate(), OUT_BUFS, OUT_FRAMES);
}

void audio_exit(void) {
    if (g_hooked) {
        appletUnhook(&g_hook);
        g_hooked = false;
    }
    __atomic_store_n(&g_shutdown, true, __ATOMIC_RELEASE);
    wake_writers();

    // a write legfeljebb WAIT_SLICE_NS után kiengedi a zárat
    mutexLock(&g_write_mutex);
    if (g_ok) {
        g_ok = false;
        audoutStopAudioOut();
        eventClose(&g_buf_event);
        audoutExit();
        for (int i = 0; i < OUT_BUFS; i++) {
            free(g_mem[i]);
            g_mem[i] = NULL;
        }
    }
    mutexUnlock(&g_write_mutex);
}

// ---------------------------------------------------------------------------
// AudioTrack-metódusok (FalsoJNI)

jobject audioTrack_init(jmethodID id, va_list args) {
    int stream_type = va_arg(args, int);
    int rate = va_arg(args, int);
    int channel_cfg = va_arg(args, int);
    int format = va_arg(args, int);
    int buffer_size = va_arg(args, int);
    int mode = va_arg(args, int);
    log_info("AudioTrack(stream %d, %d Hz, csatorna 0x%x, formátum %d, puffer %d, mód %d)",
             stream_type, rate, channel_cfg, format, buffer_size, mode);
    if (mode != 1)
        log_error("audio: csak MODE_STREAM támogatott (mód %d)", mode);
    if (rate < 1000 || rate > 192000) {
        log_error("audio: furcsa mintavételi frekvencia (%d), 44100-zal megy", rate);
        rate = 44100;
    }

    mutexLock(&g_state_mutex);
    g_in_rate = rate;
    g_in_channels = channels_from_config(channel_cfg);
    g_in_format = normalize_format(format);
    g_track = TRACK_STOPPED;
    condvarWakeAll(&g_state_cv);
    mutexUnlock(&g_state_mutex);

    mutexLock(&g_write_mutex);
    resampler_reset(rate);
    mutexUnlock(&g_write_mutex);

    // A FalsoJNI DeleteGlobalRef free()-t hívna rá, ezért valódi foglalás (a tartalma nem számít).
    return (jobject)calloc(1, 16);
}

jint audioTrack_getMinBufferSize(jmethodID id, va_list args) {
    int rate = va_arg(args, int);
    int channel_cfg = va_arg(args, int);
    int format = va_arg(args, int);
    // A játék ebből számolja a write-darabot: min(méret/4, 1024) keret. 1024 keret 44,1 kHz-en 23 ms –
    // egy 48 kHz-es kimeneti pufferhez közeli darabok, kevés késleltetés.
    int size = 1024 * frame_bytes(channels_from_config(channel_cfg), normalize_format(format));
    log_info("AudioTrack.getMinBufferSize(%d Hz, 0x%x, %d) = %d", rate, channel_cfg, format, size);
    return size;
}

static void set_track_state(int state, const char *name) {
    log_info("AudioTrack.%s()", name);
    mutexLock(&g_state_mutex);
    g_track = state;
    condvarWakeAll(&g_state_cv);
    mutexUnlock(&g_state_mutex);
}

void audioTrack_play(jmethodID id, va_list args) {
    set_track_state(TRACK_PLAYING, "play");
}

void audioTrack_pause(jmethodID id, va_list args) {
    // a már kiküldött pufferek (legfeljebb ~85 ms) még lejátszódnak
    set_track_state(TRACK_PAUSED, "pause");
}

void audioTrack_stop(jmethodID id, va_list args) {
    // MODE_STREAM: a megírt adat még lejátszódik, utána csend – nincs teendő
    set_track_state(TRACK_STOPPED, "stop");
}

void audioTrack_release(jmethodID id, va_list args) {
    set_track_state(TRACK_NONE, "release");
    mutexLock(&g_write_mutex);
    resampler_reset(g_in_rate);
    mutexUnlock(&g_write_mutex);
}

jint audioTrack_write(jmethodID id, va_list args) {
    jbyteArray array = va_arg(args, jbyteArray);
    int offset = va_arg(args, int);
    int size = va_arg(args, int);

    JavaDynArray *jda = jda_find(array);
    if (!jda) {
        log_error("AudioTrack.write: a tömb nem FalsoJNI-tömb (%p)", array);
        return -3;   // AudioTrack.ERROR_INVALID_OPERATION
    }
    if (offset < 0 || size < 0 || offset > jda->len || size > jda->len - offset) {
        log_error("AudioTrack.write: rossz tartomány (offset %d, size %d, hossz %d)", offset, size, jda->len);
        return -2;   // AudioTrack.ERROR_BAD_VALUE
    }

    // Szünet alatt (játék vagy fókusz) blokkolunk, mint Androidon a teli puffer.
    mutexLock(&g_state_mutex);
    while (!g_shutdown && (g_track == TRACK_PAUSED || g_focus_paused || g_manual_paused))
        condvarWait(&g_state_cv, &g_state_mutex);
    int rate = g_in_rate, ch = g_in_channels, fmt = g_in_format;
    mutexUnlock(&g_state_mutex);

    int fb = frame_bytes(ch, fmt);
    u32 nframes = (u32)(size / fb);
    if (nframes == 0)
        return 0;
    const u8 *src = (const u8 *)jda->array + offset;

    mutexLock(&g_write_mutex);
    bool played = g_ok && resample_push(src, nframes, ch, fmt);
    mutexUnlock(&g_write_mutex);

    // Hang nélkül (vagy leállításkor) az adat idejét kivárjuk, hogy a játék hangszála ne pörögjön.
    if (!played)
        svcSleepThread((s64)nframes * 1000000000LL / rate);

    return (jint)(nframes * fb);
}
