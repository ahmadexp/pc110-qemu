#!/bin/sh
# Fetch QEMU 11.0.2, add the PC110 device models, and build qemu-system-i386.
#
# Produces: qemu-src/build/qemu-system-i386
#
# macOS (Apple Silicon) note: the build must use the native arm64 Homebrew
# toolchain in /opt/homebrew.  A stray x86_64 Homebrew in /usr/local confuses
# meson/ninja and leaks x86 dylibs into the link, so we pin PATH/PKG_CONFIG and
# disable the optional features that pull those libs in.
set -e

QEMU_VER=11.0.2
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/qemu-src"

# --- fetch QEMU source ---
if [ ! -d "$SRC" ]; then
  echo ">> downloading QEMU $QEMU_VER ..."
  curl -L -o "$ROOT/qemu-$QEMU_VER.tar.xz" "https://download.qemu.org/qemu-$QEMU_VER.tar.xz"
  tar xf "$ROOT/qemu-$QEMU_VER.tar.xz" -C "$ROOT"
  mv "$ROOT/qemu-$QEMU_VER" "$SRC"
  rm -f "$ROOT/qemu-$QEMU_VER.tar.xz"
fi

# --- drop in the PC110 device models ---
echo ">> installing PC110 device models ..."
cp "$ROOT/qemu/hw-misc/pc110-fontrom.c"  "$SRC/hw/misc/"
cp "$ROOT/qemu/hw-misc/pc110-chipset.c"  "$SRC/hw/misc/"
cp "$ROOT/qemu/hw-misc/pc110-setupcfg.c" "$SRC/hw/misc/"

# --- register them in hw/misc/meson.build (idempotent) ---
MB="$SRC/hw/misc/meson.build"
for f in pc110-fontrom pc110-chipset pc110-setupcfg; do
  if ! grep -q "$f.c" "$MB"; then
    printf "system_ss.add(when: 'CONFIG_ISA_BUS', if_true: files('%s.c'))\n" "$f" >> "$MB"
  fi
done

# --- configure & build ---
mkdir -p "$SRC/build"
cd "$SRC/build"

CONFIGURE_FLAGS="--target-list=i386-softmmu --disable-werror"
if [ "$(uname -s)" = "Darwin" ] && [ -d /opt/homebrew ]; then
  echo ">> Apple Silicon: using /opt/homebrew toolchain"
  export PATH="/opt/homebrew/bin:$PATH"
  export PKG_CONFIG_LIBDIR="/opt/homebrew/lib/pkgconfig:/opt/homebrew/share/pkgconfig"
  # SDL2/capstone/slirp/etc. in /usr/local are x86 -> disable to keep an arm64 link
  CONFIGURE_FLAGS="$CONFIGURE_FLAGS --disable-sdl --disable-libssh --disable-curl \
    --disable-vnc --disable-capstone --disable-slirp"
fi

../configure $CONFIGURE_FLAGS
ninja qemu-system-i386

echo ""
echo ">> built: $SRC/build/qemu-system-i386"
"$SRC/build/qemu-system-i386" -device help 2>&1 | grep -i pc110 || true
