#!/usr/bin/env bash
# Fordítás + indítás az Eden emulátorban (macOS), majd a napló kiírása.
#   tools/dev/run-eden.sh [másodperc]   – ennyi ideig fut (alap 60), utána leállítja az Edent
# Előfeltétel: /Applications/Eden.app, kulcsok + firmware a ~/.local/share/eden alatt, a játék fájljai
# az emulált SD-n: ~/.local/share/eden/sdmc/switch/backstab/{libPirates.so,com.gameloft.android.ANMP.GloftSDHM/}
# Az Eden a nyers exefs/main-t 64 bitesnek venné és összeomlana: mindig az NSP-t kell kapnia.
set -euo pipefail
cd "$(dirname "$0")/../.."

SECS="${1:-60}"
EDEN=/Applications/Eden.app/Contents/MacOS/eden
EDEN_DATA="$HOME/.local/share/eden"
SD="$EDEN_DATA/sdmc/switch/backstab"
EDEN_LOG="$EDEN_DATA/log/eden_log.txt"

export DEVKITPRO="${DEVKITPRO:-$HOME/devkitpro32}" DEVKITARM="$HOME/devkitpro32/devkitARM"
export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"
[ -n "${NOBUILD:-}" ] || make -j"$(sysctl -n hw.ncpu)"

[ -f "$SD/libPirates.so" ] || { echo "hiányzik: $SD/libPirates.so" >&2; exit 1; }
pkill -9 -f "$EDEN" 2>/dev/null || true
rm -f "$SD/log.txt"

"$EDEN" -g "$PWD/build/backstab.nsp" >/dev/null 2>&1 &
PID=$!
for _ in $(seq "$SECS"); do
  kill -0 "$PID" 2>/dev/null || break
  sleep 1
done
if kill -0 "$PID" 2>/dev/null; then kill -9 "$PID"; echo "== leállítva ${SECS} mp után"; else echo "== az Eden magától kilépett"; fi

echo "== loader-napló ($SD/log.txt)"
tail -n 60 "$SD/log.txt" 2>/dev/null || echo "(nincs)"
echo "== Eden-napló (hibák, debug-kimenet)"
grep -E 'Debug.Emulated|<Critical>|<Error>|Unmapped|Unhandled|abort|Break' "$EDEN_LOG" \
  | grep -vE 'Render.Vulkan|IterateDirEntries|Failed to read title' | tail -n 40 || true
