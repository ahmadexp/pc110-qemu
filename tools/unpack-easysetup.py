#!/usr/bin/env python3
"""
Extract the PC110 Easy-Setup program from a PC110 BIOS ROM image.

Easy-Setup is stored LZW-compressed (variable 9-12 bit codes, clear=0x100,
end=0x101) starting at ROM offset 0x2BF, running up to 0x20000.  It unpacks to
a raw real-mode program that the BIOS loads at physical 0x50000 and enters at
5000:0000.

Usage: unpack-easysetup.py <pc110_bios.bin> <easysetup.bin>
"""
import sys


def main(rom_path, out_path):
    rom = open(rom_path, "rb").read()
    if not (rom[0] == 0x55 and rom[1] == 0xAA and rom[3] == 0xEB and rom[4] == 0x45):
        sys.exit("error: does not look like a PC110 BIOS image")
    src = rom[0x02BF:0x20000]

    def read_code(bitpos, bits):
        bp = bitpos // 8
        off = bitpos & 7
        if bp + 3 > len(src):
            return -1, bitpos
        v = src[bp] | (src[bp + 1] << 8) | (src[bp + 2] << 16)
        v >>= off
        return v & ((1 << bits) - 1), bitpos + bits

    def expand(code, next_code, prefix, suffix):
        st = []
        while code >= 0x100:
            if code >= next_code or len(st) >= 4096:
                return None
            st.append(suffix[code])
            code = prefix[code]
        st.append(code & 0xFF)
        return st

    out = bytearray()
    prefix = [0] * 4096
    suffix = [0] * 4096
    bits, next_code, grow_at, old, first, bitpos = 9, 0x102, 0x200, -1, 0, 0
    while True:
        code, bitpos = read_code(bitpos, bits)
        if code < 0:
            sys.exit(f"error: unexpected end at bitpos {bitpos}, out {len(out)}")
        if code == 0x101:
            break
        if code == 0x100:
            bits, next_code, grow_at, old = 9, 0x102, 0x200, -1
            code, bitpos = read_code(bitpos, bits)
            if code < 0 or code >= 0x100:
                sys.exit("error: bad clear code")
            out.append(code)
            old = first = code
            continue
        if code < next_code:
            st, special = expand(code, next_code, prefix, suffix), False
        elif code == next_code and old >= 0:
            st, special = expand(old, next_code, prefix, suffix), True
        else:
            sys.exit(f"error: bad code {code}/{next_code}")
        if st is None:
            sys.exit("error: expand failed")
        cur_first = st[-1]
        out.extend(reversed(st))
        if special:
            out.append(first)
            cur_first = first
        if old >= 0 and next_code < 4096:
            prefix[next_code] = old
            suffix[next_code] = cur_first
            next_code += 1
            if next_code >= grow_at and bits < 12:
                bits += 1
                grow_at <<= 1
        old, first = code, cur_first

    open(out_path, "wb").write(out)
    print(f"unpacked {len(out)} bytes -> {out_path}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
