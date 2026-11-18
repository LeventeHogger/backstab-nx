#!/usr/bin/env bash
# 32 bites (AArch32) Switch homebrew-eszközlánc fordítása macOS-en, natívan.
# A github.com/vita2hos/vita2hos-docker Dockerfile-ját követi, ugyanazokkal a rögzített commitokkal.
# Telepítés: $DEVKITPRO32 (alapból ~/devkitpro32), munkamappa: $WORK. Újraindítható: a kész lépéseket kihagyja.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEVKITPRO="${DEVKITPRO32:-$HOME/devkitpro32}"
WORK="${WORK:-$HOME/Library/Caches/backstab-nx-toolchain}"
JOBS="$(sysctl -n hw.ncpu)"

BUILDSCRIPTS_HASH=e6e5849f49e367ed9e5d45db7246e5c6b74feb6c   # vita2hos/buildscripts (switch32 patchek)
SWITCHTOOLS_HASH=66dd4dd14b7dc26f256b90fc73ace533ceaf62a2    # vita2hos/switch-tools (32 bites elf2nso/elf2nro)
GENERALTOOLS_HASH=46086605cdc63fb02ba0ed08cdc00801ba00c6f0   # devkitPro/general-tools (bin2s stb.)
LIBNX32_HASH=721c9773d265cc465ad79edff29b7de841aa2e52        # vita2hos/libnx (AArch32 support)

BREW="$(brew --prefix)"
export DEVKITPRO DEVKITARM="$DEVKITPRO/devkitARM"
export PATH="$BREW/opt/bison/bin:$BREW/opt/flex/bin:$BREW/opt/texinfo/bin:$BREW/opt/make/libexec/gnubin:$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"
# a macOS-es BSD patch kihagyja a patchek üres új fájljait (pl. newlib libsysbase/dummy.c) → GNU patch kell
PATCH="$(command -v gpatch)" || { echo "hiányzik: brew install gpatch" >&2; exit 1; }
export PATCH
HOST_CPPFLAGS="-I$BREW/include"
HOST_LDFLAGS="-L$BREW/lib"

mkdir -p "$WORK" "$DEVKITPRO"
cd "$WORK"
log() { printf '\n==> %s\n' "$*"; }
done_mark() { touch "$WORK/.done-$1"; }
is_done() { [ -f "$WORK/.done-$1" ]; }

checkout() { # repo hash dir
  [ -d "$3/.git" ] || git clone -q "https://github.com/$1.git" "$3"
  git -C "$3" fetch -q origin "$2" 2>/dev/null || true
  git -C "$3" checkout -q "$2"
}

# 1. devkitARM (binutils + GCC + newlib, switch32 patchekkel)
if ! is_done devkitarm; then
  log "devkitARM"
  checkout vita2hos/buildscripts "$BUILDSCRIPTS_HASH" buildscripts
  cd buildscripts
  git checkout -q -- build-devkit.sh
  git checkout -q -- build-newlib.sh
  sed -i '' "s|^INSTALLDIR=/opt/devkitpro|INSTALLDIR=$DEVKITPRO|" build-devkit.sh
  # a switch32-commit üzenete szerint a newlib is -fPIC-kel fordulna, de csak a GCC kapta meg;
  # nélküle a newlib exit/malloc szövegrelokációt hagy, és a -z text linkelés elhasal
  sed -i '' 's|^_target_cflags="-O2 -ffunction-sections -fdata-sections"|_target_cflags="-O2 -ffunction-sections -fdata-sections -fPIC"|' build-newlib.sh
  grep -q 'fdata-sections -fPIC"' build-newlib.sh || { echo "a build-newlib.sh javítása nem sikerült" >&2; exit 1; }
  mkdir -p archives
  cat > config.sh <<EOF
BUILD_DKPRO_PACKAGE=1
BUILD_DKPRO_SRCDIR=$WORK/buildscripts/archives
export MAKEFLAGS="-j$JOBS"
BUILD_DKPRO_AUTOMATED=1
GCC_DOWNLOAD_PREREQS=0
EOF
  BUILD_DKPRO_PACKAGE=1 . ./select_toolchain.sh >/dev/null
  RULES_VER="$(grep -e '^DKARM_RULES_VER=' build-devkit.sh | cut -d= -f2)"
  CRTLS_VER="$(grep -e '^DKARM_CRTLS_VER=' build-devkit.sh | cut -d= -f2)"
  cd archives
  fetch() { [ -s "$2" ] || curl -fL --retry 3 -o "$2" "$1"; }
  fetch "https://ftpmirror.gnu.org/gnu/binutils/binutils-$BINUTILS_VER.tar.xz" "binutils-$BINUTILS_VER.tar.xz"
  fetch "https://ftpmirror.gnu.org/gnu/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz" "gcc-$GCC_VER.tar.xz"
  fetch "https://sourceware.org/pub/newlib/newlib-$NEWLIB_VER.tar.gz" "newlib-$NEWLIB_VER.tar.gz"
  # a downloads.devkitpro.org 403-at ad, ugyanez a GitHub-tagekből
  fetch "https://github.com/devkitPro/devkitarm-rules/archive/refs/tags/v$RULES_VER.tar.gz" "devkitarm-rules-$RULES_VER.tar.gz"
  fetch "https://github.com/devkitPro/devkitarm-crtls/archive/refs/tags/v$CRTLS_VER.tar.gz" "devkitarm-crtls-$CRTLS_VER.tar.gz"
  cd ..
  CPPFLAGS="$HOST_CPPFLAGS" LDFLAGS="$HOST_LDFLAGS" OSXMIN=13.0 ./build-devkit.sh
  # a rules-csomag Makefile-ja fixen /opt/devkitpro-ba telepítene, ezért kézzel másoljuk
  tar -xzf "archives/devkitarm-rules-$RULES_VER.tar.gz" -C "$WORK"
  cp "$WORK/devkitarm-rules-$RULES_VER/base_rules" "$WORK/devkitarm-rules-$RULES_VER/base_tools" "$DEVKITARM/"
  "$DEVKITARM/bin/arm-none-eabi-gcc" --version | head -1
  cd "$WORK"; done_mark devkitarm
fi

# 2. switch-tools (elf2nso, elf2nro, npdmtool, build_pfs0, nacptool) – 32 bites ELF-támogatással
if ! is_done switch-tools; then
  log "switch-tools"
  checkout vita2hos/switch-tools "$SWITCHTOOLS_HASH" switch-tools
  cd switch-tools
  ./autogen.sh
  # macOS-en nincs <elf.h>: a glibc-s változat a toolchain/compat-ban
  CPPFLAGS="$HOST_CPPFLAGS -I$SCRIPT_DIR/compat" LDFLAGS="$HOST_LDFLAGS" ./configure --prefix="$DEVKITPRO/tools"
  make -j"$JOBS" install
  cd "$WORK"; done_mark switch-tools
fi

# 3. general-tools (bin2s)
if ! is_done general-tools; then
  log "general-tools"
  checkout devkitPro/general-tools "$GENERALTOOLS_HASH" general-tools
  cd general-tools
  ./autogen.sh
  CPPFLAGS="$HOST_CPPFLAGS" LDFLAGS="$HOST_LDFLAGS" ./configure --prefix="$DEVKITPRO/tools"
  make -j"$JOBS" install
  cd "$WORK"; done_mark general-tools
fi

# 4. libnx32
if ! is_done libnx32; then
  log "libnx32"
  checkout vita2hos/libnx "$LIBNX32_HASH" libnx
  # az arm-none-eabi alapból 1 bájtos enumokat használ (-fshort-enums), a libnx viszont a 64 bites
  # elrendezésre épül (pl. NvGraphicBuffer → nvnflinger IPC), és az Android ABI is 4 bájtos enumú
  sed -i '' -e 's|^ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec$|& -fno-short-enums|' libnx/nx/Makefile.32
  grep -q 'fno-short-enums' libnx/nx/Makefile.32 || { echo "a libnx Makefile.32 javítása nem sikerült" >&2; exit 1; }
  make -C libnx/nx -f Makefile.32 -j"$JOBS" install
  done_mark libnx32
fi

log "kész: DEVKITPRO=$DEVKITPRO"
