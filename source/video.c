/*
 * video.c – a bevezető videó az első töltés előtt
 *
 * Androidon a Java-indító (MyVideoView) játszotta le a files/intro.mp4-et (4:3 kijelzőn az intro_4_3.mp4-et),
 * mielőtt a játék motorja elindult; koppintással átugorható volt. A natív kód nem tud róla.
 * Itt: a csomagkészítő (tools/assets/make-intro.sh) a játék saját intro.mp4-éből MPEG-1-et gyárt (files/intro.mpg,
 * a kép változatlan felbontású, a hang 48 kHz), ezt a pl_mpeg (MIT, lib/pl_mpeg) dekódolja.
 *  - kép: Y/Cb/Cr textúrák, YCbCr → RGB (BT.601) a shaderben, képarány-helyes kitöltés;
 *  - hang: a dekóder ad 32 bites lebegőpontos sztereót → gyűrűs puffer → saját szál → audio.c (audout);
 *  - időzítés: a dekódert a falióra hajtja (plm_decode a két képkocka közti idővel);
 *  - átugrás: A, B, +, − vagy koppintás. Ha nincs intro.mpg, nincs videó.
 *
 * Copyright (C) 2026 Levi (Switch port), MIT license.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <GLES2/gl2.h>

#define PL_MPEG_IMPLEMENTATION
#include <pl_mpeg/pl_mpeg.h>

#include "game.h"
#include "port.h"
#include "util.h"

#define INTRO_PATH GAME_FILES "intro.mpg"

// --- hang: gyűrűs puffer (16 bites sztereó keretek) és a kiíró szál

#define RING_FRAMES (48000 / 2)   // fél másodperc

static s16 ring[RING_FRAMES * 2];
static u32 ring_r, ring_w;        // keretindexek (szabadon futnak, a különbségük a töltöttség)
static Mutex ring_mutex;
static CondVar ring_cv;
static bool ring_done;
static int ring_rate;
static Thread audio_thread;

static void audio_thread_fn(void *arg) {
    (void)arg;
    s16 chunk[1024 * 2];
    for (;;) {
        mutexLock(&ring_mutex);
        while (ring_w == ring_r && !ring_done)
            condvarWait(&ring_cv, &ring_mutex);
        if (ring_w == ring_r && ring_done) {
            mutexUnlock(&ring_mutex);
            break;
        }
        u32 n = ring_w - ring_r;
        if (n > 1024)
            n = 1024;
        for (u32 i = 0; i < n; i++) {
            u32 k = (ring_r + i) % RING_FRAMES;
            chunk[i * 2] = ring[k * 2];
            chunk[i * 2 + 1] = ring[k * 2 + 1];
        }
        ring_r += n;
        condvarWakeAll(&ring_cv);
        mutexUnlock(&ring_mutex);
        audio_pcm_write(chunk, n, ring_rate);   // blokkol, amíg a kimenet be nem fogadja
    }
}

static inline s16 to_s16(float f) {
    if (f > 1.0f) f = 1.0f;
    if (f < -1.0f) f = -1.0f;
    return (s16)(f * 32767.0f);
}

static void on_audio(plm_t *plm, plm_samples_t *s, void *user) {
    (void)plm;
    (void)user;
    mutexLock(&ring_mutex);
    for (unsigned i = 0; i < s->count; i++) {
        // ha a kimenet lemaradt, megvárjuk (a videó ilyenkor sem fut el a hang elől)
        while (ring_w - ring_r >= RING_FRAMES && !ring_done)
            condvarWait(&ring_cv, &ring_mutex);
        u32 k = ring_w % RING_FRAMES;
        ring[k * 2] = to_s16(s->interleaved[i * 2]);
        ring[k * 2 + 1] = to_s16(s->interleaved[i * 2 + 1]);
        ring_w++;
    }
    condvarWakeAll(&ring_cv);
    mutexUnlock(&ring_mutex);
}

// --- kép

static const char *vs_src =
    "attribute vec2 pos;\n"
    "attribute vec2 tc;\n"
    "varying vec2 uv;\n"
    "void main() { uv = tc; gl_Position = vec4(pos, 0.0, 1.0); }\n";

static const char *fs_src =
    "precision mediump float;\n"
    "uniform sampler2D ty, tcb, tcr;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "    float y = (texture2D(ty, uv).r - 0.0625) * 1.164;\n"
    "    float cb = texture2D(tcb, uv).r - 0.5;\n"
    "    float cr = texture2D(tcr, uv).r - 0.5;\n"
    "    gl_FragColor = vec4(y + 1.596 * cr, y - 0.391 * cb - 0.813 * cr, y + 2.018 * cb, 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char info[512] = "";
        glGetShaderInfoLog(s, sizeof(info), NULL, info);
        log_error("video: shader: %s", info);
    }
    return s;
}

typedef struct {
    GLuint tex[3];          // Y, Cb, Cr
    unsigned w[3], h[3];    // a síkok mérete (makroblokkra kerekítve)
    bool have;              // érkezett már képkocka
    float u, v;             // a megjelenítendő rész a síkból (a kerekítés levágva)
} Frames;

static void on_video(plm_t *plm, plm_frame_t *f, void *user) {
    (void)plm;
    Frames *fr = user;
    plm_plane_t *pl[3] = { &f->y, &f->cb, &f->cr };
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_2D, fr->tex[i]);
        if (fr->w[i] != pl[i]->width || fr->h[i] != pl[i]->height) {
            fr->w[i] = pl[i]->width;
            fr->h[i] = pl[i]->height;
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, fr->w[i], fr->h[i], 0, GL_LUMINANCE, GL_UNSIGNED_BYTE,
                         pl[i]->data);
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fr->w[i], fr->h[i], GL_LUMINANCE, GL_UNSIGNED_BYTE,
                            pl[i]->data);
        }
    }
    fr->u = (float)f->width / (float)f->y.width;
    fr->v = (float)f->height / (float)f->y.height;
    fr->have = true;
}

static bool skip_pressed(PadState *pad) {
    padUpdate(pad);
    u64 down = padGetButtonsDown(pad);
    if (down & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus | HidNpadButton_Minus))
        return true;
    HidTouchScreenState ts = { 0 };
    static bool was_touching = true;   // az indításkor már a képernyőn lévő ujj ne ugorjon át
    bool touching = hidGetTouchScreenStates(&ts, 1) && ts.count > 0;
    bool tap = touching && !was_touching;
    was_touching = touching;
    return tap;
}

void video_play_intro(void) {
    if (!file_exists(INTRO_PATH))
        return;
    plm_t *plm = plm_create_with_filename(INTRO_PATH);
    if (!plm || !plm_probe(plm, 5000 * 1024)) {
        log_error("video: %s nem olvasható MPEG-1 (lásd tools/assets/make-intro.sh)", INTRO_PATH);
        if (plm)
            plm_destroy(plm);
        return;
    }
    int vw = plm_get_width(plm), vh = plm_get_height(plm);
    ring_rate = plm_get_samplerate(plm);
    log_info("video: bevezető %dx%d, %.1f fps, %d Hz, %.1f mp", vw, vh, plm_get_framerate(plm), ring_rate,
             plm_get_duration(plm));

    // GL
    GLuint prog = glCreateProgram();
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src), fs = compile(GL_FRAGMENT_SHADER, fs_src);
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "pos");
    glBindAttribLocation(prog, 1, "tc");
    glLinkProgram(prog);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "ty"), 0);
    glUniform1i(glGetUniformLocation(prog, "tcb"), 1);
    glUniform1i(glGetUniformLocation(prog, "tcr"), 2);

    Frames fr = { 0 };
    glGenTextures(3, fr.tex);
    for (int i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_2D, fr.tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    // képarány-helyes kitöltés (a 854×480 gyakorlatilag 16:9)
    float sx = 1.0f, sy = 1.0f;
    float va = (float)vw / (float)vh, sa = (float)screen_width / (float)screen_height;
    if (va > sa)
        sy = sa / va;
    else
        sx = va / sa;

    // hang
    ring_r = ring_w = 0;
    ring_done = false;
    mutexInit(&ring_mutex);
    condvarInit(&ring_cv);
    bool audio_ok = ring_rate > 0 &&
                    R_SUCCEEDED(threadCreate(&audio_thread, audio_thread_fn, NULL, NULL, 0x4000, 0x2C, -2)) &&
                    R_SUCCEEDED(threadStart(&audio_thread));
    plm_set_audio_enabled(plm, audio_ok);
    plm_set_audio_lead_time(plm, 0.1);
    plm_set_video_decode_callback(plm, on_video, &fr);
    if (audio_ok)
        plm_set_audio_decode_callback(plm, on_audio, NULL);

    PadState pad;
    padInitializeDefault(&pad);
    padUpdate(&pad);

    uint64_t last = time_ms();
    bool skipped = false;
    while (appletMainLoop() && !plm_has_ended(plm)) {
        uint64_t now = time_ms();
        double dt = (now - last) / 1000.0;
        last = now;
        if (dt > 0.1)
            dt = 0.1;   // betöltés/fókuszvesztés után ne ugorjon
        plm_decode(plm, dt);

        glViewport(0, 0, screen_width, screen_height);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        if (fr.have) {
            const float q[] = {
                -sx, -sy, 0.0f, fr.v,   sx, -sy, fr.u, fr.v,
                -sx,  sy, 0.0f, 0.0f,   sx,  sy, fr.u, 0.0f,
            };
            glUseProgram(prog);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, q);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, q + 2);
            for (int i = 0; i < 3; i++) {
                glActiveTexture(GL_TEXTURE0 + i);
                glBindTexture(GL_TEXTURE_2D, fr.tex[i]);
            }
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glActiveTexture(GL_TEXTURE0);
        }
        gl_swap();

        if (skip_pressed(&pad)) {
            skipped = true;
            break;
        }
    }
    log_info("video: bevezető vége%s", skipped ? " (átugorva)" : "");

    // takarítás: a hangszál a maradékot még kiírja (átugrásnál eldobjuk)
    mutexLock(&ring_mutex);
    if (skipped)
        ring_r = ring_w;
    ring_done = true;
    condvarWakeAll(&ring_cv);
    mutexUnlock(&ring_mutex);
    if (audio_ok) {
        threadWaitForExit(&audio_thread);
        threadClose(&audio_thread);
    }
    audio_pcm_end();
    plm_destroy(plm);

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glUseProgram(0);
    glDeleteTextures(3, fr.tex);
    glDeleteProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
}
