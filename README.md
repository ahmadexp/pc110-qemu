# PC110-QEMU

Run the IBM Palm Top PC 110's software on QEMU using the real machine ROMs:
**IBM Personaware** (the Japanese pen GUI) boots with genuine kanji rendering —
now including straight off the **real 256 KiB PC110 BIOS**, all the way to the
pen desktop.

![Personaware on the real PC110 BIOS](docs/personaware-realbios.png)
![Easy-Setup menu](docs/easy-setup-menu.png)

This repository is a small set of QEMU device models plus build/run tooling. It
does **not** contain any IBM ROMs or disk images — you supply your own legally
obtained dumps (see [`roms/`](roms/README.md) and [`disks/`](disks/README.md)).

## What works

- **Personaware on the real PC110 BIOS** (`scripts/run-realbios.sh`) — the
  genuine 256 KiB ROM POSTs, boots DOS, runs the RIOS driver stack, hands off to
  VGA mode 12h and paints the full pen launcher with kanji from the font ROM.
  See [Booting the real BIOS](#booting-the-real-bios) for how, and the note there
  about running a `CONFIG.SYS` without EMM386.
- **Personaware on SeaBIOS** boots to the same full pen-driven launcher
  (Schedule, ToDo, Notebook, Address, E-Mail, FAX, Telephone, IR Connect, World
  Clock, Calculator, Editor, Draw Memo, Game, Personal, DOS, Power MGT) with
  sharp kanji from the PC110 font ROM. The disk is a 4 MB FAT12 image (matching
  the real unit's internal storage) so it boots straight to a clean launcher — a
  disk with 512-byte clusters trips a bogus "low disk space" dialog regardless of
  how much is free (see [`disks/`](disks/README.md)).
- **Easy-Setup** (the real graphical F1 BIOS setup: Config / Date-Time /
  Password / Start up / Test / Restart) runs under SeaBIOS and renders from the
  genuine BIOS ROM. Exiting it returns to normal Personaware mode.

## How it works

The PC110's own 256 KB BIOS cannot POST on QEMU's i440fx (it drives a custom
VLSI/SCAMP chipset and a Chips & Technologies flat-panel VGA), so this project
takes two complementary approaches:

- **Personaware** boots via SeaBIOS on QEMU's fast, correct x86 core. The one
  piece it needs from the real machine — the hardware **kanji font ROM** — is
  emulated by the `pc110-fontrom` device (an 8 KB banked window at `0xDE000`,
  bank-select at port `0x1160`), so the DOS/V font subsystem initializes and the
  GUI renders Japanese text.
- **Easy-Setup** is a self-contained graphical program embedded (LZW-compressed)
  in the BIOS ROM. It is extracted, loaded to `0x50000`, and entered at
  `5000:0000` exactly as the real BIOS does. It draws with standard VGA
  (mode 12h), which QEMU renders directly — no PC110 POST required.

### Device models (`qemu/hw-misc/`)

| Device | I/O | Purpose |
| --- | --- | --- |
| `pc110-fontrom` | `0x1160`–`0x1163`, mem `0xDE000` | Banked 1 MiB kanji font ROM window |
| `pc110-chipset` | `0x4F`, `0x74/76`, `0xEC/ED`, `0x15E8`, `0x35EA`, `0x80`–`0x8F` | VLSI/SCAMP + power-MCU shim, optional full-ROM shadow overlay (for experiments booting the real BIOS) |

> Easy-Setup needs **no custom device** — it runs on stock QEMU/SeaBIOS. Which
> screen it shows depends on the **boot device type**: booted as a floppy it
> shows the config menu; booted as a hard disk it shows its built-in
> hardware-diagnostics page. (An earlier `pc110-setupcfg` device that tried to
> drive the menu via config registers was a dead end and has been removed.) A
> minimal, dependency-free variant of just the Easy-Setup path lives in the
> companion repo **[pc110-easysetup-seabios](https://github.com/ahmadexp/pc110-easysetup-seabios)**,
> including USB/CompactFlash boot media for real hardware (e.g. Vortex86 boards).

### Easy-Setup exit path (`boot/`)

The `setupboot-floppy.asm` loader boots Easy-Setup from a floppy and makes
"Exit"/"Restart" return to normal Personaware mode three ways: a far-return exit
stub, a hooked `INT 19h` vector, and QEMU's `-boot once=a` (so a hardware reset
falls through to the hard disk).

## Requirements

- QEMU 11.0.2 build deps (a C toolchain, `meson`, `ninja`, `glib`, `pixman`).
- `nasm` (to assemble the Easy-Setup boot loader).
- `python3`.
- macOS on Apple Silicon is the primary tested host; `scripts/build-qemu.sh`
  pins the `/opt/homebrew` toolchain there. Linux should work with the default
  configure flags.

## Setup

```sh
# 1. Put your ROM dumps in place  (see roms/README.md)
#    roms/pc110_bios.bin        (256 KiB system BIOS)
#    roms/pc110-fontrom.bin     (1 MiB kanji font ROM)

# 2. Build QEMU with the PC110 devices
./scripts/build-qemu.sh

# 3. Prepare disks  (see disks/README.md)
#    Personaware (from a raw partition dump):
python3 tools/make-disk.py your-pc110-dump.img disks/Personaware-disk.img
#    Easy-Setup boot floppy (unpacked from the BIOS ROM):
./boot/build-floppy.sh

# 4. Run
./scripts/run-personaware.sh     # Personaware GUI (SeaBIOS)
./scripts/run-easysetup.sh       # Easy-Setup BIOS (Exit -> Personaware)
./scripts/run-realbios.sh        # boot the REAL BIOS (experimental, see below)
```

The QEMU window opens at 2x the guest resolution (the PC110 screen is tiny on a
high-DPI/Retina Mac); set `QEMU_COCOA_SCALE=N` to change it (1 = native). It is
still freely resizable — drag a corner, or press Ctrl+Cmd+F for full screen.
Quit with Ctrl+Cmd+Q.

## Booting the real BIOS

`scripts/run-realbios.sh` boots the genuine 256 KiB PC110 BIOS on QEMU instead
of SeaBIOS, and **reaches the full Personaware pen desktop** — POST, DOS, the
RIOS driver stack, the mode-12h video handoff, and the launcher, all driven by
the real ROM (screenshot at the top of this README).

- **POST completes** — memory sizing, chipset self-tests, timer/refresh
  calibration, the KBC warm-reset state machine.
- **DOS boots** — a software `INT 19h`/`INT 13h` service loads the boot sector
  and services disk I/O from the image; the PC-DOS kernel and the `CONFIG.SYS`
  driver stack (HIMEM, the RIOS `$FONT`/`$DISP`/`$IAS` drivers, `POWER.EXE`,
  `IBMMKKV`) plus `AUTOEXEC.BAT` (`KEYB`, `MOUSE`, `INKDRV`, `PW.BAT`) run.
- **Personaware renders** — `MET.COM` switches to VGA mode 12h (640×480×16,
  which QEMU's stock `-vga std` draws directly) and paints the launcher with
  kanji from the font ROM.

### The two pieces that make it work

1. **Loose protected mode** (`qemu/patches/05-seg-helper-loose-pm.patch`). The
   PC110 BIOS + drivers run long stretches in *unreal mode* — `CR0.PE=1` while
   loading real-mode-style segment values (`F000`, VGA selectors, …). A faithful
   x86 core (QEMU) `#GP`s on every such segment op, producing an endless
   general-protection fault storm. Post-boot, under `PC110POST`, QEMU's PM
   segment helpers (`load_seg`, far `jmp`/`call`/`ret`/`iret`, interrupt
   delivery) instead do **base-only resolution with no type/privilege/present
   checks** — using a descriptor's real base when its access byte is non-zero and
   `selector<<4` otherwise, and delivering interrupts same-privilege so frames
   stay balanced. This mirrors the reference emulator **PC110-EMU**'s
   `pc110_segment_base_for_selector`. Pre-boot POST is untouched (faithful PM).
2. **No EMM386.** EMM386's V86/paging monitor assumes *faithful* PM and conflicts
   irreconcilably with the driver's unreal-mode segment usage. Removing it from
   `CONFIG.SYS` (see [`disks/`](disks/README.md)) leaves the driver's loose PM as
   the only regime, and the boot runs clean to the desktop. (EMM386 support is
   future work — it would need a full V86-aware loose-PM model.)

The `pc110-chipset` device models the VL82C420's config windows against the
live-hardware register maps in the **Open-Source-PC110** project's
[`Discovery/Chipset`](https://github.com/ahmadexp/Open-Source-PC110/tree/main/Discovery/Chipset):
the SCAMP window (`0x74/0x76`), the **block2** POST/init window (`0x24/0x25`,
seeded from the live dump), and the clock-stop / config-lock latches
(`0x22/0x23`, `0x302`, `0x704`).

How it works (`qemu/target-i386/pc110post.c` + `qemu/patches/`):

- A TCG-level completer hooked into the CPU exec loop (enabled by the
  `PC110POST` env var) short-circuits POST wait-loops that never converge under
  emulation, seeds the warm-boot contract around the KBC CPU-reset, and services
  `INT 19h`/`INT 13h` from `$PC110BOOT`.
- The KBC `0xFE` reset is a synchronous **CPU-only** reset (RAM preserved)
  instead of QEMU's async full-machine reset, matching the 286-era
  protected-mode-exit idiom the BIOS/driver rely on.
- `pc110-chipset` supplies the ROM/shadow map (C0000-DFFFF ROM, E0000-EFFFF a
  DOS UMB that becomes writable after boot, F0000-FFFFF shadow RAM) and the
  VLSI/SCAMP + CMOS register banks seeded from a real-hardware dump.

Set `PC110RSTLOG=1` for verbose reset/driver tracing; `PC110HEARTBEAT=1` samples
where non-BIOS code executes; `PC110RESUME=1` re-enables the legacy A6E4
reset-resume shim (superseded by loose PM).

## Status / limitations

- **Personaware** (both the real-BIOS and SeaBIOS paths) and **Easy-Setup** all
  reach their GUIs.
- The **real-BIOS** path reaches the Personaware desktop with a `CONFIG.SYS` that
  does not load EMM386 (see [Booting the real BIOS](#booting-the-real-bios)).
  Booting the stock EMM386 configuration on the real BIOS is future work (needs a
  V86-aware loose-PM model).
- Easy-Setup renders and its menu is navigable, but it still calls a few PC110
  BIOS service routines that don't exist under SeaBIOS, so some in-menu actions
  may not fully function; entering it and exiting back to Personaware work.

## Credits

Chipset register behavior, the font-ROM window protocol, and the Easy-Setup LZW
container format were determined with reference to the **PC110-EMU** project
(github.com/ahmadexp/PC110-EMU), a dedicated PC110 emulator.
