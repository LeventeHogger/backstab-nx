#!/usr/bin/env bash
# Telepíthető NSP (NCA-kkal) a hacBrewPack-kel – ez látszik az Eden játéklistájában, és ezt lehet valódi
# Switchre DBI-jal/Tinfoillal telepíteni. (A make által épített build/backstab.nsp csak exefs-csomag: indítható,
# de az Eden nem listázza.) Előtte: make. Kimenet: dist/BackstabHD.nsp
#
# Kell hozzá: hacBrewPack (GPLv2, a törölt The-4n/hacBrewPack tükre: github.com/rlaphoenix/hacBrewPack),
# lefordítva: cd <mappa> && cp config.mk.template config.mk && make CC=clang
# és a saját konzolod kulcsai (prod.keys: header_key, key_area_key_application_xx). Egyik sem kerül a repóba.
set -euo pipefail
cd "$(dirname "$0")/../.."

HBP="${HACBREWPACK:-$HOME/Library/Caches/backstab-nx-toolchain/hacbrewpack/hacbrewpack}"
KEYS="${KEYS:-$HOME/.local/share/eden/keys/prod.keys}"
TID=0100bac57ab00000   # = backstab.json program_id (saját alkalmazás-azonosító; a régi 010000000000100d az Album appleté volt)

[ -x "$HBP" ] || { echo "nincs lefordított hacbrewpack: $HBP" >&2; exit 1; }
[ -f "$KEYS" ] || { echo "nincs kulcsfájl: $KEYS" >&2; exit 1; }
[ -f build/exefs/main ] && [ -f build/exefs/control.nacp ] || { echo "előbb: make" >&2; exit 1; }

W=build/hbp
rm -rf "$W"
mkdir -p "$W/exefs" "$W/control"
cp build/exefs/main build/exefs/main.npdm "$W/exefs/"
cp build/exefs/control.nacp "$W/control/"
python3 tools/package/nacp-fix.py "$W/control/control.nacp" "$TID"
# a HOME menü a rendszernyelv szerinti ikont keresi: mindegyik nyelvre ugyanaz
for lang in AmericanEnglish BritishEnglish Japanese French German LatinAmericanSpanish Spanish Italian Dutch \
            CanadianFrench Portuguese Russian Korean TraditionalChinese SimplifiedChinese BrazilianPortuguese; do
  cp build/exefs/icon_AmericanEnglish.dat "$W/control/icon_$lang.dat"
done
(cd "$W" && "$HBP" -k "$KEYS" --titleid "$TID" --noromfs --nologo --nspdir nsp)

mkdir -p dist
cp "$W/nsp/$TID.nsp" dist/BackstabHD.nsp
echo "kész: dist/BackstabHD.nsp"
