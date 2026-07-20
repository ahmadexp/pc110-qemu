/*
 * IBM PC110 chipset shim
 *
 * The PC110 palmtop uses a VLSI/SCAMP-style memory controller plus several
 * indexed I/O "extension" register files and a power-sense MCU.  The real
 * PC110 BIOS probes these during early POST; when they read back 0xFF (the
 * default for unmapped ports on a PC) the BIOS interprets fields as 255 and
 * spins in huge probe loops or halts.  Modelling them as benign indexed
 * register files (behaviour ported from the PC110-EMU reference emulator)
 * lets the real BIOS get through the chipset-detection phase of POST on
 * QEMU's i440fx platform.
 *
 * Port map (PC110-specific ports only; standard PIC/PIT/DMA/RTC/KBC are
 * provided by the QEMU PC platform):
 *
 *   0x4F         PC110 config latch (index)
 *   0x74 / 0x76  SCAMP/VLSI indexed register file (index / data)
 *   0xEC / 0xED  power-sense MCU indexed register file (index / data)
 *   0xEE / 0xEF  VL82C420 placeholder (reads 0xFF, writes ignored)
 *   0x15EA/0x15EB indexed extension register file (index / data)
 *   0x15E8/0x15EC settling status ports (busy bit clears after a few reads)
 *   0x35EA/0x35EB indexed extension register file (index / data)
 *
 * Usage:  -device pc110-chipset
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/isa/isa.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "qemu/module.h"
#include "qom/object.h"

/*
 * On the real PC110 the whole 256 KB firmware ROM decodes across
 * 0xC0000-0xFFFFF, so its integrated Chips&Technologies VGA BIOS sits at
 * 0xE0000 and the system BIOS at 0xF0000.  QEMU's PC layout instead maps only
 * the top 128 KB at 0xE0000-0xFFFFF and installs its own VGA BIOS at 0xC0000.
 * When biosfile= is given we overlay the full ROM across 0xC0000-0xFFFFF at
 * high priority, matching the real decode and hiding QEMU's competing VGA
 * BIOS, while -vga std still supplies the framebuffer + standard VGA ports the
 * real C&T VGA BIOS programs.
 */
#define PC110_BIOS_MAP_BASE 0xC0000
#define PC110_BIOS_MAP_SIZE 0x40000    /* 256 KiB -> 0xC0000..0xFFFFF */

#define TYPE_PC110_CHIPSET "pc110-chipset"
OBJECT_DECLARE_SIMPLE_TYPE(PC110ChipsetState, PC110_CHIPSET)

struct PC110ChipsetState {
    ISADevice parent_obj;

    /* optional full-ROM overlay at 0xC0000-0xFFFFF */
    char *biosfile;
    char *biospatch;   /* "off=hex,off=hex" byte patches applied to the ROM */
    char *vgac000;     /* if set: place the C&T VGA BIOS (ROM 0x20000) at 0xC0000 */
    MemoryRegion biosrom;   /* C0000-DFFFF read-only ROM (matches real POST) */
    MemoryRegion biosEram;  /* E0000-EFFFF ROM in POST, writable DOS UMB post-boot */
    MemoryRegion biosram;   /* F0000-FFFFF writable shadow RAM */

    /* DMA page registers 0x80-0x8F as plain R/W scratch (PC110 POST tests these) */
    uint8_t dma_page[16];

    /* config latch 0x4F + config-register data via 0x71 when armed */
    uint8_t config_index;
    bool    config_armed;
    uint8_t config_regs[256];

    /* CMOS/RTC 0x70/0x71 (seeded from real hardware) */
    uint8_t rtc_index;
    uint8_t cmos[128];

    /* SCAMP/VLSI 0x74/0x76 */
    uint8_t scamp_index;
    uint8_t scamp_regs[128];

    /* VL82C420 "block2" config window 0x24(index)/0x25(data): the POST/init
     * programming view (256 regs), seeded from a real-hardware dump.  Key:
     * block2[0xB8] bit3 = resume-from-suspend strap (0 = cold boot); leaving it
     * unmapped read 0xFF => bit3 set => the BIOS/driver takes the resume path. */
    uint8_t block2_index;
    uint8_t block2_regs[256];

    /* VL82C420 clock-stop / config-lock ports (STPCLK power path, chipset ref
     * 13j.8/13j.9): 0x22/0x23 config-lock, 0x302 & 0x704 clock-stop bits. */
    uint16_t reg22;
    uint16_t reg302;
    uint16_t reg704;

    /* power MCU 0xEC/0xED */
    uint8_t mcu_index;
    uint8_t mcu_regs[256];

    /* extension 0x35EA/0x35EB */
    uint8_t ext35_index;
    uint8_t ext35_regs[256];

    /* extension 0x15EA/0x15EB + status 0x15E8/0x15EC */
    uint8_t ext15_index;
    uint8_t ext15_regs[256];
    uint8_t status_15e8;
    uint8_t status_15ec;
    uint32_t status_15e8_reads;
    uint32_t status_15ec_reads;

    /* PCIC/PCMCIA-style indexed window 0x3E0(index)/0x3E1(data) */
    uint8_t pcic_index;
    uint8_t pcic_regs[256];

    /* SCAMP-style indexed window 0x3F0(index)/0x3F1(data) */
    uint8_t fdcw_index;
    uint8_t fdcw_regs[256];

    MemoryRegion io_80;
    MemoryRegion io_4f;
    MemoryRegion io_70;
    MemoryRegion io_74;
    MemoryRegion io_ec;
    MemoryRegion io_ee;
    MemoryRegion io_15e8;
    MemoryRegion io_35ea;
    MemoryRegion io_3e0;
    MemoryRegion io_3f0;
    MemoryRegion io_24;
    MemoryRegion io_22;
    MemoryRegion io_302;
    MemoryRegion io_704;
};

/*
 * Power-sense MCU indexed register file (0xEC index / 0xED data).  Plain R/W,
 * preloaded with the live-hardware dump — matching the oracle's indexed_ed[].
 * (No synthetic identity: the BIOS reads back the real config bytes.)
 */
static uint8_t mcu_indexed_value(PC110ChipsetState *s, uint8_t index)
{
    return s->mcu_regs[index];
}

/* ---- 0x80-0x8F DMA page registers as plain R/W scratch ---- */
static uint64_t dpage_read(void *o, hwaddr a, unsigned sz)
{
    return ((PC110ChipsetState *)o)->dma_page[a & 0x0F];
}
static void dpage_write(void *o, hwaddr a, uint64_t v, unsigned sz)
{
    ((PC110ChipsetState *)o)->dma_page[a & 0x0F] = v;
}

/* ---- 0x4F config latch: select config-register index and ARM config mode ---- */
static uint64_t cfg_read(void *o, hwaddr a, unsigned sz)
{
    return ((PC110ChipsetState *)o)->config_index;
}
static void cfg_write(void *o, hwaddr a, uint64_t v, unsigned sz)
{
    PC110ChipsetState *s = o;
    /*
     * 0x4F is a SEPARATE config-register index latch (the reference emulator's
     * pc110_config_index).  It only redirects 0x71 to the config bank while in
     * Easy-Setup mode (real_setup_mode==2), which normal POST never enters, so
     * during boot 0x71 is always ordinary CMOS.  It must NOT clobber rtc_index:
     * the CMOS access helper writes the index to both 0x70 and 0x4F, and it is
     * the 0x70 write that selects the CMOS byte.
     */
    s->config_index = v;
}

/* ---- 0x70/0x71 CMOS/RTC + config-register data (when 0x4F armed) ---- */
static uint8_t cmos_get(PC110ChipsetState *s, uint8_t idx)
{
    switch (idx & 0x7F) {
    case 0x0A: return s->cmos[0x0A] & 0x7F;   /* clear UIP */
    case 0x0C: return 0x00;
    case 0x0D: return 0x80;                    /* battery/RAM valid */
    default:   return s->cmos[idx & 0x7F];
    }
}
/*
 * Data port 0x71 routes to the config-register bank when the active index has
 * bit7 set (0x80-0xFF are the PC110 extended/config registers) OR when config
 * mode was armed via 0x4F; otherwise it is ordinary CMOS (index 0x00-0x7F).
 * The active index is the config index while armed, else the RTC index (0x70).
 */
/*
 * 0x71 is ordinary CMOS: index bit7 is the NMI-disable bit (masked off for the
 * CMOS address), NOT a bank selector.  The index is set by 0x70 and mirrored to
 * 0x4F on every access, so 0x71 always reads/writes CMOS[index & 0x7F].
 */
static uint64_t rtc_read(void *o, hwaddr a, unsigned sz)
{
    PC110ChipsetState *s = o;
    if (a == 0) {                             /* 0x70 index */
        return s->rtc_index;
    }
    uint8_t idx = s->rtc_index & 0x7F, val = cmos_get(s, idx);
    if (idx == 0x0F && getenv("PC110RSTLOG")) {
        fprintf(stderr, "[pc110chip] CMOS[0F] read  -> %02X\n", val);
    }
    return val;
}
static void rtc_write(void *o, hwaddr a, uint64_t v, unsigned sz)
{
    PC110ChipsetState *s = o;
    if (a == 0) {                             /* 0x70 index (bit7 = NMI disable) */
        s->rtc_index = v;
        return;
    }
    if ((s->rtc_index & 0x7F) == 0x0F && getenv("PC110RSTLOG")) {
        fprintf(stderr, "[pc110chip] CMOS[0F] write <- %02X\n", (uint8_t)v);
    }
    s->cmos[s->rtc_index & 0x7F] = v;
}

/* ---- 0x74/0x76 SCAMP ---- */
static uint64_t scamp_read(void *o, hwaddr a, unsigned sz)
{
    PC110ChipsetState *s = o;
    if (a == 0) {                             /* 0x74 index */
        return s->scamp_index;
    }
    if (a == 2) {                             /* 0x76 data */
        uint8_t idx = s->scamp_index & 0x7F;
        if (getenv("PC110SCAMPLOG")) {
            static uint32_t cnt[128];
            static uint8_t logged[128];
            if (++cnt[idx] == 100000 && !logged[idx]) {
                logged[idx] = 1;
                fprintf(stderr, "[scamp] index %02X polled 100000x, value=%02X\n",
                        idx, s->scamp_regs[idx]);
            }
        }
        return s->scamp_regs[idx];
    }
    return 0xFF;
}
static void scamp_write(void *o, hwaddr a, uint64_t v, unsigned sz)
{
    PC110ChipsetState *s = o;
    if (a == 0) {
        s->scamp_index = v & 0x7F;
    } else if (a == 2) {
        s->scamp_regs[s->scamp_index & 0x7F] = v;
    }
}

/* ---- 0xEC/0xED power MCU ---- */
static uint64_t mcu_read(void *o, hwaddr a, unsigned sz)
{
    PC110ChipsetState *s = o;
    if (a == 0) {                             /* 0xEC index */
        return s->mcu_index;
    }
    return mcu_indexed_value(s, s->mcu_index); /* 0xED data */
}
static void mcu_write(void *o, hwaddr a, uint64_t v, unsigned sz)
{
    PC110ChipsetState *s = o;
    if (a == 0) {
        s->mcu_index = v;
    } else {
        s->mcu_regs[s->mcu_index] = v;
    }
}

/* ---- 0xEE/0xEF VL82C420 placeholder ---- */
static uint64_t vl_read(void *o, hwaddr a, unsigned sz)
{
    return 0xFF;
}
static void vl_write(void *o, hwaddr a, uint64_t v, unsigned sz)
{
}

/* ---- 0x15E8..0x15EC ---- */
static uint64_t ext15_read(void *o, hwaddr a, unsigned sz)
{
    PC110ChipsetState *s = o;
    switch (a) {
    case 0x0:                                 /* 0x15E8 status */
        if (++s->status_15e8_reads > 8) {
            s->status_15e8 = 0x00;
        }
        return s->status_15e8;
    case 0x2:                                 /* 0x15EA index */
        return s->ext15_index;
    case 0x3:                                 /* 0x15EB data */
        return s->ext15_regs[s->ext15_index];
    case 0x4:                                 /* 0x15EC status */
        if (++s->status_15ec_reads > 8) {
            s->status_15ec &= (uint8_t)~0x02u; /* clear busy bit */
        }
        return s->status_15ec;
    }
    return 0xFF;
}
static void ext15_write(void *o, hwaddr a, uint64_t v, unsigned sz)
{
    PC110ChipsetState *s = o;
    switch (a) {
    case 0x2:
        s->ext15_index = v;
        break;
    case 0x3:
        s->ext15_regs[s->ext15_index] = v;
        break;
    }
}

/* ---- 0x35EA/0x35EB ---- */
static uint64_t ext35_read(void *o, hwaddr a, unsigned sz)
{
    PC110ChipsetState *s = o;
    if (a == 0) {
        return s->ext35_index;
    }
    return s->ext35_regs[s->ext35_index];
}
static void ext35_write(void *o, hwaddr a, uint64_t v, unsigned sz)
{
    PC110ChipsetState *s = o;
    if (a == 0) {
        s->ext35_index = v;
    } else {
        s->ext35_regs[s->ext35_index] = v;
    }
}

/* ---- 0x3E0/0x3E1 (PCMCIA ExCA) and 0x3F0/0x3F1 (below-FDC) ----
 * The reference emulator leaves all four ports UNMAPPED-equivalent: reads
 * return a fixed 0xFF and writes are dropped (0x3E0/0x3E1 = PCMCIA ExCA
 * placeholder; 0x3F0/0x3F1 are not the FDC — the FDC is 0x3F2-0x3F7).  We
 * still register 0x3F0/0x3F1 to shadow QEMU's FDC decode there so they read
 * 0xFF exactly like the real machine's unclaimed ports.  (The BIOS's
 * 0x55/0xAA presence probes here are informational; POST does not gate on the
 * readback — it boots on the real hardware with these reading 0xFF.) */
static uint64_t ff_read(void *o, hwaddr a, unsigned sz)
{
    return 0xFF;
}
static void ff_write(void *o, hwaddr a, uint64_t v, unsigned sz)
{
}

static const MemoryRegionOps pcic_ops = {
    .read = ff_read, .write = ff_write,
    .valid.min_access_size = 1, .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
static const MemoryRegionOps fdcw_ops = {
    .read = ff_read, .write = ff_write,
    .valid.min_access_size = 1, .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps dpage_ops = {
    .read = dpage_read, .write = dpage_write,
    .valid.min_access_size = 1, .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
static const MemoryRegionOps cfg_ops = {
    .read = cfg_read, .write = cfg_write,
    .valid.min_access_size = 1, .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
static const MemoryRegionOps rtc_ops = {
    .read = rtc_read, .write = rtc_write,
    .valid.min_access_size = 1, .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
static const MemoryRegionOps scamp_ops = {
    .read = scamp_read, .write = scamp_write,
    .valid.min_access_size = 1, .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
static const MemoryRegionOps mcu_ops = {
    .read = mcu_read, .write = mcu_write,
    .valid.min_access_size = 1, .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
static const MemoryRegionOps vl_ops = {
    .read = vl_read, .write = vl_write,
    .valid.min_access_size = 1, .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
static const MemoryRegionOps ext15_ops = {
    .read = ext15_read, .write = ext15_write,
    .valid.min_access_size = 1, .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
static const MemoryRegionOps ext35_ops = {
    .read = ext35_read, .write = ext35_write,
    .valid.min_access_size = 1, .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * E-segment (E0000-EFFFF) write policy.  On the real PC110 (and the PC110-EMU
 * reference) this window is READ-ONLY option/adapter-ROM space during POST, but
 * becomes a WRITABLE DOS Upper Memory Block once the OS has booted (the DOS
 * kernel/driver relocates code there and, e.g., a driver checksum routine writes
 * a byte back to E000:FFFF).  Modeled as a rom_device: reads are served directly
 * from the RAM backing (executable), writes come here.  Drop writes until boot
 * (so POST memory-sizing still classifies E as ROM); apply them afterwards.
 * pc110_booted is set by the INT19 handler in target/i386/pc110post.c. */
extern int pc110_booted;
static void eram_write(void *o, hwaddr a, uint64_t v, unsigned sz)
{
    PC110ChipsetState *s = o;
    if (!pc110_booted) {
        return; /* POST: E-segment is read-only ROM */
    }
    if (a + sz <= 0x10000) {
        uint8_t *ram = memory_region_get_ram_ptr(&s->biosEram);
        for (unsigned i = 0; i < sz; i++) {
            ram[a + i] = (uint8_t)(v >> (8 * i));
        }
        if (getenv("PC110RSTLOG")) {
            static unsigned ew;
            if (ew < 12 || (ew % 4096) == 0) {
                fprintf(stderr, "[pc110chip] E-seg write #%u  E000:%04X <- %02X\n",
                        ew, (unsigned)a, (unsigned)(v & 0xFF));
            }
            ew++;
        }
    }
}
static uint64_t eram_read(void *o, hwaddr a, unsigned sz)
{
    /* rom_device serves reads directly from the RAM backing; this is only a
     * fallback and should not normally be reached. */
    PC110ChipsetState *s = o;
    uint64_t r = 0;
    const uint8_t *ram = memory_region_get_ram_ptr(&s->biosEram);
    for (unsigned i = 0; i < sz && a + i < 0x10000; i++) {
        r |= (uint64_t)ram[a + i] << (8 * i);
    }
    return r;
}
static const MemoryRegionOps eram_ops = {
    .read = eram_read, .write = eram_write,
    .valid.min_access_size = 1, .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* ---- 0x24/0x25 VL82C420 "block2" config window (index/data) ----
 * The POST/init programming view of the chipset config (256 8-bit regs).  The
 * real four-read unlock gate (in FC23/F023/C023/0023) is not required here: we
 * always serve the seeded values, which is what the unlocked window returns.
 * Seeded from a real-hardware dump (Open-Source-PC110 block2_config.txt); the
 * load-bearing byte is block2[0xB8]=0x00 (bit3 clear => cold boot, not
 * resume-from-suspend). */
static uint64_t block2_read(void *o, hwaddr a, unsigned sz)
{
    PC110ChipsetState *s = o;
    if (a == 0) {                 /* 0x24 index */
        return s->block2_index;
    }
    if (getenv("PC110RSTLOG")) {
        static int n;
        if (n++ < 24) {
            fprintf(stderr, "[pc110chip] block2[%02X] read -> %02X\n",
                    s->block2_index, s->block2_regs[s->block2_index]);
        }
    }
    return s->block2_regs[s->block2_index];   /* 0x25 data */
}
static void block2_write(void *o, hwaddr a, uint64_t v, unsigned sz)
{
    PC110ChipsetState *s = o;
    if (a == 0) {                 /* 0x24 index */
        s->block2_index = (uint8_t)v;
    } else {                      /* 0x25 data */
        s->block2_regs[s->block2_index] = (uint8_t)v;
    }
}
static const MemoryRegionOps block2_ops = {
    .read = block2_read, .write = block2_write,
    .valid.min_access_size = 1, .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* ---- 0x22/0x23, 0x302, 0x704 clock-stop / config-lock (STPCLK path) ----
 * Modeled as plain 16-bit R/W latches so the BIOS/driver read-modify-write and
 * config-lock/clock-stop sequences see real values instead of an unmapped
 * 0xFFFF (which corrupts the RMW result).  We do NOT actually stop the clock. */
/* Per-port wrappers (each region is 2 bytes wide, little-endian). */
static uint64_t clk22_read(void *o, hwaddr a, unsigned sz)
{ PC110ChipsetState *s = o; return (s->reg22 >> (a * 8)) & ((sz == 1) ? 0xFF : 0xFFFF); }
static void clk22_write(void *o, hwaddr a, uint64_t v, unsigned sz)
{ PC110ChipsetState *s = o; if (sz == 1) { if (a) s->reg22 = (s->reg22 & 0x00FF) | ((v & 0xFF) << 8); else s->reg22 = (s->reg22 & 0xFF00) | (v & 0xFF); } else s->reg22 = v; }
static uint64_t clk302_read(void *o, hwaddr a, unsigned sz)
{ PC110ChipsetState *s = o; return (s->reg302 >> (a * 8)) & ((sz == 1) ? 0xFF : 0xFFFF); }
static void clk302_write(void *o, hwaddr a, uint64_t v, unsigned sz)
{ PC110ChipsetState *s = o; if (sz == 1) { if (a) s->reg302 = (s->reg302 & 0x00FF) | ((v & 0xFF) << 8); else s->reg302 = (s->reg302 & 0xFF00) | (v & 0xFF); } else s->reg302 = v; }
static uint64_t clk704_read(void *o, hwaddr a, unsigned sz)
{ PC110ChipsetState *s = o; return (s->reg704 >> (a * 8)) & ((sz == 1) ? 0xFF : 0xFFFF); }
static void clk704_write(void *o, hwaddr a, uint64_t v, unsigned sz)
{ PC110ChipsetState *s = o; if (sz == 1) { if (a) s->reg704 = (s->reg704 & 0x00FF) | ((v & 0xFF) << 8); else s->reg704 = (s->reg704 & 0xFF00) | (v & 0xFF); } else s->reg704 = v; }
static const MemoryRegionOps clk22_ops  = { .read = clk22_read,  .write = clk22_write,  .valid.min_access_size = 1, .valid.max_access_size = 2, .endianness = DEVICE_LITTLE_ENDIAN };
static const MemoryRegionOps clk302_ops = { .read = clk302_read, .write = clk302_write, .valid.min_access_size = 1, .valid.max_access_size = 2, .endianness = DEVICE_LITTLE_ENDIAN };
static const MemoryRegionOps clk704_ops = { .read = clk704_read, .write = clk704_write, .valid.min_access_size = 1, .valid.max_access_size = 2, .endianness = DEVICE_LITTLE_ENDIAN };

static void pc110_chipset_realizefn(DeviceState *d, Error **errp)
{
    ISADevice *isadev = ISA_DEVICE(d);
    PC110ChipsetState *s = PC110_CHIPSET(d);
    MemoryRegion *io = isa_address_space_io(isadev);

    s->status_15ec = 0x02;                    /* start "busy", settles on read */

    /*
     * Seed SCAMP/VLSI and power-MCU indexed registers with values read from
     * the live PC110 hardware, so POST's memory-sizing and config-detect loops
     * see real chipset state instead of zeros (which never satisfy the checks).
     */
    {
        /*
         * FULL 128-byte SCAMP (0x74/0x76) bank captured live from the real
         * PC110 over COMrade (2026-07-20).  The high indices hold the config
         * signature a Win95 driver's verify routine (F000:3A56) checks:
         * SCAMP[0x77]=0x4F, [0x7A]=0x53/[0x7B]=0x4C ('SL'=0x534C),
         * [0x7E]=0x15/[0x7F]=0xEE (must equal CMOS[0x7E/7F]), [0x0D]=0x93/
         * [0x0E]=0xE5.  With the old 32-byte seed the rest read 0, the verify
         * failed, and the driver spun forever.
         */
        static const uint8_t scamp_seed[0x80] = {
            0x00,0xBB,0x80,0x00,0xFF,0xFF,0xFF,0xFF,
            0xFF,0xFF,0xFF,0xFF,0xFF,0x93,0xE5,0x50,
            0x80,0x00,0x00,0x90,0xF0,0xE0,0xA1,0x00,
            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
            0x8E,0x02,0x00,0x00,0x00,0x00,0x00,0x00,
            0x00,0x00,0x00,0x00,0x00,0x00,0x98,0x8A,
            0x10,0x14,0x10,0x20,0x08,0xBA,0x9E,0xF1,
            0x5A,0x50,0xF1,0x3C,0x0A,0x1E,0x2C,0x01,
            0x02,0x05,0x01,0x88,0x03,0x00,0x00,0x00,
            0x00,0x0A,0x03,0x88,0x05,0x00,0x00,0x00,
            0x08,0x1E,0x11,0x88,0x11,0x00,0x00,0x00,
            0x0C,0x00,0x00,0x88,0x00,0x00,0x00,0x00,
            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
            0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x4F,
            0x0F,0x10,0x53,0x4C,0x00,0x10,0x15,0xEE,
        };
        static const uint8_t mcu_seed[0x20] = {
            0x42,0xd5,0x0b,0x00,0x06,0xa8,0x1a,0xec,
            0x38,0x00,0x03,0x00,0x29,0x00,0x00,0x2a,
            0x00,0x00,0xaa,0x55,0x55,0x6a,0x55,0x55,
            0xaa,0x1a,0x04,0x08,0x74,0x00,0x00,0x00,
        };
        /*
         * FULL 128-byte CMOS (0x70/0x71) bank captured live from the real
         * PC110 over COMrade (2026-07-20).  Matches the real machine exactly:
         *   0x15/16=0280 (640 KB base)   0x17/18=0C00 (3072 KB ext = 4 MB box)
         *   -> run QEMU with -m 4 (4096 KB total -> 3072 KB above 1 MB).
         * The driver verify (F000:3A56) also checks CMOS[0x0E].7 (=0, clear)
         * and CMOS[0x7E]=0x15/[0x7F]=0xEE (== SCAMP[0x7E/7F]).  0x00-0x09 are the
         * live clock (BCD) at capture time.
         */
        static const uint8_t cmos_seed[0x80] = {
            0x08,0x02,0x55,0x00,0x22,0x00,0x06,0x14,
            0x01,0x94,0x26,0x02,0x50,0x80,0x00,0x00,
            0x40,0x00,0xFF,0x00,0x25,0x80,0x02,0x00,
            0x0C,0x7F,0x7F,0x00,0x00,0x98,0x77,0x00,
            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
            0x00,0x00,0x00,0x00,0x00,0x00,0x03,0xFF,
            0x00,0x0C,0x19,0xFF,0x10,0xFF,0xFF,0xFF,
            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
            0xE6,0x87,0x1F,0x01,0x50,0x00,0x00,0x00,
            0x00,0x00,0x00,0x00,0x00,0x0F,0x07,0x32,
            0x17,0x1F,0x3A,0x2F,0x37,0x3F,0x47,0x2A,
            0x56,0x5E,0x6E,0x65,0x6F,0x6D,0x67,0x64,
            0x00,0x00,0xC7,0x0C,0x1F,0xFF,0xFF,0xFF,
            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
            0x00,0x00,0x10,0x48,0xA6,0x00,0x00,0x00,
            0x00,0x00,0x32,0x02,0x04,0x2B,0x15,0xEE,
        };
        static const uint8_t ext35_seed[0x20] = {
            0x00,0xff,0xf5,0xff,0x87,0xf3,0x6c,0xfd,
            0xff,0xf7,0xff,0xfc,0xff,0xff,0xff,0xff,
            0xff,0xff,0xff,0xe8,0x35,0x04,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        };
        /* VL82C420 block2 (0x24/0x25) live dump — Open-Source-PC110
         * block2_config.txt (four-read-unlock capture, 2026-07-20).  block2[0xB8]
         * = 0x00 (bit3 clear => cold boot) is the load-bearing value. */
        static const uint8_t block2_seed[0x100] = {
            0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0x40,0x41,0x42,0x43,0xFF,0xFF,0x00,0x0F,
            0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
            0xFF,0xFF,0x11,0x08,0x04,0x01,0x00,0x20,0x0B,0x0C,0x00,0x08,0xFF,0xFF,0x80,0xFF,
            0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
            0x00,0x00,0x12,0x00,0x50,0x05,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0xFF,
            0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
            0x00,0x01,0x00,0xFF,0xFF,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
            0x02,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
            0xFF,0xFF,0xFF,0xFF,0xEE,0x15,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
            0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xC0,0x41,0x42,0x43,0xAA,0xAA,0x00,0x0E,
            0xFF,0xFF,0x11,0x70,0x02,0x01,0x00,0x20,0x0B,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
            0xBA,0x9E,0xF0,0x5A,0x50,0xF5,0xDA,0x00,0x00,0x40,0x00,0x00,0x02,0xFF,0x10,0x14,
            0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xF2,0x03,0xA1,0x02,0xFF,0xFF,0xFF,0xFF,
            0x60,0x00,0x24,0x00,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x20,0x00,0xFF,0xFF,0xFF,0xFF,
            0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xF4,0x03,0xA1,0x02,0xFF,0xFF,0xFF,0xFF,
            0x80,0x00,0x00,0x00,0x10,0x20,0x08,0x07,0x0E,0xFF,0xFF,0x18,0xE0,0xFF,0x3F,0x00,
        };
        memcpy(s->scamp_regs, scamp_seed, sizeof(scamp_seed));
        memcpy(s->mcu_regs, mcu_seed, sizeof(mcu_seed));
        memcpy(s->cmos, cmos_seed, sizeof(cmos_seed));
        memcpy(s->ext35_regs, ext35_seed, sizeof(ext35_seed));
        memcpy(s->block2_regs, block2_seed, sizeof(block2_seed));
    }

    /* 0x80-0x8F: override QEMU's partial DMA page decode with full R/W scratch */
    memory_region_init_io(&s->io_80, OBJECT(d), &dpage_ops, s, "pc110-dmapage", 16);
    memory_region_add_subregion_overlap(io, 0x80, &s->io_80, 10);

    memory_region_init_io(&s->io_4f, OBJECT(d), &cfg_ops, s, "pc110-cfg", 1);
    memory_region_add_subregion(io, 0x4F, &s->io_4f);

    /* 0x70/0x71: CMOS/RTC + config-register data — override QEMU's mc146818 */
    memory_region_init_io(&s->io_70, OBJECT(d), &rtc_ops, s, "pc110-rtc", 2);
    memory_region_add_subregion_overlap(io, 0x70, &s->io_70, 10);

    /* 0x74..0x76 (index at 0x74, data at 0x76) */
    memory_region_init_io(&s->io_74, OBJECT(d), &scamp_ops, s, "pc110-scamp", 3);
    memory_region_add_subregion(io, 0x74, &s->io_74);

    memory_region_init_io(&s->io_ec, OBJECT(d), &mcu_ops, s, "pc110-mcu", 2);
    memory_region_add_subregion(io, 0xEC, &s->io_ec);

    memory_region_init_io(&s->io_ee, OBJECT(d), &vl_ops, s, "pc110-vl82c420", 2);
    memory_region_add_subregion(io, 0xEE, &s->io_ee);

    /* 0x15E8..0x15EC */
    memory_region_init_io(&s->io_15e8, OBJECT(d), &ext15_ops, s, "pc110-ext15", 5);
    memory_region_add_subregion(io, 0x15E8, &s->io_15e8);

    /* 0x35EA..0x35EB */
    memory_region_init_io(&s->io_35ea, OBJECT(d), &ext35_ops, s, "pc110-ext35", 2);
    memory_region_add_subregion(io, 0x35EA, &s->io_35ea);

    /* 0x3E0/0x3E1 PCIC-style window (unclaimed by default PC -> add plainly) */
    memory_region_init_io(&s->io_3e0, OBJECT(d), &pcic_ops, s, "pc110-pcic", 2);
    memory_region_add_subregion_overlap(io, 0x3E0, &s->io_3e0, 10);

    /* 0x3F0/0x3F1 SCAMP-style window (override default FDC decode at 0x3F0) */
    memory_region_init_io(&s->io_3f0, OBJECT(d), &fdcw_ops, s, "pc110-fdcw", 2);
    memory_region_add_subregion_overlap(io, 0x3F0, &s->io_3f0, 10);

    /* 0x24/0x25 VL82C420 block2 config window (POST/init programming view) */
    memory_region_init_io(&s->io_24, OBJECT(d), &block2_ops, s, "pc110-block2", 2);
    memory_region_add_subregion_overlap(io, 0x24, &s->io_24, 10);

    /* clock-stop / config-lock latches (STPCLK power path) */
    memory_region_init_io(&s->io_22, OBJECT(d), &clk22_ops, s, "pc110-clk22", 2);
    memory_region_add_subregion_overlap(io, 0x22, &s->io_22, 10);
    memory_region_init_io(&s->io_302, OBJECT(d), &clk302_ops, s, "pc110-clk302", 2);
    memory_region_add_subregion_overlap(io, 0x302, &s->io_302, 10);
    memory_region_init_io(&s->io_704, OBJECT(d), &clk704_ops, s, "pc110-clk704", 2);
    memory_region_add_subregion_overlap(io, 0x704, &s->io_704, 10);
    s->reg22 = 0x0100;   /* config-lock bit8 set (locked) at rest */
    s->reg302 = 0x0000;
    s->reg704 = 0x0000;

    /* optional full-ROM overlay at 0xC0000-0xFFFFF */
    if (s->biosfile) {
        gchar *contents;
        gsize len;
        GError *gerr = NULL;

        if (!g_file_get_contents(s->biosfile, &contents, &len, &gerr)) {
            error_setg(errp, "pc110-chipset: cannot read biosfile %s: %s",
                       s->biosfile, gerr->message);
            g_error_free(gerr);
            return;
        }
        if (len != PC110_BIOS_MAP_SIZE) {
            error_setg(errp, "pc110-chipset: biosfile %s is %zu bytes, "
                       "expected %d", s->biosfile, (size_t)len,
                       PC110_BIOS_MAP_SIZE);
            g_free(contents);
            return;
        }
        /* apply "off=hex,off=hex" byte patches to the ROM image */
        if (s->biospatch && s->biospatch[0]) {
            gchar **items = g_strsplit(s->biospatch, ",", 0);
            for (int i = 0; items[i]; i++) {
                gchar **kv = g_strsplit(items[i], "=", 2);
                if (kv[0] && kv[1]) {
                    unsigned long off = strtoul(kv[0], NULL, 16);
                    const char *h = kv[1];
                    size_t n = strlen(h) / 2;
                    for (size_t b = 0; b < n && (off + b) < PC110_BIOS_MAP_SIZE; b++) {
                        char byte[3] = { h[2 * b], h[2 * b + 1], 0 };
                        contents[off + b] = (char)strtoul(byte, NULL, 16);
                    }
                }
                g_strfreev(kv);
            }
            g_strfreev(items);
        }

        /*
         * The PC110 BIOS initializes video by 'call far C000:0003' (F000:3F67
         * / F000:4293), i.e. it expects the C&T VGA BIOS at 0xC0000 (the
         * standard VGA option-ROM location).  A flat 1:1 map puts the VGA BIOS
         * (ROM offset 0x20000) at 0xE0000 instead, so the call hits garbage.
         * When vgac000 is set, copy the 64KB VGA BIOS to the 0xC0000 window so
         * the video-init far-call reaches it.
         */
        if (s->vgac000 && s->vgac000[0]) {
            memcpy(contents + 0x00000, contents + 0x20000, 0x10000);
        }

        /*
         * Memory-window policy MUST match the real PC110 POST (verified against
         * the PC110-EMU reference): during POST only F0000-FFFFF is writable
         * shadow RAM (the BIOS relocates and patches POST tables there); the
         * whole C0000-EFFFF window is READ-ONLY ROM.  This is load-bearing: the
         * BIOS memory-sizing/test walks the top of memory and writes probe
         * patterns across C/D/E; on the real machine (and the reference) those
         * writes are IGNORED (ROM), so the sizer correctly classifies C/D/E as
         * ROM and moves on.  If C/D/E were writable, the probes succeed, the
         * E-segment ROM code (e.g. the E900 delay routine reached by a later
         * CALL FAR E900:xxxx) gets overwritten, and the memory test spins
         * forever re-running against a corrupted E-segment.
         *
         * C0000-DFFFF: read-only ROM (128 KiB).
         * E0000-EFFFF: rom_device (64 KiB) — read-only ROM during POST, writable
         *              DOS UMB after boot (see eram_ops / eram_write above).
         * F0000-FFFFF: writable RAM shadow (64 KiB), pre-loaded from the ROM.
         */
        memory_region_init_rom(&s->biosrom, OBJECT(d), "pc110-bios-crom",
                               0x20000, &error_fatal);
        memcpy(memory_region_get_ram_ptr(&s->biosrom), contents, 0x20000);
        memory_region_add_subregion_overlap(get_system_memory(),
                                            PC110_BIOS_MAP_BASE,
                                            &s->biosrom, 20);

        memory_region_init_rom_device(&s->biosEram, OBJECT(d), &eram_ops, s,
                                      "pc110-bios-eram", 0x10000, &error_fatal);
        memcpy(memory_region_get_ram_ptr(&s->biosEram), contents + 0x20000,
               0x10000);
        memory_region_add_subregion_overlap(get_system_memory(),
                                            PC110_BIOS_MAP_BASE + 0x20000,
                                            &s->biosEram, 20);

        memory_region_init_ram(&s->biosram, OBJECT(d), "pc110-bios-fram",
                               0x10000, &error_fatal);
        memcpy(memory_region_get_ram_ptr(&s->biosram), contents + 0x30000,
               0x10000);
        memory_region_add_subregion_overlap(get_system_memory(),
                                            PC110_BIOS_MAP_BASE + 0x30000,
                                            &s->biosram, 20);
        g_free(contents);
    }
}

static const VMStateDescription vmstate_pc110_chipset = {
    .name = TYPE_PC110_CHIPSET,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(config_index, PC110ChipsetState),
        VMSTATE_UINT8(scamp_index, PC110ChipsetState),
        VMSTATE_UINT8_ARRAY(scamp_regs, PC110ChipsetState, 128),
        VMSTATE_UINT8(mcu_index, PC110ChipsetState),
        VMSTATE_UINT8_ARRAY(mcu_regs, PC110ChipsetState, 256),
        VMSTATE_UINT8(ext35_index, PC110ChipsetState),
        VMSTATE_UINT8_ARRAY(ext35_regs, PC110ChipsetState, 256),
        VMSTATE_UINT8(ext15_index, PC110ChipsetState),
        VMSTATE_UINT8_ARRAY(ext15_regs, PC110ChipsetState, 256),
        VMSTATE_UINT8(status_15e8, PC110ChipsetState),
        VMSTATE_UINT8(status_15ec, PC110ChipsetState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property pc110_chipset_properties[] = {
    DEFINE_PROP_STRING("biosfile", PC110ChipsetState, biosfile),
    DEFINE_PROP_STRING("biospatch", PC110ChipsetState, biospatch),
    DEFINE_PROP_STRING("vgac000", PC110ChipsetState, vgac000),
};

static void pc110_chipset_class_initfn(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pc110_chipset_realizefn;
    dc->vmsd = &vmstate_pc110_chipset;
    dc->desc = "IBM PC110 chipset/MCU shim";
    device_class_set_props(dc, pc110_chipset_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo pc110_chipset_info = {
    .name          = TYPE_PC110_CHIPSET,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(PC110ChipsetState),
    .class_init    = pc110_chipset_class_initfn,
};

static void pc110_chipset_register_types(void)
{
    type_register_static(&pc110_chipset_info);
}

type_init(pc110_chipset_register_types)
