#!/usr/bin/env python3
"""A debug-build képkocka-mentései (sdmc:/switch/backstab/shots/*.rgba) → PNG.

A gl.c a csere előtti hátsó puffert írja ki: 1920×1080 RGBA8, fejléc nélkül, alulról felfelé (glReadPixels).
Futtatás: <Pillow-s python> tools/dev/shot2png.py <.rgba fájlok vagy mappa> [--out <mappa>] [--scale 0.5]
A kimenet alapból a fájlok mellé kerül (.png); a repóba soha.
"""
import argparse
from pathlib import Path

from PIL import Image

W, H = 1920, 1080


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+", type=Path)
    ap.add_argument("--out", type=Path)
    ap.add_argument("--scale", type=float, default=1.0)
    a = ap.parse_args()
    files = []
    for p in a.paths:
        files += sorted(p.glob("*.rgba")) if p.is_dir() else [p]
    for f in files:
        data = f.read_bytes()
        if len(data) != W * H * 4:
            print(f"kihagyva (méret {len(data)}): {f}")
            continue
        img = Image.frombytes("RGBA", (W, H), data).transpose(Image.Transpose.FLIP_TOP_BOTTOM).convert("RGB")
        if a.scale != 1.0:
            img = img.resize((round(W * a.scale), round(H * a.scale)), Image.Resampling.LANCZOS)
        out = (a.out or f.parent) / (f.stem + ".png")
        out.parent.mkdir(parents=True, exist_ok=True)
        img.save(out)
        print(out)


if __name__ == "__main__":
    main()
