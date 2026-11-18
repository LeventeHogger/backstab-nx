#!/usr/bin/env bash
# Kipróbálható Switch-csomag a saját játékfájlokból → dist/BackstabHD-Switch/ és dist/BackstabHD-Switch.zip
#   SD-kartyara/switch/backstab/     – libPirates.so + a játék adatmappája (files/, benne intro.mpg)
#   SD-kartyara/BackstabHD.nsp       – DBI-jal telepítendő (tools/package/pack-nsp.sh kimenete)
#   OLVASS_EL.txt                    – útmutató (tools/package/OLVASS_EL.txt)
# Előtte: kiadási make (nem debug) és tools/package/pack-nsp.sh. A nagy fájlok APFS-klónként kerülnek a mappába (nem
# foglalnak plusz helyet; a zip igen). A csomag a saját példányból készül – a repóba és nyilvános helyre soha.
#
# Források (környezeti változóval felülírhatók):
#   SO       – az APK libPirates.so-ja (SHA1 9a36ce92…, más verzióra a foltok nem illenek)
#   DATA     – a játék adatmappája (files/); a mentések, a felbontáshoz nem illő sprite-csomagok és az mp4-ek kimaradnak
#   STRINGS  – angol strings.gla, ha a DATA-é más nyelvű (alap: a Gameloft eredeti csomagjából, ha megvan)
#   EFFECTS  – az eredeti effects.gla (a Gameloft csomagjából) a DATA-é helyett, ha megvan; EFFECTS= üresen kihagyja
#   NOZIP=1  – csak a mappa
set -euo pipefail
cd "$(dirname "$0")/../.."

CACHE="$HOME/Library/Caches/backstab-nx-toolchain"
SO="${SO:-$CACHE/game/libPirates.so}"
DATA="${DATA:-$HOME/Documents/com.gameloft.android.ANMP.GloftSDHM/files}"
STRINGS="${STRINGS-$CACHE/gameloft-data/strings.gla}"
EFFECTS="${EFFECTS-$CACHE/gameloft-data/amz/effects.gla}"
NSP=dist/BackstabHD.nsp
OUT=dist/BackstabHD-Switch
ZIP=dist/BackstabHD-Switch.zip

die() { echo "hiba: $*" >&2; exit 1; }

[ -f build/.defines ] && ! grep -q -- -DDEBUG build/.defines || die "a build debug – előbb kiadási make (DEFINES nélkül)"
[ -f "$NSP" ] || die "nincs $NSP – előbb: tools/package/pack-nsp.sh"
[ "$NSP" -nt build/exefs/main ] || die "$NSP régebbi a buildnél – előbb: tools/package/pack-nsp.sh"
[ "$(shasum "$SO" | cut -d' ' -f1)" = 9a36ce92ac09f2908c155ab1debb3e138a3496b8 ] || die "rossz libPirates.so: $SO"
[ -f "$DATA/sprites_1920x1080.gla" ] || die "nincs játékadat (sprites_1920x1080.gla): $DATA"

clone() { cp -c "$1" "$2" 2>/dev/null || cp "$1" "$2"; }

rm -rf "$OUT" "$ZIP"
SD="$OUT/SD-kartyara/switch/backstab"
FILES="$SD/com.gameloft.android.ANMP.GloftSDHM/files"
mkdir -p "$FILES"
clone "$SO" "$SD/libPirates.so"
n=0
for f in "$DATA"/*; do
  [ -f "$f" ] || continue
  case "$(basename "$f")" in
    # mentések és beállítások (a saját játékállásod ne menjen tovább), a port által nem használt fájlok
    save*.dat|globalsave*.dat|settings.dat|control0|*.mp4|*.mpg|*.bak|*.ru|strings.gla.*|.*) continue ;;
    sprites_*.gla) [ "$(basename "$f")" = sprites_1920x1080.gla ] || continue ;;
  esac
  clone "$f" "$FILES/$(basename "$f")"
  n=$((n + 1))
done
if [ -n "$STRINGS" ] && [ -f "$STRINGS" ]; then
  clone "$STRINGS" "$FILES/strings.gla"
  echo "strings.gla: $STRINGS"
fi
if [ -n "$EFFECTS" ] && [ -f "$EFFECTS" ]; then
  clone "$EFFECTS" "$FILES/effects.gla"
  echo "effects.gla: $EFFECTS"
fi
# a bevezető videó (az Android-indító játszotta le; a port MPEG-1-ből, lásd tools/assets/make-intro.sh)
if [ -f "$DATA/intro.mp4" ] && command -v ffmpeg >/dev/null; then
  tools/assets/make-intro.sh "$DATA/intro.mp4" "$FILES/intro.mpg" >/dev/null
else
  echo "figyelem: nincs intro.mp4 vagy ffmpeg – a bevezető videó kimarad" >&2
fi
clone "$NSP" "$OUT/SD-kartyara/BackstabHD.nsp"
cp tools/package/OLVASS_EL.txt "$OUT/OLVASS_EL.txt"
# a macOS rejtett fájljai (.DS_Store, ._*) és kiterjesztett attribútumai ne kerüljenek az SD-re / a zipbe
find "$OUT" \( -name .DS_Store -o -name '._*' \) -delete
xattr -rc "$OUT" 2>/dev/null || true

echo "kész: $OUT ($n adatfájl, $(du -sh "$OUT/SD-kartyara" | cut -f1) az SD-re)"
if [ -z "${NOZIP:-}" ]; then
  (cd dist && zip -q -r -0 -X "$(basename "$ZIP")" "$(basename "$OUT")")
  echo "zip: $ZIP ($(du -h "$ZIP" | cut -f1))"
fi
