#!/usr/bin/env bash
# 32 bites Switch-portlibek: libdrm_nouveau + Mesa (EGL, GLES2) a devkitPro Switch-es forkjaiból,
# a libnx32-höz és a patchelt devkitARM-hoz fordítva. Előfeltétel: toolchain/build-toolchain.sh.
# Telepítés: $DEVKITPRO/portlibs/switch32. Újraindítható: a kész lépéseket kihagyja.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEVKITPRO="${DEVKITPRO32:-$HOME/devkitpro32}"
WORK="${WORK:-$HOME/Library/Caches/backstab-nx-toolchain}/portlibs"
PORTLIBS="$DEVKITPRO/portlibs/switch32"
JOBS="$(sysctl -n hw.ncpu)"

LIBDRM_NOUVEAU_VER=1.0.1
MESA_VER=20.1.0
MESA_PKGREL=5   # devkitPro pacman-packages: switch/mesa/switch-mesa-20.1.0-5.patch

# a loaderrel egyező kódgenerálás: ARM-mód, VFP/NEON utasítások, de soft-float hívási konvenció
# -fno-short-enums: 4 bájtos enumok, mint a libnx32-ben (lásd build-toolchain.sh) és az Android ABI-ban
ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec -mfloat-abi=softfp -mfpu=neon-fp-armv8 -fno-short-enums"

export DEVKITPRO DEVKITARM="$DEVKITPRO/devkitARM"
export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"

mkdir -p "$WORK" "$PORTLIBS"
cd "$WORK"
log() { printf '\n==> %s\n' "$*"; }
done_mark() { touch "$WORK/.done-$1"; }
is_done() { [ -f "$WORK/.done-$1" ]; }
fetch() { [ -s "$2" ] || curl -fL --retry 3 -o "$2" "$1"; }

# 1. libdrm_nouveau (a libnx nv*-wrappereit használja, amelyeket a libnx32 32 bitre igazított)
if ! is_done libdrm_nouveau; then
  log "libdrm_nouveau $LIBDRM_NOUVEAU_VER"
  fetch "https://github.com/devkitPro/libdrm_nouveau/archive/v$LIBDRM_NOUVEAU_VER.tar.gz" "libdrm_nouveau-$LIBDRM_NOUVEAU_VER.tar.gz"
  rm -rf "libdrm_nouveau-$LIBDRM_NOUVEAU_VER"
  tar -xzf "libdrm_nouveau-$LIBDRM_NOUVEAU_VER.tar.gz"
  cd "libdrm_nouveau-$LIBDRM_NOUVEAU_VER"
  # 64 bites szabályok → 32 bites; -Werror ki (a GCC 16 újabb figyelmeztetései miatt)
  sed -i '' \
    -e 's|include $(DEVKITPRO)/libnx/switch_rules|include $(DEVKITPRO)/libnx32/switch32_rules|' \
    -e "s|^ARCH	:=.*|ARCH	:=	$ARCH|" \
    -e 's|-g -Wall -Werror|-g -Wall|' \
    Makefile
  grep -q 'switch32_rules' Makefile || { echo "a libdrm_nouveau Makefile javítása nem sikerült" >&2; exit 1; }
  make -j"$JOBS" PORTLIBS="$PORTLIBS" install
  cd "$WORK"; done_mark libdrm_nouveau
fi

# 2. Mesa (a devkitPro switch-mesa receptje alapján: GLES2 + EGL, nouveau gallium driver)
if ! is_done mesa; then
  log "Mesa $MESA_VER"
  sed -i '' "s|^prefix=.*|prefix=$PORTLIBS|" "$PORTLIBS/lib/pkgconfig/libdrm_nouveau.pc"

  # a Mesa kódgeneráló szkriptjeihez mako kell
  [ -x venv/bin/python3 ] || { "$(brew --prefix)/bin/python3.11" -m venv venv && venv/bin/pip install -q mako; }
  export PATH="$WORK/venv/bin:$PATH"

  P=https://raw.githubusercontent.com/devkitPro/pacman-packages/master/switch/mesa
  fetch "https://archive.mesa3d.org/older-versions/20.x/mesa-$MESA_VER-rc3.tar.xz" "mesa-$MESA_VER-rc3.tar.xz"
  echo "c90b75ea34302ebde9b81b87c5642fa864c40fe9c4ad34ce0793170c1413168d  mesa-$MESA_VER-rc3.tar.xz" | shasum -a 256 -c -
  fetch "$P/switch-mesa-$MESA_VER-$MESA_PKGREL.patch" switch-mesa.patch
  fetch "$P/gl_XML.py.patch" gl_XML.py.patch
  fetch "$P/glX_XML.py.patch" glX_XML.py.patch

  rm -rf "mesa-$MESA_VER-rc3"
  tar -xf "mesa-$MESA_VER-rc3.tar.xz"
  cd "mesa-$MESA_VER-rc3"
  for p in switch-mesa gl_XML.py glX_XML.py; do gpatch -s -p1 -i "../$p.patch"; done
  # a newlib 4.6 már deklarálja a timespec_get-et (de a libc-ben nincs meg), ezért a Mesa
  # statikus pótlása ütközne vele: átnevezzük
  perl -0pi -e 's/#ifndef HAVE_TIMESPEC_GET\n/#ifndef HAVE_TIMESPEC_GET\n#define timespec_get mesa_timespec_get\n/' include/c11/threads.h
  grep -q 'mesa_timespec_get' include/c11/threads.h || { echo "a threads.h javítása nem sikerült" >&2; exit 1; }

  # a meson próbalinkeléseihez is kell a libnx (a crt0 és a libsysbase arra hivatkozik)
  cat > ../switch32-meson.specs <<EOF
%include <$DEVKITPRO/libnx32/switch32.specs>

*libgloss:
+ -lnx
EOF
  meson_list() { local out="" a; for a in "$@"; do out="$out'$a', "; done; echo "[${out%, }]"; }
  CARGS=$(meson_list $ARCH -D__SWITCH__ -ffunction-sections -fdata-sections \
    -I"$DEVKITPRO/libnx32/include" -I"$PORTLIBS/include" \
    -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=implicit-function-declaration)
  # a newlib/libstdc++ rövid enumokkal fordult; a vegyes enum-méret itt szándékos (lásd fent)
  LARGS=$(meson_list $ARCH -Wl,--no-enum-size-warning -specs="$WORK/switch32-meson.specs" -L"$DEVKITPRO/libnx32/lib" -L"$PORTLIBS/lib")
  cat > ../cross-switch32.txt <<EOF
[binaries]
c = 'arm-none-eabi-gcc'
cpp = 'arm-none-eabi-g++'
ar = 'arm-none-eabi-gcc-ar'
strip = 'arm-none-eabi-strip'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = '$PORTLIBS/lib/pkgconfig'
needs_exe_wrapper = true

[built-in options]
c_args = $CARGS
cpp_args = $CARGS
c_link_args = $LARGS
cpp_link_args = $LARGS

[host_machine]
system = 'horizon'
cpu_family = 'arm'
cpu = 'cortex-a57'
endian = 'little'
EOF
  meson setup build --cross-file ../cross-switch32.txt \
    --prefix "$PORTLIBS" --libdir lib --default-library static --buildtype release -Db_ndebug=true \
    -Dplatforms=switch -Dgallium-drivers=nouveau -Ddri-drivers= -Dvulkan-drivers= \
    -Degl=true -Dgles1=false -Dgles2=true -Dopengl=true -Dshared-glapi=true \
    -Dglx=disabled -Dgbm=false -Dllvm=false -Dshader-cache=false -Dvalgrind=false \
    -Dlibunwind=false -Dlmsensors=false -Dbuild-tests=false -Dselinux=false \
    -Dgallium-xa=false -Dgallium-nine=false -Dgallium-va=false -Dgallium-vdpau=false \
    -Dgallium-xvmc=false -Dgallium-omx=disabled -Dgallium-opencl=disabled -Dosmesa=none
  ninja -C build -j"$JOBS" install
  cd "$WORK"; done_mark mesa
fi

log "kész: $PORTLIBS"
