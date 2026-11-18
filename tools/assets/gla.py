#!/usr/bin/env python3
"""A játék .gla archívumainak kezelése (a játékfájlok sosem kerülnek a repóba – a kimenet a cache-be menjen).

  gla.py list <archív.gla> [szűrő]                 – bejegyzések (név, méret)
  gla.py extract <archív.gla> <név> <kimenet>       – egy bejegyzés kiírása
  gla.py png <archív.gla> <név.pvr> <kimenet.png>   – PVR v2 textúra → PNG (PVRTC2/4, ETC1, RGBA8888)
  gla.py modules <archív.gla> <név.bsprite>         – BSprite modultábla (sorszám, x, y, w, h)

GLA: big-endian fejléc (+8: névtábla eltolása, +12: darabszám), 0x10-től 16 bájtos bejegyzések
(eltolás, méret, névtábla-beli eltolás, 0). BSprite: u16 fejléc (+4: modulok száma), 0x10-től modulonként
egy típusbájt, utána x/y/w/h u16-tömbök (lásd docs/visszafejtes.md). A png/modules parancshoz Pillow és
texture2ddecoder kell (python3 -m venv … && pip install pillow texture2ddecoder).
"""
import struct
import sys


def entries(path):
    d = open(path, "rb").read()
    nt, cnt = struct.unpack_from(">II", d, 8)
    out = {}
    for i in range(cnt):
        off, size, no, _ = struct.unpack_from(">IIII", d, 0x10 + i * 16)
        out[d[nt + no:d.index(b"\0", nt + no)].decode(errors="replace")] = d[off:off + size]
    return out


def pvr_to_png(blob, out):
    import texture2ddecoder
    from PIL import Image
    hsize, h, w, mips, flags = struct.unpack_from("<5I", blob, 0)
    fmt, data = flags & 0xFF, blob[hsize:]
    if fmt in (0x19, 0x0D, 0x18, 0x0C):
        rgba = texture2ddecoder.decode_pvrtc(data, w, h, fmt in (0x18, 0x0C))
    elif fmt == 0x36:
        rgba = texture2ddecoder.decode_etc1(data, w, h)
    elif fmt == 0x12:
        Image.frombytes("RGBA", (w, h), data[:w * h * 4]).save(out)
        return w, h, fmt
    else:
        sys.exit(f"ismeretlen PVR-formátum: {fmt:#x}")
    img = Image.frombytes("RGBA", (w, h), rgba, "raw", "BGRA")
    if flags & 0x10000:
        img = img.transpose(Image.FLIP_TOP_BOTTOM)
    img.save(out)
    return w, h, fmt


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, gla = sys.argv[1], sys.argv[2]
    e = entries(gla)
    if cmd == "list":
        flt = sys.argv[3].lower() if len(sys.argv) > 3 else ""
        for name, blob in e.items():
            if flt in name.lower():
                print(f"{len(blob):10d}  {name}")
    elif cmd == "extract":
        open(sys.argv[4], "wb").write(e[sys.argv[3]])
    elif cmd == "png":
        print("PVR %dx%d formátum %#x" % pvr_to_png(e[sys.argv[3]], sys.argv[4]))
    elif cmd == "modules":
        bs = e[sys.argv[3]]
        n = struct.unpack_from("<8H", bs, 0)[2]
        base = 0x10 + n
        xs, ys, ws, hs = [struct.unpack_from("<%dH" % n, bs, base + k * n * 2) for k in range(4)]
        for i in range(n):
            print(f"{i:4d}  x={xs[i]:4d} y={ys[i]:4d} w={ws[i]:4d} h={hs[i]:4d}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
