/*
 * IBM PC110 font ROM emulation
 *
 * The IBM PC110 palmtop carries a 1 MB kanji font ROM (IBM FRU 84G7940)
 * exposed to real mode through an 8 KB banked window:
 *
 *   port 0x1160  R/W  bank select (8 KB banks, 128 banks for 1 MB)
 *   port 0x1161  R/W  (scratch, unknown function on real hardware)
 *   port 0x1162  R    window segment high byte (0xDE -> segment 0xDE00)
 *   port 0x1163  W    window enable (IBM $FONT.SYS writes 1 before probing)
 *
 *   memory 0xDE000..0xDFFFF: read-only view of rom[bank * 0x2000 .. +0x2000]
 *
 * $FONT.SYS detects the hardware by toggling bit 0 of port 0x1160 and
 * checking it reads back, then verifies the window starts with 0xAA55 and
 * carries "FONT" at offset 0x0E, and that writes to the window do not stick.
 *
 * Usage:  -device pc110-fontrom,romfile=/path/to/pc110-fontrom.bin
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "hw/isa/isa.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_PC110_FONTROM "pc110-fontrom"
OBJECT_DECLARE_SIMPLE_TYPE(PC110FontRomState, PC110_FONTROM)

#define FONTROM_SIZE     (1 * MiB)
#define FONTROM_BANK_SZ  0x2000
#define FONTROM_BANKS    (FONTROM_SIZE / FONTROM_BANK_SZ)
#define FONTROM_IOBASE   0x1160
#define FONTROM_WINDOW   0xde000

struct PC110FontRomState {
    ISADevice parent_obj;

    char *romfile;
    uint8_t *rom;
    uint8_t bank;
    uint8_t scratch;
    uint8_t enable;
    MemoryRegion io;
    MemoryRegion window;
};

static uint64_t fontrom_io_read(void *opaque, hwaddr addr, unsigned size)
{
    PC110FontRomState *s = opaque;

    switch (addr) {
    case 0: /* 0x1160 bank */
        return s->bank;
    case 1: /* 0x1161 */
        return s->scratch;
    case 2: /* 0x1162 window segment high byte */
        return FONTROM_WINDOW >> 12;
    case 3: /* 0x1163 enable */
        return s->enable;
    }
    return 0xff;
}

static void fontrom_io_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    PC110FontRomState *s = opaque;

    switch (addr) {
    case 0:
        s->bank = val;
        break;
    case 1:
        s->scratch = val;
        break;
    case 3:
        s->enable = val & 1;
        break;
    }
}

static uint64_t fontrom_window_read(void *opaque, hwaddr addr, unsigned size)
{
    PC110FontRomState *s = opaque;
    uint32_t base = (s->bank % FONTROM_BANKS) * FONTROM_BANK_SZ;
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        val |= (uint64_t)s->rom[base + ((addr + i) & (FONTROM_BANK_SZ - 1))]
               << (8 * i);
    }
    return val;
}

static void fontrom_window_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    /* ROM: writes are ignored ($FONT.SYS relies on this to detect ROM) */
}

static const MemoryRegionOps fontrom_io_ops = {
    .read = fontrom_io_read,
    .write = fontrom_io_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps fontrom_window_ops = {
    .read = fontrom_window_read,
    .write = fontrom_window_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void fontrom_realizefn(DeviceState *d, Error **errp)
{
    ISADevice *isadev = ISA_DEVICE(d);
    PC110FontRomState *s = PC110_FONTROM(d);
    gchar *contents;
    gsize len;
    GError *gerr = NULL;

    if (!s->romfile) {
        error_setg(errp, "pc110-fontrom: romfile property must be set");
        return;
    }
    if (!g_file_get_contents(s->romfile, &contents, &len, &gerr)) {
        error_setg(errp, "pc110-fontrom: cannot read romfile %s: %s",
                   s->romfile, gerr->message);
        g_error_free(gerr);
        return;
    }
    if (len != FONTROM_SIZE) {
        error_setg(errp, "pc110-fontrom: romfile %s is %zu bytes, expected %d",
                   s->romfile, (size_t)len, FONTROM_SIZE);
        g_free(contents);
        return;
    }
    s->rom = (uint8_t *)contents;

    memory_region_init_io(&s->io, OBJECT(d), &fontrom_io_ops, s,
                          "pc110-fontrom-io", 4);
    memory_region_add_subregion(isa_address_space_io(isadev),
                                FONTROM_IOBASE, &s->io);

    memory_region_init_io(&s->window, OBJECT(d), &fontrom_window_ops, s,
                          "pc110-fontrom-window", FONTROM_BANK_SZ);
    /*
     * Map above the i440FX PAM aliases (prio 1) in the system address
     * space: on the real PC110 this window is decoded by the chipset
     * ahead of RAM, and PAM would otherwise shadow it with pc.ram.
     */
    memory_region_add_subregion_overlap(get_system_memory(),
                                        FONTROM_WINDOW, &s->window, 10000);
}

static const VMStateDescription vmstate_fontrom = {
    .name = TYPE_PC110_FONTROM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(bank, PC110FontRomState),
        VMSTATE_UINT8(scratch, PC110FontRomState),
        VMSTATE_UINT8(enable, PC110FontRomState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property fontrom_properties[] = {
    DEFINE_PROP_STRING("romfile", PC110FontRomState, romfile),
};

static void fontrom_class_initfn(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = fontrom_realizefn;
    dc->vmsd = &vmstate_fontrom;
    dc->desc = "IBM PC110 banked kanji font ROM";
    device_class_set_props(dc, fontrom_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo fontrom_info = {
    .name          = TYPE_PC110_FONTROM,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(PC110FontRomState),
    .class_init    = fontrom_class_initfn,
};

static void fontrom_register_types(void)
{
    type_register_static(&fontrom_info);
}

type_init(fontrom_register_types)
