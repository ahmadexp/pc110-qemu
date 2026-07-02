# ROMs

Place your **legally obtained** IBM PC110 ROM dumps here. They are copyrighted
IBM material and are **not** included in this repository.

| File | Size | Purpose |
| --- | --- | --- |
| `pc110_bios.bin` | 262144 (256 KiB) | PC110 system BIOS. Easy-Setup is extracted from it. |
| `pc110-fontrom.bin` | 1048576 (1 MiB) | PC110 Japanese kanji font ROM (FRU 84G7940). Used by Personaware and the `pc110-fontrom` device. |

The font ROM can be dumped from a real PC110 through its banked I/O window
(port `0x1160` = 8 KiB bank select, window at `0xDE000`), 128 banks for 1 MiB.

Sanity check (optional):

```
# BIOS: reset vector 'EA 5B E0 00 F0' at 0x3FFF0; "COPR. IBM" near 0x3E000
# Font ROM: header 55 AA .. 'FONT' at offset 0x0E, "84G7940" at 0x80
```
