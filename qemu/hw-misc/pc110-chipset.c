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
    MemoryRegion biosmap;

    /* DMA page registers 0x80-0x8F as plain R/W scratch (PC110 POST tests these) */
    uint8_t dma_page[16];

    /* config latch 0x4F */
    uint8_t config_index;

    /* SCAMP/VLSI 0x74/0x76 */
    uint8_t scamp_index;
    uint8_t scamp_regs[128];

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

    MemoryRegion io_80;
    MemoryRegion io_4f;
    MemoryRegion io_74;
    MemoryRegion io_ec;
    MemoryRegion io_ee;
    MemoryRegion io_15e8;
    MemoryRegion io_35ea;
};

/*
 * Power-sense MCU indexed value.  The real BIOS reads an identity block; we
 * report a "present" MCU with a small synthetic firmware id, mirroring the
 * PC110-EMU model closely enough to satisfy the presence checks.
 */
static const char MCU_FW_ID[] = "PC110-PMCU";

static uint8_t mcu_indexed_value(PC110ChipsetState *s, uint8_t index)
{
    if (index >= 0x80 && index < 0xE0) {
        size_t off = index - 0x80;
        return off < strlen(MCU_FW_ID) ? (uint8_t)MCU_FW_ID[off] : 0x00;
    }
    switch (index) {
    case 0xF0: return 'M';
    case 0xF1: return 'C';
    case 0xF2: return 'U';
    case 0xF3: return 0x08;                 /* firmware revision */
    case 0xF9: return 0x81;                 /* present/ready */
    case 0xFC: return (uint8_t)strlen(MCU_FW_ID);
    case 0xFF: return 0xA5;                  /* present signature */
    default:   return s->mcu_regs[index];
    }
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

/* ---- 0x4F config latch ---- */
static uint64_t cfg_read(void *o, hwaddr a, unsigned sz)
{
    return ((PC110ChipsetState *)o)->config_index;
}
static void cfg_write(void *o, hwaddr a, uint64_t v, unsigned sz)
{
    ((PC110ChipsetState *)o)->config_index = v;
}

/* ---- 0x74/0x76 SCAMP ---- */
static uint64_t scamp_read(void *o, hwaddr a, unsigned sz)
{
    PC110ChipsetState *s = o;
    if (a == 0) {                             /* 0x74 index */
        return s->scamp_index;
    }
    if (a == 2) {                             /* 0x76 data */
        return s->scamp_regs[s->scamp_index & 0x7F];
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

static void pc110_chipset_realizefn(DeviceState *d, Error **errp)
{
    ISADevice *isadev = ISA_DEVICE(d);
    PC110ChipsetState *s = PC110_CHIPSET(d);
    MemoryRegion *io = isa_address_space_io(isadev);

    s->status_15ec = 0x02;                    /* start "busy", settles on read */

    /* 0x80-0x8F: override QEMU's partial DMA page decode with full R/W scratch */
    memory_region_init_io(&s->io_80, OBJECT(d), &dpage_ops, s, "pc110-dmapage", 16);
    memory_region_add_subregion_overlap(io, 0x80, &s->io_80, 10);

    memory_region_init_io(&s->io_4f, OBJECT(d), &cfg_ops, s, "pc110-cfg", 1);
    memory_region_add_subregion(io, 0x4F, &s->io_4f);

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
         * On the PC110, C0000-FFFFF is shadow RAM that the BIOS copies itself
         * into during POST (it copies 0x10000->0xE0000 and verifies).  Model it
         * as writable RAM pre-loaded with the ROM image, so the shadow-copy
         * POST step succeeds instead of looping against read-only ROM.
         */
        memory_region_init_ram(&s->biosmap, OBJECT(d), "pc110-bios-cseg",
                               PC110_BIOS_MAP_SIZE, &error_fatal);
        memcpy(memory_region_get_ram_ptr(&s->biosmap), contents,
               PC110_BIOS_MAP_SIZE);
        g_free(contents);
        /* priority above isa-bios (1) and the VGA option ROM */
        memory_region_add_subregion_overlap(get_system_memory(),
                                            PC110_BIOS_MAP_BASE,
                                            &s->biosmap, 20);
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
