/*
 * PC110 POST completer intercept (ported from PC110-EMU's complete_f000_* funcs)
 *
 * The real PC110 BIOS POST contains loops that never terminate under QEMU
 * because they depend on PC110 chipset/memory behavior QEMU doesn't model.
 * PC110-EMU's interpreter detects these loops by EIP and completes them in
 * native code, then jumps to the loop's exit.  This replicates that at the
 * QEMU TCG level: cpu_exec_loop() calls pc110_post_intercept() with the guest
 * linear PC; on a match we perform the loop's effect on env and set eip to the
 * exit, and cpu_exec_loop re-fetches from the new eip.
 *
 * The loops run either at CS=F000 (in place) or relocated to CS=2000 (POST
 * copies itself to low RAM to test the F-segment shadow), so we match on the
 * 16-bit offset with CS in {0xF000, 0x2000}.  Set the PC110POST env var to
 * enable (off by default so normal QEMU is unaffected).
 */
#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/cpu-common.h"

/* cpu_reset() lives in hw/core; declare it here to avoid pulling hw headers
 * into this target file. */
void cpu_reset(CPUState *cpu);

static int pc110post_enabled = -1;

static inline uint16_t lo16(target_ulong v) { return (uint16_t)(v & 0xFFFF); }
static inline void set16(target_ulong *r, uint16_t v)
{
    *r = (*r & ~(target_ulong)0xFFFF) | v;
}

#define PC_CC_MASK (CC_O | CC_S | CC_Z | CC_A | CC_P | CC_C)

static void pc_store_eflags(CPUX86State *env, uint32_t f)
{
    /* store all condition codes explicitly (CC_OP_EFLAGS: CC_SRC = flags) */
    env->cc_src = f & PC_CC_MASK;
    env->cc_op = CC_OP_EFLAGS;
    env->df = 1 - (2 * ((f >> 10) & 1));
    env->eflags = f & ~(PC_CC_MASK | DF_MASK);
}

static void set_zf(CPUX86State *env, int zf)
{
    uint32_t f = cpu_compute_eflags(env);
    if (zf) { f |= CC_Z; } else { f &= ~CC_Z; }
    pc_store_eflags(env, f);
}

static void set_cf(CPUX86State *env, int cf)
{
    uint32_t f = cpu_compute_eflags(env);
    if (cf) { f |= CC_C; } else { f &= ~CC_C; }
    pc_store_eflags(env, f);
}

static uint32_t phys_ds(CPUX86State *env, uint16_t off)
{
    return (uint32_t)env->segs[R_DS].base + off;
}
static uint32_t phys_es(CPUX86State *env, uint16_t off)
{
    return (uint32_t)env->segs[R_ES].base + off;
}

/* F000:3CC1..3CC8  adapter/shadow checksum: BL(bh-position) += sum of CX words @DS:SI */
static void complete_checksum(CPUX86State *env, uint16_t off)
{
    uint16_t cx = lo16(env->regs[R_ECX]);
    uint16_t si = lo16(env->regs[R_ESI]);
    uint8_t bl = (uint8_t)((env->regs[R_EBX] >> 8) & 0xFF);
    if (off >= 0x3CC2 && off <= 0x3CC6) {
        uint16_t ax = lo16(env->regs[R_EAX]);
        bl = (uint8_t)(bl + (uint8_t)ax + (uint8_t)(ax >> 8));
        if (cx) cx--;
    }
    while (cx) {
        uint8_t w[2];
        cpu_physical_memory_read(phys_ds(env, si), w, 2);
        bl = (uint8_t)(bl + w[0] + w[1]);
        si += 2;
        cx--;
    }
    set16(&env->regs[R_ESI], si);
    env->regs[R_ECX] &= ~(target_ulong)0xFFFF;
    env->regs[R_EBX] = (env->regs[R_EBX] & ~(target_ulong)0xFF00) | ((uint32_t)bl << 8);
    env->eip = 0x3CC8;
}

/* F000:3C34..3C4C block-copy helper: copy min(CX,0x200) x 0x200B blocks
 * src->dst (+0x20/blk).  Triggered at the loop-body block start.  Before
 * 0x3C38 (MOV DS,AX/MOV ES,BX) the segs come from AX/BX; after, from DS/ES. */
static void complete_copy(CPUX86State *env, uint16_t off)
{
    uint16_t ax = lo16(env->regs[R_EAX]);
    uint16_t bx = lo16(env->regs[R_EBX]);
    uint16_t outer = lo16(env->regs[R_ECX]);
    uint16_t src = (off >= 0x3C38) ? (uint16_t)env->segs[R_DS].selector : ax;
    uint16_t dst = (off >= 0x3C38) ? (uint16_t)env->segs[R_ES].selector : bx;
    unsigned count = outer ? outer : 1;
    if (count > 0x8000) count = 0x8000;       /* safety bound (16 MB) */
    (void)ax; (void)bx;
    for (unsigned i = 0; i < count; i++) {
        uint8_t buf[0x200];
        cpu_physical_memory_read(((uint32_t)src << 4), buf, 0x200);
        cpu_physical_memory_write(((uint32_t)dst << 4), buf, 0x200);
        src += 0x20;
        dst += 0x20;
    }
    env->regs[R_ECX] &= ~(target_ulong)0xFFFF;
    env->eip = 0x3C4E;
    /* Match the reference emulator's complete_f000_3c31_copy_loop: exit with
     * ZF=0 (the POPA/RET epilogue at 3C4E does not depend on ZF; the reference
     * leaves it clear). */
    set_zf(env, 0);
}

/* F000:4139..413E  delay/progress loop: store BX at phys 0x72/0x73 */
static void complete_delay(CPUX86State *env)
{
    uint16_t cx = lo16(env->regs[R_ECX]);
    uint16_t bx = lo16(env->regs[R_EBX]);
    if (cx) {
        uint8_t b[2] = { (uint8_t)bx, (uint8_t)(bx >> 8) };
        cpu_physical_memory_write(0x72, b, 2);
    }
    env->regs[R_ECX] &= ~(target_ulong)0xFFFF;
    env->eip = 0x413F;
}

/* F000:61DB..61E0 (SHL) / 61E9..61EE (RCL)  DI-pattern write loops */
static void complete_mempat(CPUX86State *env, uint16_t off)
{
    int second = (off >= 0x61E9);
    uint16_t cx = lo16(env->regs[R_ECX]);
    uint16_t di = lo16(env->regs[R_EDI]);
    uint8_t al = (uint8_t)(env->regs[R_EAX] & 0xFF);
    while (cx) {
        cpu_physical_memory_write(phys_es(env, di), &al, 1);
        if (second) {
            uint16_t oldcf = cpu_compute_eflags(env) & CC_C ? 1 : 0;
            uint16_t newcf = (di & 0x8000) ? 1 : 0;
            di = (uint16_t)((di << 1) | oldcf);
            set_cf(env, newcf);
        } else {
            set_cf(env, (di & 0x8000) != 0);
            di = (uint16_t)(di << 1);
        }
        cx--;
    }
    set16(&env->regs[R_EDI], di);
    env->regs[R_ECX] &= ~(target_ulong)0xFFFF;
    env->eip = second ? 0x61EF : 0x61E1;
}

/*
 * Software INT 13h (disk) service, hooked at the ROM handler entry F000:D2D7
 * (IVT[0x13]).  The PC110 ROM INT13 doesn't read QEMU's IDE correctly for the
 * DOS boot chain (it loads zeros and the loader jumps into them), so — exactly
 * like the reference emulator — we service INT13 ourselves out of the disk
 * image named by $PC110BOOT, using the PC110 CF geometry (2 heads, 32 sec/trk),
 * then perform the IRET by hand (pop IP/CS/FLAGS, patch CF for status).
 */
static void pc110_int13(CPUX86State *env)
{
    const uint32_t SPT = 32, HEADS = 2;
    uint32_t ss_base = (uint32_t)env->segs[R_SS].base;
    uint16_t sp = lo16(env->regs[R_ESP]);
    uint8_t fr[6];
    uint16_t ret_ip, ret_cs, ret_fl;
    uint8_t ah = (env->regs[R_EAX] >> 8) & 0xFF;
    uint8_t al = env->regs[R_EAX] & 0xFF;
    uint8_t ch = (env->regs[R_ECX] >> 8) & 0xFF;
    uint8_t cl = env->regs[R_ECX] & 0xFF;
    uint8_t dh = (env->regs[R_EDX] >> 8) & 0xFF;
    uint8_t dl = env->regs[R_EDX] & 0xFF;
    int cf = 0;
    uint8_t rah = 0;
    const char *path = getenv("PC110BOOT");

    cpu_physical_memory_read(ss_base + sp, fr, 6);
    ret_ip = fr[0] | (fr[1] << 8);
    ret_cs = fr[2] | (fr[3] << 8);
    ret_fl = fr[4] | (fr[5] << 8);

    if (dl < 0x80) {
        cf = 1; rah = 0x80;                       /* no floppy attached */
    } else if (ah == 0x00 || ah == 0x09 || ah == 0x0D ||
               ah == 0x11 || ah == 0x25) {
        cf = 0; rah = 0;                          /* reset/init/recalibrate: ok */
    } else if (ah == 0x02) {                      /* read sectors (CHS) */
        uint32_t cyl  = ch | ((uint32_t)(cl & 0xC0) << 2);
        uint32_t sec  = cl & 0x3F;
        uint32_t head = dh;
        uint32_t count = al, got = 0;
        FILE *f = (path && sec) ? fopen(path, "rb") : NULL;
        if (f) {
            uint32_t lba = (cyl * HEADS + head) * SPT + (sec - 1);
            uint32_t dst = (uint32_t)env->segs[R_ES].base + lo16(env->regs[R_EBX]);
            if (fseek(f, (long)lba * 512, SEEK_SET) == 0) {
                uint8_t buf[512];
                for (; got < count; got++) {
                    if (fread(buf, 1, 512, f) != 512) {
                        break;
                    }
                    cpu_physical_memory_write(dst + got * 512, buf, 512);
                }
            }
            fclose(f);
        }
        cf = (got == count && count) ? 0 : 1;
        rah = cf ? 0x04 : 0x00;                   /* 0x04 = sector not found */
        env->regs[R_EAX] = (env->regs[R_EAX] & ~(target_ulong)0xFF) | (got & 0xFF);
    } else if (ah == 0x08) {                      /* get drive parameters */
        uint32_t max_cyl = 127;                   /* 128 cyls, 0-based (4 MB) */
        uint16_t cx = (uint16_t)(((max_cyl & 0xFF) << 8)
                     | (((max_cyl >> 2) & 0xC0)) | (SPT & 0x3F));
        env->regs[R_ECX] = (env->regs[R_ECX] & ~(target_ulong)0xFFFF) | cx;
        env->regs[R_EDX] = (env->regs[R_EDX] & ~(target_ulong)0xFFFF)
                         | ((uint32_t)(HEADS - 1) << 8) | 0x01;  /* DH=max head, DL=1 drive */
        cf = 0; rah = 0;
    } else if (ah == 0x15) {                      /* get disk type: fixed disk */
        rah = 0x03; cf = 0;
    } else {
        /*
         * Unhandled function (e.g. AH=41h EDD/LBA-extensions check, 42h ext
         * read, 48h ext get-params).  Report UNSUPPORTED (CF=1) rather than a
         * false success — otherwise DOS believes an unimplemented service
         * worked and proceeds with no data / a wrong geometry, computing
         * garbage CHS and crashing.  CF=1 forces DOS onto the CHS path (AH=02),
         * which we service correctly.
         */
        cf = 1; rah = 0x01;                       /* invalid/unsupported */
    }

    env->regs[R_EAX] = (env->regs[R_EAX] & ~(target_ulong)0xFF00)
                     | ((uint32_t)rah << 8);
    /* manual IRET */
    if (cf) { ret_fl |= 0x0001; } else { ret_fl &= ~0x0001; }
    env->eip = ret_ip;
    env->segs[R_CS].selector = ret_cs;
    env->segs[R_CS].base = (uint32_t)ret_cs << 4;
    set16(&env->regs[R_ESP], (uint16_t)(sp + 6));
    pc_store_eflags(env, ret_fl);
}

int pc110_booted; /* set once INT19 has software-booted the disk (read by the
                   * chipset: the E-segment becomes a writable DOS UMB post-boot) */
/* Set by hw/input/pckbd.c when a KBC 0xFE (or output-port reset-line) command is
 * issued in PC110POST mode, INSTEAD of QEMU's async full-machine reset (which
 * wipes RAM).  Consumed here at the next TB boundary as a synchronous CPU-only
 * reset so the driver's 286-era protected-mode-exit reset preserves RAM. */
int pc110_kbc_reset_pending;
static int g_reset_count; /* number of KBC-FE resets so far (diag) */
static uint16_t g_prev_off; /* previous F-seg TB offset (diagnostic: reset caller) */
static uint32_t g_prev_lin; /* previous TB linear PC across ALL segments (diag) */
#define G_LIN_RING_N 256
static uint32_t g_lin_ring[G_LIN_RING_N]; /* ring of recent TB linear PCs (diag) */
static unsigned g_lin_ring_pos;
static uint32_t g_dos_ring[16]; /* ring of recent conventional-mem (<0xA0000) PCs */
static unsigned g_dos_ring_pos;
static uint32_t g_last_nonfseg; /* last non-F-seg (DOS/driver) linear PC (diag) */

bool pc110_post_intercept(CPUState *cs, vaddr pc);
bool pc110_post_intercept(CPUState *cs, vaddr pc)
{
    X86CPU *cpu = X86_CPU(cs);
    CPUX86State *env = &cpu->env;
    uint32_t csbase;
    uint16_t cssel, off;

    if (pc110post_enabled < 0) {
        pc110post_enabled = getenv("PC110POST") ? 1 : 0;
    }
    if (!pc110post_enabled) {
        return false;
    }

    /* Track the previous TB's linear PC across ALL segments (diagnostic): lets
     * the 5516 reset handler report the driver/DOS address that triggered a
     * post-boot reset, which runs outside the F-segment. */
    uint32_t prev_lin = g_prev_lin;
    g_prev_lin = (uint32_t)pc;
    g_lin_ring[g_lin_ring_pos & (G_LIN_RING_N - 1)] = (uint32_t)pc;
    g_lin_ring_pos++;
    /* Separate ring of CONVENTIONAL-memory PCs (< 0xA0000): genuine DOS/driver
     * code, excluding C/E/F-seg BIOS.  Reveals what the driver was doing right
     * before it dived into the BIOS reset cascade. */
    if ((uint32_t)pc < 0x000A0000) {
        g_dos_ring[g_dos_ring_pos & 15] = (uint32_t)pc;
        g_dos_ring_pos++;
        /* Log CR0 in the driver checksum region to confirm real vs protected
         * mode (reference runs this driver in real mode, cr0=0x10 PE=0). */
        if (getenv("PC110RSTLOG") && (uint32_t)pc >= 0x00038A30 &&
            (uint32_t)pc <= 0x00038A62) {
            static int cr0n;
            if (cr0n++ < 4) {
                fprintf(stderr, "[pc110post] driver@%05X cr0=%08X (PE=%d) "
                        "cs=%04X\n", (unsigned)pc, (unsigned)env->cr[0],
                        (int)(env->cr[0] & 1), (unsigned)env->segs[R_CS].selector);
            }
        }
    }

    /*
     * Driver-issued KBC reset (OUT 64,FE from DOS/driver code, any CS): pckbd.c
     * set pc110_kbc_reset_pending instead of an async machine reset.  Consume it
     * here as a synchronous CPU-only reset (RAM preserved) + warm-boot signature,
     * regardless of CS base.  This is the Personaware driver's protected-mode-exit
     * reset; the reference (PC110-EMU) traps KBC 0xFE at the port level the same
     * way (kbc_cpu_reset_pending).  Without this the OUT falls through to QEMU's
     * i8042 async full-machine reset, wiping RAM and losing DOS. */
    if (pc110_kbc_reset_pending) {
        pc110_kbc_reset_pending = 0;
        static const uint8_t warmsig[2] = { 0x34, 0x12 };
        cpu_physical_memory_write(0x00000472, warmsig, 2);
        if (getenv("PC110RSTLOG")) {
            static int drn;
            fprintf(stderr, "[pc110post] KBC reset (PORT-level/driver) #%d  "
                    "CS=%04X base=%05X IP=%04X\n", ++drn,
                    (unsigned)env->segs[R_CS].selector,
                    (unsigned)env->segs[R_CS].base,
                    (unsigned)(env->eip & 0xFFFF));
        }
        cpu_reset(cs);
        return true;
    }

    /*
     * Gate on the CS *base*, not the selector value.  The PC110 POST runs its
     * F-segment code under several different CS selector values that all keep
     * base = 0xF0000 (0xF000 in real mode, but also 0x0040 and others left over
     * from big-real-mode segment loads) — the KBC warm reset, for instance,
     * executes at 0040:551A.  Matching on the selector missed all of those; the
     * base is invariant.  0x20000 is the relocated (CS=2000) POST copy.
     */
    csbase = (uint32_t)env->segs[R_CS].base;
    if (csbase != 0xF0000 && csbase != 0x20000) {
        g_last_nonfseg = (uint32_t)pc; /* DOS/driver code (diag) */
        return false;
    }
    cssel = (uint16_t)env->segs[R_CS].selector;
    (void)cssel;
    off = (uint16_t)(pc - csbase);

    uint16_t prev_off = g_prev_off; /* previous F-seg TB offset (reset-caller diag) */
    g_prev_off = off;

    /*
     * KBC warm reset F000:5516 = MOV AL,0FE / OUT 64,AL.  The PC110 pulses the
     * keyboard-controller CPU-reset line (command 0xFE) both during POST and,
     * critically, from the running OS/driver as the 286-era "exit protected
     * mode" idiom.  QEMU's i8042 turns 0xFE into an ASYNC qemu_system_reset_
     * request that (a) doesn't fire promptly under TCG and (b) is a full MACHINE
     * reset that wipes RAM.  Do a synchronous CPU-only reset here: cpu_reset()
     * returns the CPU to the real-mode reset vector (F000:FFF0) while RAM, the
     * chipset CMOS, and the BDA all persist.
     *
     * THE RESUME CONTRACT (verified against the PC110-EMU reference, which boots
     * this BIOS to the desktop): the reference writes the BDA warm-boot signature
     * 40:72 = 1234h on EVERY KBC-FE reset before re-entering the ROM.  Without it
     * the BIOS reset entry treats the restart as a COLD boot and re-runs the full
     * chipset/SCAMP init+verify sequence -- the post-DOS-boot loop we hit.  With
     * it, the BIOS takes the warm path and honours the driver's CMOS-0x0F /
     * 40:67 continuation, so DOS/driver init survives the reset.  40:72 only
     * gates the 0x0F==0 cold path, so setting it is safe on every reset. */
    if (csbase == 0xF0000 && off == 0x5516) {
        static const uint8_t warmsig[2] = { 0x34, 0x12 }; /* 1234h, little-endian */
        cpu_physical_memory_write(0x00000472, warmsig, 2);
        g_reset_count++;
        if (getenv("PC110RSTLOG")) {
            static int rn;
            uint8_t v67[4] = {0}, v72[2] = {0};
            cpu_physical_memory_read(0x00000467, v67, 4);
            cpu_physical_memory_read(0x00000472, v72, 2);
            char ring[256]; int rp = 0;
            for (int k = 32; k >= 2; k--) {
                unsigned idx = (g_lin_ring_pos - k) & 31;
                rp += snprintf(ring + rp, sizeof(ring) - rp, "%05X ",
                               (unsigned)g_lin_ring[idx]);
            }
            char dring[160]; int dp = 0;
            for (int k = 16; k >= 1; k--) {
                unsigned idx = (g_dos_ring_pos - k) & 15;
                dp += snprintf(dring + dp, sizeof(dring) - dp, "%05X ",
                               (unsigned)g_dos_ring[idx]);
            }
            if (getenv("PC110RSTLOG"))
                fprintf(stderr, "            recent DOS(<A0000) TBs: %s\n", dring);
            fprintf(stderr, "[pc110post] KBC-FE reset #%d @F000:5516  %s  "
                    "prevTB=F000:%04X  lastDOS=%05X  40:72=%02X%02X\n"
                    "            recentTBs(old->new): %s\n",
                    ++rn, pc110_booted ? "POST-BOOT" : "pre-boot", (unsigned)prev_off,
                    (unsigned)g_last_nonfseg, v72[1], v72[0], ring);
            (void)v67;
            (void)prev_lin;
        }
        /* On the FIRST post-boot reset, dump the full 256-TB history so we can
         * see the exact driver->BIOS transition that dives into the reset. */
        if (pc110_booted && getenv("PC110RSTLOG")) {
            static int dumped;
            if (!dumped++) {
                fprintf(stderr, "[pc110post] === full 256-TB history before first "
                        "post-boot reset (old->new) ===\n");
                for (int k = 0; k < G_LIN_RING_N; k += 16) {
                    char line[256]; int lp = 0;
                    for (int j = 0; j < 16; j++) {
                        unsigned idx = (g_lin_ring_pos - G_LIN_RING_N + k + j)
                                       & (G_LIN_RING_N - 1);
                        lp += snprintf(line + lp, sizeof(line) - lp, "%05X ",
                                       (unsigned)g_lin_ring[idx]);
                    }
                    fprintf(stderr, "   %s\n", line);
                }
            }
        }
        cpu_reset(cs);
        return true;
    }

    /*
     * DIAGNOSTIC (PC110RSTLOG): the BIOS "unexpected interrupt" handler at
     * F000:5B68 (reached via the per-vector stubs at F000:5B00..5B66, each of
     * which loads a POST code into AL then jmps here; the handler IRETs or, on
     * port-0x8C<0x3B, HLT-loops at 5B9A).  During the warm re-POST after a
     * post-DOS reset something vectors here and wedges.  Log AL (the stub's POST
     * code = which vector) and the interrupted CS:IP:FLAGS off the INT frame. */
    /* DIAGNOSTIC (PC110RSTLOG): the five ROM sites that jmp to the 5516 FE-reset,
     * each guarded by a different condition (4B7B/4B93 = SMSW PE-bit / real-vs-
     * protected; 4D5C/4D6F = [0x72]==0x5678 / [0x12] signature; 5AB3 = option-ROM
     * 0xAA55).  Log which gate fires each reset so we can see why QEMU takes an
     * extra post-boot reset the reference does not. */
    if (csbase == 0xF0000 && getenv("PC110RSTLOG") &&
        (off == 0x4B7B || off == 0x4B93 || off == 0x4D5C ||
         off == 0x4D6F || off == 0x5AB3)) {
        static int seen;
        if (seen++ < 32) {
            uint32_t dsb = (uint32_t)env->segs[R_DS].base;
            uint8_t w72[2] = {0}, b12 = 0;
            cpu_physical_memory_read(dsb + 0x72, w72, 2);
            cpu_physical_memory_read(dsb + 0x12, &b12, 1);
            fprintf(stderr, "[pc110post] ->5516 via %04X  %s  "
                    "AX=%04X DS=%04X [ds:72]=%02X%02X [ds:12]=%02X\n",
                    (unsigned)off, pc110_booted ? "POST-BOOT" : "pre-boot",
                    (unsigned)(env->regs[R_EAX] & 0xFFFF),
                    (unsigned)env->segs[R_DS].selector,
                    w72[1], w72[0], b12);
        }
        return false;
    }

    if (csbase == 0xF0000 && off == 0x5B68 && getenv("PC110RSTLOG")) {
        uint32_t ssb = (uint32_t)env->segs[R_SS].base;
        uint16_t sp = (uint16_t)env->regs[R_ESP];
        uint8_t fr[6] = {0};
        cpu_physical_memory_read(ssb + sp, fr, 6);
        static int seen;
        if (seen++ < 24) {
            fprintf(stderr, "[pc110post] unexpected-int @5B68 code=%02X  "
                    "from %02X%02X:%02X%02X fl=%02X%02X\n",
                    (unsigned)(env->regs[R_EAX] & 0xFF),
                    fr[3], fr[2], fr[1], fr[0], fr[5], fr[4]);
        }
        return false;
    }

    /*
     * INT 19h bootstrap F000:52BD.  The PC110 ROM's INT19 runs a PC110-specific
     * boot-device discovery loop (INT15 AX=CA00/CA01 power-management probes +
     * chipset device control) to pick a device; that probe doesn't recognise
     * QEMU's IDE disk, so it never issues the boot read.  The reference
     * emulator sidesteps the whole probe by servicing INT19 in software: load
     * LBA0 to 0000:7C00 and jump there with DL=boot drive.  Do the same — read
     * the first 512 bytes of the disk image named by $PC110BOOT and boot it.
     */
    /* INT 13h handler entry F000:D2D7 (IVT[0x13]): service disk I/O from the
     * $PC110BOOT image ourselves (the ROM path mis-reads QEMU's IDE). */
    if (csbase == 0xF0000 && off == 0xD2D7 && getenv("PC110BOOT")) {
        pc110_int13(env);
        return true;
    }


    if (csbase == 0xF0000 && off == 0x52BD) {
        if (getenv("PC110RSTLOG")) {
            static int h52;
            fprintf(stderr, "[pc110post] INT19 @F000:52BD hit #%d  "
                    "after %d resets  (booted=%d)\n",
                    ++h52, g_reset_count, pc110_booted);
        }
        const char *bootpath = getenv("PC110BOOT");
        if (!pc110_booted && bootpath && bootpath[0]) {
            FILE *bf = fopen(bootpath, "rb");
            if (bf) {
                uint8_t sec[512];
                size_t n = fread(sec, 1, sizeof(sec), bf);
                fclose(bf);
                if (n == sizeof(sec)) {
                    pc110_booted = 1;
                    cpu_physical_memory_write(0x00007C00, sec, sizeof(sec));
                    /*
                     * Fill the INT 41h fixed-disk parameter table (IVT[0x41] ->
                     * F000:EA93, in writable F-segment shadow).  POST left it
                     * all zeros, so IO.SYS reads geometry 0/0/0 and computes
                     * garbage CHS -> crash.  Write the CF geometry
                     * (128 cyl x 2 heads x 32 sec/trk = 4 MB): FDPT[0..1]=cyls,
                     * [2]=heads, [0xE]=spt. */
                    {
                        static const uint8_t fdpt[16] = {
                            0x80, 0x00,       /* cylinders = 128 */
                            0x02,             /* heads = 2 */
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x80, 0x00,       /* landing zone = 128 */
                            0x20,             /* sectors/track = 32 */
                            0x00,
                        };
                        cpu_physical_memory_write(0x000FEA93, fdpt, sizeof(fdpt));
                    }
                    /*
                     * Set BDA 40:13 (base memory size) = 640 KB.  POST left it
                     * at 0xFFFF (the memory-sizing routine that fills it is the
                     * one SYSF makes us skip), so MS-DOS 7's MSLOAD read 65535
                     * KB, computed a bogus top-of-memory, and relocated itself
                     * into the BIOS shadow (~FF44:0) — running corrupted and
                     * issuing garbage INT13 (DL=CC).  640 KB puts MSLOAD at the
                     * correct top of conventional RAM (~9FC0:0).
                     */
                    {
                        static const uint8_t basemem[2] = { 0x80, 0x02 }; /* 640 */
                        cpu_physical_memory_write(0x00000413, basemem, 2);
                    }
                    /* enter the boot sector in real mode at 0000:7C00, DL=drive */
                    env->segs[R_CS].selector = 0; env->segs[R_CS].base = 0;
                    env->segs[R_DS].selector = 0; env->segs[R_DS].base = 0;
                    env->segs[R_ES].selector = 0; env->segs[R_ES].base = 0;
                    env->regs[R_EDX] = (env->regs[R_EDX] & ~(target_ulong)0xFF)
                                       | 0x80;   /* DL = first fixed disk */
                    env->eip = 0x7C00;
                    fprintf(stderr, "[pc110post] INT19: booted %s LBA0 -> 0000:7C00 "
                            "(sig %02X%02X) DL=80\n", bootpath, sec[510], sec[511]);
                    return true;
                }
                fprintf(stderr, "[pc110post] INT19: short read from %s\n", bootpath);
            } else {
                fprintf(stderr, "[pc110post] INT19: cannot open PC110BOOT=%s\n", bootpath);
            }
        }
    }

    /*
     * NOTE: the old F000:38FA (force-4MB) and F000:60C1 (skip mem-probe)
     * forcers were REMOVED.  With the KBC system-flag (SYSF) seeded set, the
     * PC110 POST takes its normal path (F000:384C JNZ) and never enters the
     * chipset memory-sizing routine at all, and with 20 MiB RAM present the
     * memory-pattern probe runs and passes naturally; we only guarantee its
     * success return at F000:61BE below (as the reference emulator does).
     */

    /* Memory-test success return F000:61BE: the reference forces the probe's
     * return to the success convention (ZF=1, CF=0, AX=0) every time so the
     * caller's JNZ at 418D does not branch to the "215" halt.  Set the flags
     * and let the RET at 61BE execute (return false). */
    if (off == 0x61BE) {
        env->regs[R_EAX] &= ~(target_ulong)0xFFFF;
        set_zf(env, 1);
        set_cf(env, 0);
        return false;                         /* let the real RET run */
    }
    /* PIT calibration F000:6981 (SUB BX,AX; then range-check BX in
     * [0x1614,0x1868] at 6983/6989, else error "102").  The test cross-checks
     * PIT ch0 against the DRAM-refresh rate over an EA80 delay of 320 refresh
     * toggles; QEMU's refresh-clock-to-PIT ratio doesn't match the real PC110
     * so the natural delta is out of range -> error 102 -> halt.  Force an
     * in-range delta (0x1700) and jump straight to the pass path 0x698F.  Fire
     * immediately (QEMU's real PIT reaches 6981 once, with no BX=0 retry loop,
     * so the reference's hit-threshold never triggers here). */
    if (off == 0x6981) {
        set16(&env->regs[R_EBX], 0x1700);
        env->eip = 0x698F;
        return true;
    }
    /* Port-61 refresh/timer waits that never settle under QEMU's timing:
     *   F000:6943  (AND AL,20 / JNZ 6949)  -> force bit5 set, take ready branch
     *   F000:C960  (AND AL,10 / LOOP)      -> collapse the delay (CX=0)
     *   F000:EA90  (AND AL,10 / LOOP EA86) -> collapse the delay (CX=0)
     * and the BDA-status wait F000:6961 (OUT 4F / LOOP 6958, waits [006B].0). */
    if (off == 0x6943) {
        static uint32_t h; if (++h < 1024) { return false; }
        h = 0;
        env->regs[R_EAX] |= 0x20;
        set_zf(env, 0);
        env->eip = 0x6949; return true;
    }
    if (off == 0xC960) {
        static uint32_t h; if (++h < 4096) { return false; }
        h = 0; env->regs[R_ECX] &= ~(target_ulong)0xFFFF; env->eip = 0xC966; return true;
    }
    if (off == 0xEA90) {
        static uint32_t h; if (++h < 1024) { return false; }
        h = 0; env->regs[R_ECX] &= ~(target_ulong)0xFFFF; env->eip = 0xEA92; return true;
    }
    if (off == 0x6961) {
        static uint32_t h; if (++h < 1024) { return false; }
        h = 0;
        {
            uint8_t b; cpu_physical_memory_read(0x0000006B, &b, 1);
            b |= 0x01; cpu_physical_memory_write(0x0000006B, &b, 1);
        }
        env->eip = 0x6958; return true;
    }
    /*
     * NOTE: the F000:5527 / F000:5553 decimal-string completers were REMOVED.
     * The reference emulator never fires them (they terminate naturally); firing
     * them here SKIPPED the decimal-to-ASCII conversion, leaving the POST status
     * message without its 0x0A terminator, which made the string-print routine
     * (F000:53C1) walk off the end and flood INT10 -> the VGA scroll spun
     * forever.  Let the conversion run to completion instead.
     */
    /* String-print flood breaker F000:53C1 (MOV AL,[CS:SI]/INC SI/print/CMP
     * AL,0A/JNZ 53C1): if a status string is missing its 0x0A terminator the
     * routine walks off the end and floods INT10 (spinning the VGA scroll).
     * 53C1 is the loop-back target (a TB start) with a balanced stack, so after
     * a generous char budget jump to the routine's RET at 0x53CE.  (Mirrors the
     * reference emulator's F000:53C5 output guard.) */
    if (off == 0x53C1) {
        static uint32_t h; if (++h < 4096) { return false; }
        h = 0; env->eip = 0x53CE; return true;
    }
    /* zero-length option-ROM skip F000:6D3A: advance the C000 scan past the
     * video ROM (size byte 0) so it reaches DX==si and exits. */
    if (off == 0x6D3A) {
        set16(&env->regs[R_EDX], (uint16_t)(lo16(env->regs[R_EDX]) + 0x80));
        env->eip = 0x6D49;
        return true;
    }
    /* FDC MSR ready-wait F000:3D90 (in 0x3F4; and 0xC0; cmp 0x80; loopne):
     * skip to the routine's RET — no floppy is attached (IDE boot). */
    if (off == 0x3D90) {
        env->eip = 0x3D9E;
        return true;
    }
    /* HLT resumes: the idle-wait block is F000:6998 = CLI(FA);HLT(F4);RET(C3).
     * The BIOS parks here with interrupts disabled expecting an external wake;
     * under QEMU that never comes, so skip CLI+HLT straight to the RET at
     * 0x699A (matches the reference's "idle HLT resume").  The intercept fires
     * at TB starts, and the TB begins at 0x6998, so match the block start.
     * Also skip the post-INT19 fallback HLT block at 52BF. */
    if (off == 0x6998 || off == 0x6999) { env->eip = 0x699A; return true; }
    if (off == 0x52BE || off == 0x52BF) { env->eip = 0x52C0; return true; }
    /* Tripwire: the "215"/error POST halt.  Should never be reached on the
     * success path; log once so upstream failures are visible. */
    if (off == 0x4A51) {
        static int seen;
        if (!seen++) {
            fprintf(stderr, "[pc110post] POST 215/error halt reached at F000:4A51\n");
        }
        env->eip = 0x4A53;                    /* step past the HLT to keep going */
        return true;
    }

    /*
     * The four loop completers fire only after the loop has spun many times
     * (matching PC110-EMU's hit thresholds), so loops that terminate naturally
     * on QEMU run to completion and only genuinely-stuck loops are completed.
     * Each hit = one loop iteration (we return false to let the body run).
     */
    static uint32_t h_cksum, h_copy, h_delay, h_mempat;

    if (off >= 0x3CC1 && off < 0x3CC8) {
        if (++h_cksum < 4096) return false;
        h_cksum = 0;
        complete_checksum(env, off);
        return true;
    }
    /* Only at the loop-body block start 0x3C34 (the loope target): there the
     * stack is clean ([es,ds,pusha,ret]) so jumping to the epilogue 3C4E
     * (pop es/pop ds/popa/ret) is balanced.  Firing mid-body (e.g. at 3C4B,
     * after a pending push cx) would corrupt the stack and ret to garbage. */
    if (off == 0x3C34) {
        if (++h_copy < 512) return false;
        h_copy = 0;
        complete_copy(env, off);
        return true;
    }
    if (off >= 0x4139 && off <= 0x413E) {
        if (++h_delay < 256) return false;
        h_delay = 0;
        complete_delay(env);
        return true;
    }
    if ((off >= 0x61DB && off <= 0x61E0) || (off >= 0x61E9 && off <= 0x61EE)) {
        if (++h_mempat < 128) return false;
        h_mempat = 0;
        complete_mempat(env, off);
        return true;
    }
    return false;
}
