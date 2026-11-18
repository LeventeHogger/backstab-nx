#!/usr/bin/env bash
# Az Eden 1. játékosának bevitele: keyboard (automatikus teszthez: A=C, B=X, X=V, Y=Z, bal kar WASD,
# jobb kar IJKL, +=M) vagy controller (a saját kontrollered, a mentett beállításból).
#   tools/dev/eden-input.sh keyboard | controller
# Csak zárt Edennel (kilépéskor felülírja a beállítást). A kontrolleres beállítás mentése:
# ~/Library/Caches/backstab-nx-toolchain/qt-config.controller.ini (a karokon invert_y:- kell!).
set -euo pipefail
C="$HOME/.config/eden/qt-config.ini"
SAVED="$HOME/Library/Caches/backstab-nx-toolchain/qt-config.controller.ini"

if pgrep -f /Applications/Eden.app/Contents/MacOS/eden >/dev/null; then
  echo "előbb zárd be az Edent" >&2
  exit 1
fi
case "${1:-}" in
  keyboard)
    grep -q '^player_0_.*engine:sdl' "$C" && cp "$C" "$SAVED"
    # a player_0 kiosztás alapértékre (= billentyűzet); a tárolt SDL-értékek maradnak, csak nem élnek
    sed -i '' -E 's/^(player_0_[a-z0-9_]+)\\default=false$/\1\\default=true/' "$C"
    echo "1. játékos: billentyűzet (vissza: $0 controller)"
    ;;
  controller)
    [ -f "$SAVED" ] || { echo "nincs mentett kontrolleres beállítás: $SAVED" >&2; exit 1; }
    # csak a player_0_ sorokat hozzuk vissza a mentésből; a többi beállítás (pl. hang) marad
    python3 - "$C" "$SAVED" <<'PY'
import sys
cur, saved = sys.argv[1], sys.argv[2]
p0 = {l.split('=', 1)[0]: l for l in open(saved, encoding='utf-8') if l.startswith('player_0_')}
out = [p0.pop(l.split('=', 1)[0], l) if l.startswith('player_0_') else l for l in open(cur, encoding='utf-8')]
open(cur, 'w', encoding='utf-8').writelines(out)
PY
    echo "1. játékos: kontroller"
    ;;
  *)
    echo "használat: $0 keyboard | controller" >&2
    exit 1
    ;;
esac
