/*
 * data_install.c – a binárisba ágyazott játékfájlok
 *
 * - A backstab-vita data/control0 fájlját (a kontrollerre szabott billentyűséma) induláskor kiírjuk a
 *   GAME_FILES-ba, ha hiányzik. Meglévő fájlt nem írunk felül: a játék is mentheti.
 *   (A Vita javított nyitóképei – Gameloft-grafika – nem kellenek, lásd patch.c.)
 * - Az eredeti betűkészlet 1080p-re nagyítva (hd_eu_font_*): a make gyártja a cache-beli Gameloft-
 *   csomagból (tools/assets/make-fonts.py → build/hdfonts/, HD_FONTS), a játék a patch.c-n át memóriafájlként
 *   kapja (embedded_file), az SD-re nem kerül ki.
 *
 * Az .incbin útvonala a fordítás munkakönyvtárához (build/, lásd Makefile: make -C build)
 * képest relatív, ezért „../data/…”, illetve „hdfonts/…”.
 *
 * Copyright (C) 2022-2023 Volodymyr Atamanenko (a fájlok), (C) 2026 Levi (Switch port)
 * MIT license, lásd LICENSE.
 */
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>

#include "port.h"
#include "util.h"

#define BLOB(name, file)                                  \
    __asm__(".section .rodata." #name ",\"a\"\n"          \
            ".balign 4\n"                                 \
            ".global " #name "\n"                         \
            #name ":\n"                                   \
            ".incbin \"" file "\"\n"                      \
            ".global " #name "_end\n"                     \
            #name "_end:\n"                               \
            ".previous\n");                               \
    extern const unsigned char name[], name##_end[];

BLOB(blob_control0, "../data/control0")
#ifdef HD_FONTS
BLOB(blob_font_small_bs, "hdfonts/hd_eu_font_small.bsprite")
BLOB(blob_font_small_tex, "hdfonts/hd_eu_font_small.pvr")
BLOB(blob_font_medium_bs, "hdfonts/hd_eu_font_medium.bsprite")
BLOB(blob_font_medium_tex, "hdfonts/hd_eu_font_medium.pvr")
BLOB(blob_font_big_bs, "hdfonts/hd_eu_font_big.bsprite")
BLOB(blob_font_big_tex, "hdfonts/hd_eu_font_big.pvr")
BLOB(blob_font_map, "hdfonts/hd_eu_font_map.bin")
#endif

static const struct {
    const char *name;
    const unsigned char *start, *end;
} files[] = {
    { "control0",                   blob_control0,        blob_control0_end },
};

void data_install(void) {
    char path[256];
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        size_t size = (size_t)(files[i].end - files[i].start);
        snprintf(path, sizeof(path), GAME_FILES "%s", files[i].name);

        struct stat st;
        if (stat(path, &st) == 0 && st.st_size > 0)
            continue;

        mkpath(path);
        FILE *f = fopen(path, "wb");
        if (!f || fwrite(files[i].start, 1, size, f) != size) {
            if (f) fclose(f);
            remove(path);   // félig írt fájl ne maradjon: a következő indításkor újra próbáljuk
            fatal_error("Nem sikerült kiírni: %s", path);
        }
        fclose(f);
        log_info("data_install: %s kiírva (%u bájt)", files[i].name, (unsigned)size);
    }
}

static const struct {
    const char *name;
    const unsigned char *start, *end;
} embedded[] = {
#ifdef HD_FONTS
    { "hd_eu_font_small.bsprite",   blob_font_small_bs,   blob_font_small_bs_end },
    { "hd_eu_font_small.pvr",       blob_font_small_tex,  blob_font_small_tex_end },
    { "hd_eu_font_medium.bsprite",  blob_font_medium_bs,  blob_font_medium_bs_end },
    { "hd_eu_font_medium.pvr",      blob_font_medium_tex, blob_font_medium_tex_end },
    { "hd_eu_font_big.bsprite",     blob_font_big_bs,     blob_font_big_bs_end },
    { "hd_eu_font_big.pvr",         blob_font_big_tex,    blob_font_big_tex_end },
    { "hd_eu_font_map.bin",         blob_font_map,        blob_font_map_end },
#endif
    { NULL, NULL, NULL },
};

const void *embedded_file(const char *name, int *size) {
    for (size_t i = 0; embedded[i].name; i++) {
        if (strcmp(embedded[i].name, name) == 0) {
            *size = (int)(embedded[i].end - embedded[i].start);
            return embedded[i].start;
        }
    }
    return NULL;
}
