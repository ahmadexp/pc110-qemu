/*
 * IBM PC110 setup config-register interface (SeaBIOS-safe)
 *
 * The PC110 Easy-Setup program reads/writes a bank of chipset "config"
 * registers through an index/data pair that overlays the RTC data port:
 *
 *   port 0x4F   write: select config register index (also arms "config mode")
 *               read:  returns the current index
 *   port 0x70   write: normal RTC/CMOS index select — also DISARMS config mode
 *   port 0x71   read/write: config-register data while config mode is armed,
 *               otherwise ordinary RTC/CMOS data
 *
 * Under the real PC110 BIOS these are real chipset registers; SeaBIOS has no
 * equivalent, so Easy-Setup's config reads hit the plain RTC and it falls to
 * its error/self-test screen.  This device supplies the config-register bank
 * (with the reg-0xFB "first-draw" latch behaviour ported from PC110-EMU) plus
 * a minimal CMOS/RTC so Easy-Setup can run standalone on SeaBIOS.
 *
 * Unlike pc110-chipset this touches only 0x4F/0x70/0x71 — it does NOT override
 * the DMA page registers (0x80-0x8F), which triple-faults SeaBIOS.
 *
 * Usage:  -device pc110-setupcfg
 */

#include "qemu/osdep.h"
#include "hw/isa/isa.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_PC110_SETUPCFG "pc110-setupcfg"
OBJECT_DECLARE_SIMPLE_TYPE(PC110SetupCfgState, PC110_SETUPCFG)

struct PC110SetupCfgState {
    ISADevice parent_obj;

    uint8_t cfg_index;      /* selected by port 0x4F */
    bool    cfg_armed;      /* set by 0x4F write, cleared by 0x70 write */
    uint8_t cfg_regs[256];

    uint8_t rtc_index;      /* selected by port 0x70 */
    uint8_t cmos[128];

    uint8_t mcu_index;      /* power-sense MCU, port 0xEC */
    uint8_t mcu_regs[256];

    MemoryRegion io_4f;
    MemoryRegion io_70;     /* covers 0x70-0x71 */
    MemoryRegion io_ec;     /* covers 0xEC-0xED (power MCU) */
};

/* Power-sense MCU: report a healthy, present MCU (battery OK, on AC). */
static const char MCU_FW_ID[] = "PC110-PMCU";
static uint8_t mcu_value(PC110SetupCfgState *s, uint8_t idx)
{
    if (idx >= 0x80 && idx < 0xE0) {
        size_t off = idx - 0x80;
        return off < strlen(MCU_FW_ID) ? (uint8_t)MCU_FW_ID[off] : 0x00;
    }
    switch (idx) {
    case 0xF0: return 'M';
    case 0xF1: return 'C';
    case 0xF2: return 'U';
    case 0xF3: return 0x08;   /* revision */
    case 0xF9: return 0x81;   /* present/ready */
    case 0xFC: return (uint8_t)strlen(MCU_FW_ID);
    case 0xFF: return 0xA5;   /* present signature */
    default:   return s->mcu_regs[idx];
    }
}

/* Minimal CMOS/RTC: plausible fixed BCD time; retains other writes. */
static uint8_t cmos_read(PC110SetupCfgState *s, uint8_t idx)
{
    switch (idx & 0x7F) {
    case 0x00: return 0x00;   /* seconds  */
    case 0x02: return 0x30;   /* minutes  */
    case 0x04: return 0x12;   /* hours    */
    case 0x06: return 0x02;   /* weekday  */
    case 0x07: return 0x01;   /* day      */
    case 0x08: return 0x07;   /* month    */
    case 0x09: return 0x26;   /* year     */
    case 0x0A: return s->cmos[0x0A] & 0x7F;   /* not updating */
    case 0x0B: return s->cmos[0x0B];
    case 0x0C: return 0x00;
    case 0x0D: return 0x80;   /* battery OK */
    case 0x32: return 0x20;   /* century  */
    default:   return s->cmos[idx & 0x7F];
    }
}

/* ---- port 0x4F: config index / arm ---- */
static uint64_t cfg4f_read(void *o, hwaddr a, unsigned sz)
{
    return ((PC110SetupCfgState *)o)->cfg_index;
}
static void cfg4f_write(void *o, hwaddr a, uint64_t v, unsigned sz)
{
    PC110SetupCfgState *s = o;
    s->cfg_index = v;
    s->cfg_armed = true;
}

/* ---- ports 0x70/0x71: RTC index + config/RTC data ---- */
static uint64_t rtc_read(void *o, hwaddr a, unsigned sz)
{
    PC110SetupCfgState *s = o;
    if (a == 0) {                       /* 0x70 */
        return s->rtc_index;
    }
    /* 0x71 */
    if (s->cfg_armed) {
        uint8_t idx = s->cfg_index;
        uint8_t v = s->cfg_regs[idx];
        /* reg 0xFB bit1 is a "busy/first-draw" latch: report ready */
        if (idx == 0xFB) {
            v &= (uint8_t)~0x02u;
        }
        return v;
    }
    return cmos_read(s, s->rtc_index);
}
static void rtc_write(void *o, hwaddr a, uint64_t v, unsigned sz)
{
    PC110SetupCfgState *s = o;
    if (a == 0) {                       /* 0x70: RTC index select, disarm config */
        s->rtc_index = v & 0x7F;
        s->cfg_armed = false;
        return;
    }
    /* 0x71 */
    if (s->cfg_armed) {
        s->cfg_regs[s->cfg_index] = v;
        return;
    }
    s->cmos[s->rtc_index & 0x7F] = v;
}

/* ---- ports 0xEC/0xED: power-sense MCU index/data ---- */
static uint64_t mcu_read(void *o, hwaddr a, unsigned sz)
{
    PC110SetupCfgState *s = o;
    return a == 0 ? s->mcu_index : mcu_value(s, s->mcu_index);
}
static void mcu_write(void *o, hwaddr a, uint64_t v, unsigned sz)
{
    PC110SetupCfgState *s = o;
    if (a == 0) {
        s->mcu_index = v;
    } else {
        s->mcu_regs[s->mcu_index] = v;
    }
}

static const MemoryRegionOps cfg4f_ops = {
    .read = cfg4f_read, .write = cfg4f_write,
    .valid.min_access_size = 1, .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
static const MemoryRegionOps mcu_ops = {
    .read = mcu_read, .write = mcu_write,
    .valid.min_access_size = 1, .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};
static const MemoryRegionOps rtc_ops = {
    .read = rtc_read, .write = rtc_write,
    .valid.min_access_size = 1, .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void pc110_setupcfg_realizefn(DeviceState *d, Error **errp)
{
    ISADevice *isadev = ISA_DEVICE(d);
    PC110SetupCfgState *s = PC110_SETUPCFG(d);
    MemoryRegion *io = isa_address_space_io(isadev);

    memory_region_init_io(&s->io_4f, OBJECT(d), &cfg4f_ops, s, "pc110-setup-4f", 1);
    memory_region_add_subregion(io, 0x4F, &s->io_4f);

    /* overlay RTC ports so config reads via 0x71 work while armed */
    memory_region_init_io(&s->io_70, OBJECT(d), &rtc_ops, s, "pc110-setup-rtc", 2);
    memory_region_add_subregion_overlap(io, 0x70, &s->io_70, 10);

    /* power-sense MCU at 0xEC/0xED (SeaBIOS-safe) */
    memory_region_init_io(&s->io_ec, OBJECT(d), &mcu_ops, s, "pc110-setup-mcu", 2);
    memory_region_add_subregion(io, 0xEC, &s->io_ec);
}

static const VMStateDescription vmstate_pc110_setupcfg = {
    .name = TYPE_PC110_SETUPCFG,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(cfg_index, PC110SetupCfgState),
        VMSTATE_BOOL(cfg_armed, PC110SetupCfgState),
        VMSTATE_UINT8_ARRAY(cfg_regs, PC110SetupCfgState, 256),
        VMSTATE_UINT8(rtc_index, PC110SetupCfgState),
        VMSTATE_UINT8_ARRAY(cmos, PC110SetupCfgState, 128),
        VMSTATE_END_OF_LIST()
    }
};

static void pc110_setupcfg_class_initfn(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pc110_setupcfg_realizefn;
    dc->vmsd = &vmstate_pc110_setupcfg;
    dc->desc = "IBM PC110 Easy-Setup config-register interface";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo pc110_setupcfg_info = {
    .name          = TYPE_PC110_SETUPCFG,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(PC110SetupCfgState),
    .class_init    = pc110_setupcfg_class_initfn,
};

static void pc110_setupcfg_register_types(void)
{
    type_register_static(&pc110_setupcfg_info);
}

type_init(pc110_setupcfg_register_types)
