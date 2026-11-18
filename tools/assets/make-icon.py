#!/usr/bin/env python3
"""Az NSP ikonja (icon.jpg, 256×256) a saját APK-d res/drawable/icon.png-jéből. A kép a játékhoz
tartozik, ezért nem kerül a repóba (.gitignore); ha nincs icon.jpg, a libnx alapikonja megy.

Használat: ~/Library/Caches/backstab-nx-toolchain/venv-tools/bin/python tools/assets/make-icon.py <BackStab.apk> [icon.jpg]
"""
import io
import sys
import zipfile
from pathlib import Path
from PIL import Image

apk = sys.argv[1]
out = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parents[2] / "icon.jpg"
with zipfile.ZipFile(apk) as z:
    img = Image.open(io.BytesIO(z.read("res/drawable/icon.png"))).convert("RGBA")
bg = Image.new("RGBA", img.size, (0, 0, 0, 255))   # a Switch-ikon nem lehet átlátszó
bg.alpha_composite(img)
bg.convert("RGB").resize((256, 256), Image.LANCZOS).save(out, "JPEG", quality=92)
print("kész:", out, "(eredeti:", img.size, ")")
