/*
 * port.h – a Switch-port saját kiegészítései (a game.h modulfelületén kívül)
 *
 * Copyright (C) 2026 Levi (Switch port), MIT license.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

// data_install.c – a Vita-port control0 billentyűsémáját (data/) a binárisba ágyazva hozza;
// ha hiányzik a GAME_FILES-ból, kiírja.
// main.c hívja, a játék indítása (so_file_load) előtt.
void data_install(void);
// data_install.c – a binárisba ágyazott, csak olvasott fájl (pl. „hd_eu_font_small.pvr”, ha a build
// tartalmazza: HD_FONTS); NULL, ha nincs.
const void *embedded_file(const char *name, int *size);

// controls.c – szabad menükurzor (tartalék: ahol nincs fókuszálható gomb, vagy R3-ra): a bal kar
// mozgatja, az A „koppint”. Igazat ad, ha a kurzort ki kell rajzolni; x, y képernyő-koordináta.
bool controls_cursor(int *x, int *y);
// controls.c – menüben a fókuszban lévő gomb téglalapja (képernyő-koordináta), a kiemelő kerethez.
bool controls_focus_rect(int *x, int *y, int *w, int *h);
// a patch.c gomb-Draw-hookjai hívják: ezt a gombot a játék most kirajzolta (menü-navigáció)
void controls_note_drawn(const void *obj);
// heap.c – a nagy foglalások régiója (a newlib heap tetején, lefelé nő): teljes méret és foglalt rész, MB
void heap_stats(unsigned *region_mb, unsigned *used_mb);
// audio.c – a bevezető videó hangja: 16 bites sztereó PCM (blokkol, amíg a kimenet be nem fogadja); a végén
// audio_pcm_end() állítja vissza a játék hangjának resamplerét
bool audio_pcm_write(const int16_t *frames, uint32_t nframes, int rate);
void audio_pcm_end(void);
// video.c – a bevezető videó (files/intro.mpg, ha van) az első töltés előtt; gl_init és controls_init után
void video_play_intro(void);
#ifdef DEBUG
// gl.c – csak debug: a következő képkockát a csere előtt kimenti: DATA_PATH "shots/<név>.rgba"
// (1920×1080 RGBA8, fejléc nélkül, alulról felfelé; a gépen: tools/dev/shot2png.py)
void gl_debug_shot(const char *name);
#endif
