#!/bin/sh
# Enter the PC110 Easy-Setup BIOS the authentic way: boot the REAL 256 KiB BIOS
# and take the F1-at-POST branch into Easy-Setup, showing the graphical config
# menu (Config / Date-Time / Password / Start up / Test / Restart).
#
# How it works (target/i386/pc110post.c):
#   * PC110SETUP=1 makes the POST completer take the F1 outcome at the boot
#     decision point (F000:52BD) instead of booting the disk -- the ROM's own
#     F1 branch is F000:3273 -> F000:3391.
#   * The ROM's live Easy-Setup decompress+enter stalls under emulation, so we
#     load the extracted Easy-Setup program (PC110SETUPIMG, produced by
#     boot/build-floppy.sh via tools/unpack-easysetup.py) to 0x50000 and enter
#     it at its real entry 5000:0000.
#   * Easy-Setup reads the POST error log via INT 15h AH=21h AL=00h and shows an
#     ERROR/diagnostics screen when it is non-empty; our emulated POST logs
#     spurious entries, so PC110SETUP also forces that service to return an
#     EMPTY log -- so Easy-Setup opens the config menu.
#
# Requires (in addition to run-realbios.sh's needs):
#   boot/easysetup.bin  -- run boot/build-floppy.sh first to unpack it from the ROM
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QEMU="$ROOT/qemu-src/build/qemu-system-i386"
BIOS="$ROOT/roms/pc110_bios.bin"
DISK="$ROOT/disks/Personaware-disk.img"
FONT="$ROOT/roms/pc110-fontrom.bin"
ESIMG="${PC110SETUPIMG:-$ROOT/boot/easysetup.bin}"
[ -f "$ESIMG" ] || { echo "error: missing $ESIMG -- run boot/build-floppy.sh first"; exit 1; }

exec env PC110POST=1 PC110SETUP=1 PC110SETUPIMG="$ESIMG" PC110BOOT="$DISK" \
  QEMU_COCOA_SCALE="${QEMU_COCOA_SCALE:-2}" \
  "$QEMU" \
  -m 4 -cpu 486 \
  -bios "$BIOS" \
  -drive id=hd0,file="$DISK",format=raw,if=none,snapshot=on \
  -device ide-hd,drive=hd0,bus=ide.0,unit=0,cyls=128,heads=2,secs=32 \
  -boot c -vga std \
  -device "pc110-chipset,biosfile=$BIOS,vgac000=on" \
  -device pc110-fontrom,romfile="$FONT" \
  -display cocoa,zoom-to-fit=on "$@"
