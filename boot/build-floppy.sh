#!/bin/sh
# Build the Easy-Setup boot floppy:
#   1. unpack Easy-Setup from the PC110 BIOS ROM
#   2. assemble the boot loader (needs nasm)
#   3. lay both onto a 1.44MB floppy image (boot sector + image at LBA 1)
#
# Output: disks/easysetup-floppy.img
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

BIOS="$ROOT/roms/pc110_bios.bin"
[ -f "$BIOS" ] || { echo "error: missing $BIOS (see roms/README.md)"; exit 1; }
command -v nasm >/dev/null || { echo "error: nasm not found (brew install nasm)"; exit 1; }

echo ">> unpacking Easy-Setup from BIOS ROM ..."
python3 "$ROOT/tools/unpack-easysetup.py" "$BIOS" "$ROOT/boot/easysetup.bin"

echo ">> assembling boot loader ..."
nasm -f bin -o "$ROOT/boot/setupboot-floppy.bin" "$ROOT/boot/setupboot-floppy.asm"

echo ">> building floppy image ..."
python3 - "$ROOT" <<'PY'
import sys
root = sys.argv[1]
boot = open(f"{root}/boot/setupboot-floppy.bin", "rb").read()
img  = open(f"{root}/boot/easysetup.bin", "rb").read()
assert len(boot) == 512, "boot sector must be 512 bytes"
data = bytearray(1474560)          # 1.44 MB
data[0:512] = boot
data[512:512 + len(img)] = img
open(f"{root}/disks/easysetup-floppy.img", "wb").write(data)
print(f"   easysetup-floppy.img: image {len(img)} bytes ({(len(img)+511)//512} sectors)")
PY
echo ">> done: disks/easysetup-floppy.img"
