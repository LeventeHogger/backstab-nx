#!/usr/bin/env python3
"""Az eredeti (Gameloft) latin betűkészlet 1080p-re nagyítva → hd_eu_font_*.{bsprite,pvr}, hd_eu_font_map.bin

Miért: a 1920×1080-as sprite-csomag (sprites_1920x1080.gla) nem része a Gameloft eredeti csomagjának, és
annak eu_font_* fontjai nem az eredetiek (utólag újragyártottak): cirill betűk vannak benne, a nyugati ékezetesek (à é ß …)
hiányoznak, és a méretarányok sem az eredetiek (a „small” nagyobb a „medium”-nál). Az eredeti fontok a
Gameloft letöltőszerveréről származó 1280×800-as csomagban vannak (sprites_1280x800.gla, DXT5 .dds).
A játék a fontot nem skálázza (NoScale modulméretek, a sorköz is a modulmagasságból jön), ezért az
1280×800-as képernyőhöz képest 1080/800 = 1,35-szörös modulokkal ugyanazt az arányt kapjuk, mint az
eredetiben. A make futtatja (Makefile: FONT_PACK), a kimenetet a binárisba ágyazza (data_install.c), és a
patch.c a játék eu_font_* fájljai helyett ezeket adja a játék fájlrendszerének.

A jeleket egyenként nagyítjuk (premultiplikált alfával, Lanczos), és 2 px réssel új atlaszba rakjuk, hogy
a bilineáris szűrés ne húzzon át a szomszédból; a .bsprite-ban csak a modulok x/y/w/h-ja változik
(a font fmodul-eltolásai nullák, a két furcsa érték – 'p', 'q' – nem képpont, változatlan marad).

Kézi futtatás (a kimenet a build/-be vagy a cache-be kerül, a repóba soha):
  ~/Library/Caches/backstab-nx-toolchain/venv-tools/bin/python tools/assets/make-fonts.py <sprites_1280x800.gla> <kimeneti mappa>
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gla  # noqa: E402

SCALE = 1080 / 800
PAD = 2
FONTS = ("eu_font_small", "eu_font_medium", "eu_font_big")
PREFIX = "hd_"


def dds_rgba(blob):
    """DXT1/DXT5 .dds → PIL RGBA kép."""
    import texture2ddecoder
    from PIL import Image
    if blob[:4] != b"DDS ":
        sys.exit("nem DDS")
    h, w = struct.unpack_from("<II", blob, 12)
    fourcc = blob[84:88]
    data = blob[128:]
    if fourcc == b"DXT5":
        raw = texture2ddecoder.decode_bc3(data, w, h)
    elif fourcc == b"DXT1":
        raw = texture2ddecoder.decode_bc1(data, w, h)
    else:
        sys.exit(f"ismeretlen DDS-formátum: {fourcc}")
    return Image.frombytes("RGBA", (w, h), raw, "raw", "BGRA")


def scaled_glyph(img, box, size):
    """Egy jel nagyítása premultiplikált alfával (különben a átlátszó szélek sötét szegélyt adnak)."""
    from PIL import Image
    g = img.crop(box)
    if size[0] <= 0 or size[1] <= 0:
        return Image.new("RGBA", (max(size[0], 0), max(size[1], 0)))
    pre = g.convert("RGBa").resize(size, Image.LANCZOS)
    return pre.convert("RGBA")


def pot(n):
    p = 1
    while p < n:
        p *= 2
    return p


def build_font(bsprite, dds):
    from PIL import Image
    nm = struct.unpack_from("<8H", bsprite, 0)[2]
    base = 0x10 + nm
    xs, ys, ws, hs = [list(struct.unpack_from("<%dH" % nm, bsprite, base + k * nm * 2)) for k in range(4)]
    src = dds_rgba(dds)

    sizes = [(round(w * SCALE), round(h * SCALE)) for w, h in zip(ws, hs)]
    # polcos elhelyezés magasság szerint csökkenő sorrendben, 1024 széles atlaszba
    width = 1024
    order = sorted(range(nm), key=lambda i: -sizes[i][1])
    pos = [None] * nm
    x = y = PAD
    shelf = 0
    for i in order:
        w, h = sizes[i]
        if x + w + PAD > width:
            x, y = PAD, y + shelf + PAD
            shelf = 0
        pos[i] = (x, y)
        x += w + PAD
        shelf = max(shelf, h)
    height = pot(y + shelf + PAD)
    atlas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    for i in range(nm):
        if sizes[i][0] and sizes[i][1]:
            g = scaled_glyph(src, (xs[i], ys[i], xs[i] + ws[i], ys[i] + hs[i]), sizes[i])
            atlas.paste(g, pos[i])

    out = bytearray(bsprite)
    new = ([p[0] for p in pos], [p[1] for p in pos], [s[0] for s in sizes], [s[1] for s in sizes])
    for k, arr in enumerate(new):
        struct.pack_into("<%dH" % nm, out, base + k * nm * 2, *arr)
    return bytes(out), atlas


def pvr_rgba8888(img):
    """PVR v2, RGBA8888, mipmap nélkül (a játék saját RGBA8888 .pvr-jeinek fejlécével)."""
    w, h = img.size
    data = img.tobytes()
    hdr = struct.pack("<13I", 52, h, w, 0, 0x8012, len(data), 32,
                      0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000, 0x21525650, 1)
    return hdr + data


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    pack, outdir = sys.argv[1], Path(sys.argv[2]).expanduser()
    e = gla.entries(pack)
    outdir.mkdir(parents=True, exist_ok=True)
    for f in FONTS:
        bs, atlas = build_font(e[f + ".bsprite"], e[f + ".dds"])
        (outdir / f"{PREFIX}{f}.bsprite").write_bytes(bs)
        (outdir / f"{PREFIX}{f}.pvr").write_bytes(pvr_rgba8888(atlas))
        print(f"{PREFIX}{f}: {atlas.size[0]}×{atlas.size[1]}")
    (outdir / f"{PREFIX}eu_font_map.bin").write_bytes(e["eu_font_map.bin"])
    print(f"kész → {outdir}")


if __name__ == "__main__":
    main()
