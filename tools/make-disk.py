#!/usr/bin/env python3
"""
Turn a raw PC110 FAT16 *partition* dump into a bootable *disk* image.

The PC110 flash is usually dumped as a bare partition (the FAT16 volume with no
MBR).  Its BPB records 32 hidden sectors and a 2-head / 32-sector geometry, so
booting the dump directly fails ("Non-System disk").  This prepends a small MBR
bootstrap plus the hidden track, producing a disk QEMU can boot.

Usage: make-disk.py <partition-dump.img> <disk-out.img>
"""
import struct
import sys

HIDDEN = 32           # sectors before the partition (from the BPB)
HEADS, SPT = 2, 32    # PC110 flash geometry (from the BPB)


def chs(c, h, s):
    return bytes([h, ((c >> 2) & 0xC0) | (s & 0x3F), c & 0xFF])


def main(src_path, dst_path):
    part = open(src_path, "rb").read()
    if part[510:512] != b"\x55\xAA":
        sys.exit("error: source has no 0x55AA boot signature (not a FAT partition dump?)")
    part_sectors = len(part) // 512

    # MBR bootstrap: relocate to 0x0600, load the VBR from CHS(0,1,1) to 0x7C00,
    # point DS:SI at the relocated partition entry, jump into the VBR.
    code = bytes([
        0xFA, 0x33, 0xC0, 0x8E, 0xD0, 0xBC, 0x00, 0x7C, 0x8E, 0xD8, 0x8E, 0xC0,
        0xFB, 0xFC, 0xBE, 0x00, 0x7C, 0xBF, 0x00, 0x06, 0xB9, 0x00, 0x01, 0xF3,
        0xA5, 0xEA, 0x1E, 0x06, 0x00, 0x00,
        0xB8, 0x01, 0x02, 0xBB, 0x00, 0x7C, 0xB9, 0x01, 0x00, 0xB6, 0x01, 0xB2,
        0x80, 0xCD, 0x13, 0x72, 0xFE, 0xBE, 0xBE, 0x07, 0xEA, 0x00, 0x7C, 0x00,
        0x00,
    ])
    mbr = bytearray(512)
    mbr[:len(code)] = code

    end_lba = HIDDEN + part_sectors - 1
    ec = end_lba // (HEADS * SPT)
    eh = (end_lba // SPT) % HEADS
    es = (end_lba % SPT) + 1
    entry = b"\x80" + chs(0, 1, 1) + b"\x04" + chs(ec, eh, es) \
        + struct.pack("<II", HIDDEN, part_sectors)
    mbr[0x1BE:0x1BE + 16] = entry
    mbr[0x1FE:0x200] = b"\x55\xAA"

    with open(dst_path, "wb") as f:
        f.write(bytes(mbr))
        f.write(bytes(512 * (HIDDEN - 1)))  # rest of the hidden track
        f.write(part)

    total = HIDDEN + part_sectors
    print(f"wrote {dst_path}: {total} sectors "
          f"(CHS {total // (HEADS * SPT)}/{HEADS}/{SPT}), partition at LBA {HIDDEN}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
