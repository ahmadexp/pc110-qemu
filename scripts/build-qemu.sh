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

# --- register them in hw/misc/meson.build (idempotent) ---
MB="$SRC/hw/misc/meson.build"
for f in pc110-fontrom pc110-chipset; do
  if ! grep -q "$f.c" "$MB"; then
    printf "system_ss.add(when: 'CONFIG_ISA_BUS', if_true: files('%s.c'))\n" "$f" >> "$MB"
  fi
done

# --- real-BIOS POST completer + core patches (experimental) ---
# pc110post.c is the TCG-level POST-loop/INT19/INT13 completer that lets the
# REAL 256 KiB PC110 BIOS run through POST and boot DOS (enabled at run time via
# the PC110POST env var).  It is a new target/i386 source plus small in-place
# edits to QEMU core files (the cpu-exec hook that calls it, the KBC CPU-only
# reset, the meson registration, and an optional cocoa window-scale).  See
# README "Booting the real BIOS".
echo ">> installing PC110 real-BIOS POST completer + patches ..."
cp "$ROOT/qemu/target-i386/pc110post.c" "$SRC/target/i386/"
for p in "$ROOT"/qemu/patches/*.patch; do
  [ -e "$p" ] || continue
  if patch -p1 -d "$SRC" -N --dry-run < "$p" >/dev/null 2>&1; then
    patch -p1 -d "$SRC" -N < "$p" >/dev/null
    echo "   applied $(basename "$p")"
  else
    echo "   skip $(basename "$p") (already applied)"
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
