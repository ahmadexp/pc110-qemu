#!/bin/sh
# EXPERIMENTAL: boot the REAL 256 KiB IBM PC110 BIOS on QEMU (not SeaBIOS).
#
# Status: the real BIOS runs the full POST (memory sizing, chipset self-tests,
# the C&T flat-panel VGA BIOS, the KBC warm-reset state machine), boots the
# disk via a software INT19/INT13 service, runs the MS-DOS 7 kernel + CONFIG.SYS
# drivers, and now reaches a STABLE post-boot idle state: the RIOS/Personaware
# power driver's protected-mode idle loop (enter PM, KBC-0xFE reset to exit PM,
# resume) runs without crashing.  The old terminal wedge -- an untagged post-DOS
# reset that cold-re-POSTed and cascaded into an unexpected-interrupt HLT -- is
# fixed by resuming those resets through the BIOS's own resume handler (F000:A6E4)
# instead of cold-booting (target/i386/pc110post.c; disable with PC110NORESUME).
# It does NOT yet render the desktop on-screen: the Chips & Technologies F65535
# flat-panel VGA mode-set is not modeled, so the framebuffer stays blank even
# though DOS + the driver stack are running underneath.
# See README "Booting the real BIOS" for the full write-up.
#
# The POST completer is enabled by the PC110POST env var and reads the boot
# image named by PC110BOOT.  PC110RSTLOG=1 turns on the (verbose) reset/driver
# diagnostics; PC110HEARTBEAT=1 samples where non-BIOS code runs at steady state.
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

# The Personaware CF image is 8192 sectors = 128 cyl x 2 heads x 32 secs (4 MB,
# partition at LBA 32); present that geometry explicitly so the software INT13
# service and the boot chain agree.  QEMU_COCOA_SCALE: see run-personaware.sh.
exec env PC110POST=1 PC110BOOT="$DISK" QEMU_COCOA_SCALE="${QEMU_COCOA_SCALE:-2}" \
  "$QEMU" \
  -m 4 -cpu 486 \
  -bios "$BIOS" \
  -drive id=hd0,file="$DISK",format=raw,if=none,snapshot=on \
  -device ide-hd,drive=hd0,bus=ide.0,unit=0,cyls=128,heads=2,secs=32 \
  -boot c -vga std \
  -device "pc110-chipset,biosfile=$BIOS,vgac000=on" \
  -device pc110-fontrom,romfile="$FONT" \
  -display cocoa,zoom-to-fit=on "$@"
