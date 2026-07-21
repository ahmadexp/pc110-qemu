#!/bin/sh
# EXPERIMENTAL: boot the REAL 256 KiB IBM PC110 BIOS on QEMU (not SeaBIOS).
#
# Status: the real BIOS boots ALL THE WAY to the Personaware pen desktop -- POST,
# DOS, the RIOS driver stack, the VGA mode-12h handoff, and the launcher, all
# driven by the genuine 256 KiB ROM.  Two things make it work:
#   1. Loose protected mode (qemu/patches/05-seg-helper-loose-pm.patch): post-boot,
#      QEMU's PM segment helpers do base-only resolution with no type/privilege
#      checks, so the BIOS/driver's "unreal mode" (PE=1 with real-mode segment
#      values) does not #GP-storm.  Mirrors PC110-EMU's segment resolver.
#   2. A CONFIG.SYS WITHOUT EMM386.  EMM386's V86/paging monitor assumes faithful
#      PM and conflicts with the driver's unreal-mode segment use; removing it
#      leaves loose PM as the only regime and the boot runs clean to the desktop.
#      (See disks/README.md for the one-line mtools recipe to strip EMM386.)
# See README "Booting the real BIOS" for the full write-up.
#
# The POST completer is enabled by the PC110POST env var and reads the boot image
# named by PC110BOOT.  PC110RSTLOG=1 turns on verbose reset/driver diagnostics;
# PC110HEARTBEAT=1 samples where non-BIOS code runs; PC110RESUME=1 re-enables the
# legacy A6E4 reset-resume shim (superseded by loose PM).
#
# Requires:
#   qemu-src/build/qemu-system-i386   (scripts/build-qemu.sh)
#   roms/pc110_bios.bin               (see roms/README.md)
#   roms/pc110-fontrom.bin            (see roms/README.md)
#   disks/Personaware-disk.img        (see disks/README.md)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QEMU="$ROOT/qemu-src/build/qemu-system-i386"
BIOS="$ROOT/roms/pc110_bios.bin"
DISK="$ROOT/disks/Personaware-disk.img"
FONT="$ROOT/roms/pc110-fontrom.bin"
ESIMG="${PC110SETUPIMG:-$ROOT/boot/easysetup.bin}"   # for the in-POST F1 -> Easy-Setup

# The Personaware CF image is 8192 sectors = 128 cyl x 2 heads x 32 secs (4 MB,
# partition at LBA 32); present that geometry explicitly so the software INT13
# service and the boot chain agree.  QEMU_COCOA_SCALE: see run-personaware.sh.
# Audio: the PC110's sound is the PC speaker (PIT ch2 gated by port 0x61), just
# like the reference emulator.  Wire QEMU's built-in PC speaker to a host audio
# backend so beeps are audible.  Default to CoreAudio (macOS); set PC110_AUDIODEV
# (e.g. pa / pipewire / alsa) on other hosts, or =none to mute.
# Realistic boot (on by default): show the genuine BIOS POST on screen (real POST
# codes -> "Starting PC DOS..."), a power-on beep, and a "Press F1 for Easy-Setup"
# prompt.  Press F1 promptly (the emulated POST reaches its boot decision in about
# a second) to enter Easy-Setup; otherwise Personaware boots.  Run with
# PC110POSTUI=0 to disable it and boot straight to Personaware.
POSTUI_ENV=""
[ "${PC110POSTUI-1}" != 0 ] && POSTUI_ENV="PC110POSTUI=1"
exec env PC110POST=1 $POSTUI_ENV PC110SETUPIMG="$ESIMG" \
  PC110BOOT="$DISK" QEMU_COCOA_SCALE="${QEMU_COCOA_SCALE:-2}" \
  "$QEMU" \
  -m 4 -cpu 486 \
  -bios "$BIOS" \
  -drive id=hd0,file="$DISK",format=raw,if=none,snapshot=on \
  -device ide-hd,drive=hd0,bus=ide.0,unit=0,cyls=128,heads=2,secs=32 \
  -boot c -vga std \
  -audiodev "${PC110_AUDIODEV:-coreaudio},id=snd0" -machine pcspk-audiodev=snd0 \
  -device "pc110-chipset,biosfile=$BIOS,vgac000=on" \
  -device pc110-fontrom,romfile="$FONT" \
  -display cocoa,zoom-to-fit=on "$@"
