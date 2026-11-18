#!/bin/sh
# check-imports.sh – minden import (imports.txt, soronként egy név) szerepel-e a dynlib.c táblájában.
# Használat: tools/dev/check-imports.sh [imports.txt] [dynlib.c]
# Az imports.txt: arm-none-eabi-readelf --dyn-syms -W libPirates.so | awk '$7=="UND" && $8!="" {print $8}'
# (verziócímke nélkül); alapból az eszközlánc munkamappájából.
set -eu
here=$(cd "$(dirname "$0")/../.." && pwd)
imports=${1:-"$HOME/Library/Caches/backstab-nx-toolchain/game/imports.txt"}
dynlib=${2:-"$here/source/dynlib.c"}
[ -r "$imports" ] || { echo "nincs meg: $imports" >&2; exit 2; }
[ -r "$dynlib" ] || { echo "nincs meg: $dynlib" >&2; exit 2; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# a táblasorok: DIRECT(név) / WRAP(név, függvény) / { "név", ... }
sed -nE 's/^[[:space:]]*(DIRECT|WRAP)\([[:space:]]*([A-Za-z0-9_]+).*/\2/p; s/^[[:space:]]*\{[[:space:]]*"([^"]+)".*/\1/p' \
    "$dynlib" | sort > "$tmp/table"
sed -e 's/@.*//' -e 's/[[:space:]]//g' "$imports" | grep -v '^$' | sort -u > "$tmp/imports"

missing=$(comm -23 "$tmp/imports" "$tmp/table")
extra=$(comm -13 "$tmp/imports" "$tmp/table" | sort -u)
dups=$(uniq -d "$tmp/table")

echo "importok: $(wc -l < "$tmp/imports" | tr -d ' '), táblasorok: $(wc -l < "$tmp/table" | tr -d ' ')"
[ -n "$dups" ] && { echo "kétszer szerepel:"; echo "$dups" | sed 's/^/  /'; }
[ -n "$extra" ] && { echo "a táblában van, de a játék nem importálja:"; echo "$extra" | sed 's/^/  /'; }
if [ -n "$missing" ]; then
    echo "HIÁNYZIK ($(echo "$missing" | wc -l | tr -d ' ')):"
    echo "$missing" | sed 's/^/  /'
    exit 1
fi
echo "hiányzó import: 0"
[ -z "$dups" ]
