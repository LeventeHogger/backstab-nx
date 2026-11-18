#!/usr/bin/env python3
"""Switch-gombjelek rajzolása a játék PlayStation-stílusú (Xperia Play) jelei helyére → source/glyphs_data.h

A modulok helye és mérete az interface.bsprite modultáblájából (352 modul a 0x170-es eltolástól,
x/y/w/h tömbökként); a glyphs.c a játék interface textúrájának feltöltésekor ezekre a helyekre másolja
a képeket. Saját vonalrajz (nem betűtípus), így szabadon a repóban tartható.

Futtatás: ~/Library/Caches/backstab-nx-toolchain/venv-tools/bin/python tools/assets/make-glyphs.py [előnézet.png]
"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw

SS = 16                                  # túlmintavételezés
INK = (44, 44, 44, 255)                  # sötét jel, ahogy a világos kerek gombon a PS-jelek is
HALO = (235, 235, 235, 255)              # világos körvonal, hogy minden háttéren olvasható legyen

# modul: (x, y, w, h, jel)  – a játék modulsorszámával
GLYPHS = {
    250: (1684, 265, 16, 17, "B"),       # ×  – ugrás          → B (lent)
    251: (1700, 265, 17, 17, "Y"),       # □  – kard           → Y (balra)
    252: (1100, 598, 19, 17, "X"),       # △  – interakció     → X (fent)
    253: (1878, 523, 19, 18, "A"),       # ○  – kontra         → A (jobbra)
    349: (178, 601, 13, 12, "X"),        # kis △
    311: (191, 601, 13, 12, "A"),        # kis ○
    256: (262, 601, 37, 10, "+"),        # START  → +
    255: (457, 608, 46, 10, "-"),        # SELECT → −
}


def strokes(ch, w, h):
    """A jel vonalai a (0..1)² négyzetben: (pontok, zárt?) listák és ívek."""
    if ch == "X":
        return [[(0.18, 0.12), (0.82, 0.88)], [(0.82, 0.12), (0.18, 0.88)]]
    if ch == "Y":
        return [[(0.18, 0.12), (0.5, 0.52), (0.82, 0.12)], [(0.5, 0.52), (0.5, 0.9)]]
    if ch == "A":
        return [[(0.16, 0.9), (0.5, 0.1), (0.84, 0.9)], [(0.3, 0.62), (0.7, 0.62)]]
    if ch == "B":
        return [[(0.24, 0.1), (0.24, 0.9)],
                [(0.24, 0.1), (0.56, 0.1), "arc", (0.56, 0.3, 0.2), (0.56, 0.5), (0.24, 0.5)],
                [(0.24, 0.5), (0.6, 0.5), "arc", (0.6, 0.7, 0.2), (0.6, 0.9), (0.24, 0.9)]]
    if ch == "+":
        return [[(0.5 - 0.11 * h / w * 3, 0.5), (0.5 + 0.11 * h / w * 3, 0.5)], [(0.5, 0.05), (0.5, 0.95)]]
    if ch == "-":
        return [[(0.5 - 0.11 * h / w * 3, 0.5), (0.5 + 0.11 * h / w * 3, 0.5)]]
    raise ValueError(ch)


def render(ch, w, h):
    W, H = w * SS, h * SS
    side = min(W, H)                       # a betűk négyzetben, középre; a +/− a teljes magasságban
    ox, oy = (W - side) / 2, (H - side) / 2
    sx = W if ch in "+-" else side
    if ch in "+-":
        ox = 0

    def P(p):
        return (ox + p[0] * sx, oy + p[1] * side)

    def path(seg):
        pts = []
        i = 0
        while i < len(seg):
            item = seg[i]
            if item == "arc":              # félkör a (cx, cy, r) körül, jobbra domborodva
                cx, cy, r = seg[i + 1]
                import math
                for k in range(0, 33):
                    a = -math.pi / 2 + math.pi * k / 32
                    pts.append(P((cx + r * math.cos(a), cy + r * math.sin(a))))
                i += 2
                continue
            pts.append(P(item))
            i += 1
        return pts

    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    stroke = side * (0.17 if min(w, h) >= 15 else 0.2)
    if ch in "+-":
        stroke = H * 0.3
    for color, width in ((HALO, stroke * 1.9), (INK, stroke)):
        for seg in strokes(ch, w, h):
            pts = path(seg)
            d.line(pts, fill=color, width=max(1, int(width)), joint="curve")
            for p in (pts[0], pts[-1]):    # kerek végek
                r = width / 2
                d.ellipse((p[0] - r, p[1] - r, p[0] + r, p[1] + r), fill=color)
    return img.resize((w, h), Image.LANCZOS)


def main():
    root = Path(__file__).resolve().parents[2]
    out = root / "source" / "glyphs_data.h"
    lines = ["// Generálva: tools/assets/make-glyphs.py – ne szerkeszd kézzel.", "#pragma once", "#include <stdint.h>", "",
             "typedef struct { uint16_t module, x, y, w, h; const uint8_t *rgba; } Glyph;", ""]
    previews = []
    for mod, (x, y, w, h, ch) in GLYPHS.items():
        img = render(ch, w, h)
        previews.append(img)
        data = img.tobytes()
        lines.append(f"static const uint8_t glyph_{mod}[{len(data)}] = {{")
        for i in range(0, len(data), 24):
            lines.append("    " + ", ".join(str(b) for b in data[i:i + 24]) + ",")
        lines.append("};")
    lines.append("")
    lines.append("static const Glyph glyphs[] = {")
    for mod, (x, y, w, h, ch) in GLYPHS.items():
        lines.append(f"    {{ {mod}, {x}, {y}, {w}, {h}, glyph_{mod} }},   // {ch}")
    lines.append("};")
    out.write_text("\n".join(lines) + "\n")
    print("kész:", out)

    if len(sys.argv) > 1:                  # előnézet: 8× nagyítva, világos gombon és sötét háttéren
        tiles = []
        for img in previews:
            big = img.resize((img.width * 8, img.height * 8), Image.NEAREST)
            for bgc in ((230, 230, 230, 255), (40, 40, 40, 255)):
                bg = Image.new("RGBA", big.size, bgc)
                bg.alpha_composite(big)
                tiles.append(bg)
        sheet = Image.new("RGB", (sum(t.width for t in tiles) + 6 * len(tiles), max(t.height for t in tiles)), "white")
        x = 0
        for t in tiles:
            sheet.paste(t, (x, 0))
            x += t.width + 6
        sheet.save(sys.argv[1])


if __name__ == "__main__":
    main()
