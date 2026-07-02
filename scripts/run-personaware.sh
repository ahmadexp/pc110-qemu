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

exec "$QEMU" \
  -m 20M -cpu 486 \
  -bios "$ROOT/qemu-src/pc-bios/bios.bin" \
  -drive file="$ROOT/disks/Personaware-disk.img",format=raw,index=0,media=disk \
  -boot c \
  -net nic,model=ne2k_isa \
  -vga cirrus \
  -device pc110-fontrom,romfile="$ROOT/roms/pc110-fontrom.bin" \
  -display cocoa,zoom-to-fit=on
