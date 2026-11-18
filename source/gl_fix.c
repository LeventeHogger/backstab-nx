/*
 * gl_fix.c – a játék GLSL-forrásainak javítása a Mesa szabványos fordítójához
 *
 * Az Android-meghajtók elnéztek olyan hibákat, amelyeket a Mesa (helyesen) elutasít; a le nem forduló
 * shaderrel az anyag egyszerűen nem rajzolódik ki. Ismert esetek:
 *  - UnlitTexturedFP.glsl (glitch): TEXTURE_TYPE == TEXTURE_CUBE_MAP-ra samplerCube-ot deklarál, de a main()
 *    csak a 3D-s ágat kezeli, a többi texture2D(samplerCube, vec3) – „no matching function for call to
 *    texture2D(samplerCube, vec3)”. Ide a hiányzó textureCube-ágat szúrjuk be (a ForceCommitTextureFS.glsl
 *    mintájára, ami helyesen kezeli). Az eredeti Gameloft-csomag változatában ez az ág megvan.
 *  - UnlitTexturedFP.glsl az eredeti Gameloft-csomagban: „#endif]” – a Mesa előfeldolgozója elutasítja („unexpected
 *    ']'”); az #endif utáni szemetet szóközre cseréljük.
 *
 * Copyright (C) 2026 Levi (Switch port), MIT license.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <GLES2/gl2.h>

#include "util.h"

// az #endif utáni, sorvégig tartó szemét (megjegyzés kivételével) szóközre; igaz, ha változott
static bool fix_endif_junk(char *src) {
    bool changed = false;
    for (char *p = strstr(src, "#endif"); p; p = strstr(p, "#endif")) {
        p += 6;
        for (char *q = p; *q && *q != '\n' && *q != '\r'; q++) {
            if (q[0] == '/' && (q[1] == '/' || q[1] == '*'))
                break;
            if (*q != ' ' && *q != '\t') {
                *q = ' ';
                changed = true;
            }
        }
    }
    return changed;
}

static const char *const cube_line = "gl_FragColor = texture2D(texture, vCoord0) * vColor0;";

// új (malloc-olt) forrás, ha javítani kellett; különben NULL
static char *fix_cube_branch(const char *src) {
    if (!strstr(src, "samplerCube texture") || strstr(src, "textureCube(texture"))
        return NULL;
    const char *line = strstr(src, cube_line);
    if (!line)
        return NULL;
    // a sor előtti #else
    const char *els = NULL;
    for (const char *p = line; p > src + 4; p--) {
        if (!strncmp(p - 5, "#else", 5)) {
            els = p - 5;
            break;
        }
    }
    if (!els)
        return NULL;
    static const char *const ins =
        "#elif TEXTURE_TYPE == TEXTURE_CUBE_MAP\n"
        "    gl_FragColor = textureCube(texture, vCoord0) * vColor0;\n";
    size_t n = strlen(src), k = strlen(ins);
    char *out = malloc(n + k + 1);
    if (!out)
        return NULL;
    size_t head = (size_t)(els - src);
    memcpy(out, src, head);
    memcpy(out + head, ins, k);
    memcpy(out + head + k, els, n - head + 1);
    return out;
}

void fix_glShaderSource(GLuint sh, GLsizei count, const GLchar *const *str, const GLint *len) {
    // a darabokat összefűzzük (a glitch a #define-okat külön darabban adja)
    size_t total = 1;
    for (GLsizei i = 0; i < count; i++)
        total += len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
    char *src = malloc(total);
    if (!src) {
        glShaderSource(sh, count, str, len);
        return;
    }
    size_t at = 0;
    for (GLsizei i = 0; i < count; i++) {
        size_t l = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
        memcpy(src + at, str[i], l);
        at += l;
    }
    src[at] = 0;

    bool endif = fix_endif_junk(src);
    if (endif) {
        static int logged;
        if (!logged++)
            log_info("gl_fix: szemét az #endif után (UnlitTexturedFP) – kivéve");
    }
    char *fixed = fix_cube_branch(src);
    if (fixed) {
        static int logged;
        if (!logged++)
            log_info("gl_fix: UnlitTexturedFP kockatérképes ága javítva (textureCube)");
        const GLchar *one = fixed;
        glShaderSource(sh, 1, &one, NULL);
        free(fixed);
    } else if (endif) {
        const GLchar *one = src;
        glShaderSource(sh, 1, &one, NULL);
    } else {
        glShaderSource(sh, count, str, len);
    }
    free(src);
}
