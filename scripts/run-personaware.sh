#!/bin/sh
# Boot IBM Personaware (PC110) with the emulated kanji font ROM.
#
# Requires:
#   qemu-src/build/qemu-system-i386   (scripts/build-qemu.sh)
#   roms/pc110-fontrom.bin            (see roms/README.md)
#   disks/Personaware-disk.img        (see disks/README.md)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QEMU="$ROOT/qemu-src/build/qemu-system-i386"

# QEMU_COCOA_SCALE opens the cocoa window at this multiple of the guest
# resolution (the PC110 screen is tiny on a high-DPI/Retina Mac).  Default 2;
# override, e.g.  QEMU_COCOA_SCALE=3 scripts/run-personaware.sh
exec env QEMU_COCOA_SCALE="${QEMU_COCOA_SCALE:-2}" "$QEMU" \
  -m 20M -cpu 486 \
  -bios "$ROOT/qemu-src/pc-bios/bios.bin" \
  -drive file="$ROOT/disks/Personaware-disk.img",format=raw,index=0,media=disk \
  -boot c \
  -net nic,model=ne2k_isa \
  -vga cirrus \
  -audiodev "${PC110_AUDIODEV:-coreaudio},id=snd0" -machine pcspk-audiodev=snd0 \
  -device pc110-fontrom,romfile="$ROOT/roms/pc110-fontrom.bin" \
  -display cocoa,zoom-to-fit=on
