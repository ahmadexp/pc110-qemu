#!/bin/sh
# Boot the real PC110 Easy-Setup (the graphical F1 BIOS setup) under SeaBIOS.
#
#   - Easy-Setup is on a floppy (drive A) and booted first (once=a).
#   - Personaware is the hard disk (drive C:).
#   - Exiting / Restarting Easy-Setup returns to normal Personaware mode, via:
#       * retf            -> exit stub -> boots the hard disk
#       * INT 19h         -> vector hooked to the exit stub
#       * hardware reset  -> QEMU 'once=a' reverts the boot order to the HDD
#
# Requires:
#   qemu-src/build/qemu-system-i386   (scripts/build-qemu.sh)
#   disks/easysetup-floppy.img        (boot/build-floppy.sh)
#   roms/pc110-fontrom.bin, disks/Personaware-disk.img
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QEMU="$ROOT/qemu-src/build/qemu-system-i386"

exec "$QEMU" \
  -m 20M -cpu 486 \
  -bios "$ROOT/qemu-src/pc-bios/bios.bin" \
  -fda "$ROOT/disks/easysetup-floppy.img" \
  -drive file="$ROOT/disks/Personaware-disk.img",format=raw,index=0,media=disk \
  -boot order=c,once=a \
  -vga cirrus \
  -audiodev "${PC110_AUDIODEV:-coreaudio},id=snd0" -machine pcspk-audiodev=snd0 \
  -device pc110-fontrom,romfile="$ROOT/roms/pc110-fontrom.bin" \
  -display cocoa,zoom-to-fit=on
