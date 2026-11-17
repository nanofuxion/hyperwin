/* Windows boot compatibility for stock AVF (controlled baseline).
 *
 * AVF runs crosvm with --no-pmu, and Windows boot code uses PMU registers unconditionally, so
 * images are patched in memory (patch_pmu) at three points, all inside boot services:
 *   - the boot manager, when it asks for its loaded-image protocol;
 *   - winload.efi, which the boot manager maps itself, once its mapped image is populated;
 *   - kernel, HAL and boot drivers, from winload's live page allocations, at ExitBootServices.
 * Only boot services are hooked, and only the five this needs. Runtime services are never
 * touched: this driver is boot-services code. Nothing is printed after ExitBootServices
 * succeeds. Progress marks go to the shared window for the host app (tag HyperWinDbg). */
#include "hw_abi.h"

typedef uint64_t (*efi_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef uint64_t (*efi_out)(void *, const uint16_t *);
typedef uint64_t (*efi_crc)(const void *, uint64_t, uint32_t *);

#define BS_SLOT(bs, id) ((efi_fn *)((uint8_t *)(bs) + 24 + (id) * 8))
#define ID_EXIT_BOOT_SERVICES 26
#define FIRST_FULL_CALLS 64u
#define SAMPLE_MASK 1023u

static void *con;
static efi_fn orig[48];
static uint32_t calls[48];
static int after_exit;
static int quiet_output;

static void put(const char *s)
{
    if (quiet_output) return;
    efi_out write = *(efi_out *)((uint8_t *)con + 8);
    uint16_t wide[96];
    while (*s) {
        uint32_t n = 0;
        while (*s && n + 1 < sizeof(wide) / sizeof(wide[0])) wide[n++] = (uint8_t)*s++;
        wide[n] = 0;
        write(con, wide);
    }
}

static void puthex(uint64_t v)
{
    char t[17], b[17];
    uint32_t n = 0, i;
    if (!v) { put("0"); return; }
    while (v) { t[n++] = "0123456789abcdef"[v & 15]; v >>= 4; }
    for (i = 0; i < n; ++i) b[i] = t[n - 1 - i];
    b[n] = 0;
    put(b);
}

static void putguid(uint64_t p)
{
    if (!p) { put("null"); return; }
    puthex(*(const uint32_t *)p);
}

static void flush_code(void *base, uint64_t bytes)
{
    uint64_t a = (uint64_t)base & ~63ull, e = (uint64_t)base + bytes;
    uint64_t p;
    for (p = a; p < e; p += 64) __asm__ volatile("dc cvau, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (p = a; p < e; p += 64) __asm__ volatile("ic ivau, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb ish\n isb" ::: "memory");
}

#ifdef HYPERWIN_STANDALONE_COMPAT
static void install_el0_pmccntr(uint64_t base);
static void install_el1_trap_logger(uint64_t base);
#endif
static uint32_t total_patched;

/* ---- the cycle counter, at the real processor's pace ----
 * Windows works out the processor's speed by timing its cycle counter, and uses that counter for
 * all its CPU accounting. This VM has no cycle counter, so reads of it become reads of the
 * virtual timer, which ticks at about 25 MHz: Windows then reports a 25 MHz processor. Multiplying
 * every such read by (real clock / timer rate) makes the counter look like the phone's own.
 * A multiply needs more than the one instruction a read occupies, so each read in the kernel
 * branches to a five-word stub placed in unused padding of the kernel's code, which reads the
 * timer, scales it with shifts and adds on the same register, and branches back. */
#ifdef HYPERWIN_STANDALONE_COMPAT
#define HW_SCALE_KERNEL_COUNTER 0
static uint32_t scale_shift;          /* count << scale_shift ... */
static uint32_t scale_op[2];          /* ... then up to two "x = x +/- (x >> n)", rd = rn = rm = 0 */
static uint32_t scale_ready;          /* a factor was worked out */
static uint32_t scale_on;             /* and the kernel has room for every stub */
static uint64_t kernel_base;

/* Picks shift and corrections whose product is closest to processor Hz / timer Hz. */
static void plan_scale(void)
{
    const struct hw_header *h = (const struct hw_header *)HW_WINDOW_PA;
    uint64_t frq, want, best = ~0ull;
    uint32_t a, i, j;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
    if (h->magic[0] != 'H' || h->magic[1] != 'Y' || h->header_bytes < sizeof *h || !h->cpu_mhz || !frq) return;
    want = (((uint64_t)h->cpu_mhz * 1000000ull) << 16) / frq;             /* 16.16 fixed point */
    if (want < (2u << 16) || want > (4096u << 16)) return;
    for (a = 0; a <= 12; ++a) for (i = 0; i < 25; ++i) for (j = 0; j < 25; ++j) {
        /* i, j: 0 none, 1..12 add x >> n, 13..24 subtract x >> (n - 12) */
        uint64_t c = 65536ull << a, err;
        if (i) c = i <= 12 ? c + (c >> i) : c - (c >> (i - 12));
        if (j) c = j <= 12 ? c + (c >> j) : c - (c >> (j - 12));
        if (!i && j) continue;
        err = c > want ? c - want : want - c;
        if (err >= best) continue;
        best = err;
        scale_shift = a;
        scale_op[0] = !i ? 0 : i <= 12 ? 0x8b400000u | (i << 10) : 0xcb400000u | ((i - 12) << 10);
        scale_op[1] = !j ? 0 : j <= 12 ? 0x8b400000u | (j << 10) : 0xcb400000u | ((j - 12) << 10);
    }
    /* Off. A read must branch to its stub, and the only room for 493 stubs is padding in and around
     * the kernel's exception vector tables. Windows rewrites some of that at run time, on some
     * CPUs, so 2 boots in 3 crashed (ATTEMPTED_EXECUTE_OF_NOEXECUTE_MEMORY). Windows reports the
     * timer's 25 MHz instead; the agent puts the real clock in the registry for display. */
    scale_ready = HW_SCALE_KERNEL_COUNTER;
}

/* The three scaling instructions for register r, padded with NOPs. */
static void emit_scale(volatile uint32_t *out, uint32_t r)
{
    uint32_t k;
    out[0] = scale_shift ? 0xd3400000u | (((64 - scale_shift) & 63) << 16) | ((63 - scale_shift) << 10) | (r << 5) | r
                         : 0xd503201fu;                                   /* lsl xR, xR, #shift */
    for (k = 0; k < 2; ++k)
        out[1 + k] = scale_op[k] ? scale_op[k] | (r << 16) | (r << 5) | r : 0xd503201fu;
}

/* Unused padding in the kernel's code that stays in memory: runs of zero words in sections that
 * are neither pageable nor dropped after start-up. These are the gaps around its vector tables. */
static volatile uint32_t *gap_at[512];
static uint32_t gap_words[512], gaps, gap_i, gap_off;

static void find_gaps(uint64_t base)
{
    uint32_t pe = *(const uint32_t *)(base + 0x3c), nsec, optsz, i;
    const uint8_t *hdr = (const uint8_t *)(base + pe);
    gaps = gap_i = gap_off = 0;
    if (pe < 0x40u || pe > 0x400u || *(const uint32_t *)hdr != 0x4550u) return;
    nsec = *(const uint16_t *)(hdr + 6);
    optsz = *(const uint16_t *)(hdr + 20);
    for (i = 0; i < nsec && i < 32; ++i) {
        const uint8_t *sec = hdr + 24 + optsz + i * 40;
        uint32_t vsize = *(const uint32_t *)(sec + 8), va = *(const uint32_t *)(sec + 12);
        uint32_t flags = *(const uint32_t *)(sec + 36), words = vsize / 4, n = 0;
        volatile uint32_t *w = (volatile uint32_t *)(base + va);
        if ((flags & 0x2a000000u) != 0x28000000u || vsize > 0x2000000u) continue;   /* code, not paged, kept */
        if (sec[0] == 'I' && sec[1] == 'N' && sec[2] == 'I' && sec[3] == 'T') continue;
        while (n < words) {
            uint32_t m = n;
            while (m < words && !w[m]) ++m;
            if (m - n >= 16 && gaps < 512) {            /* leave a word untouched at each end */
                gap_at[gaps] = w + n + 1;
                gap_words[gaps++] = m - n - 2;
            }
            n = m > n ? m : n + 1;
        }
    }
}

/* Next free five-word place. A vector entry starts every 0x80 bytes; never begin or cross one. */
static volatile uint32_t *next_slot(uint32_t *g, uint32_t *off)
{
    while (*g < gaps) {
        while (*off + 5 <= gap_words[*g]) {
            volatile uint32_t *p = gap_at[*g] + *off;
            uint64_t a = (uint64_t)p & 0x7f;
            if (a == 0 || a > 0x80 - 20) { *off += 1; continue; }
            *off += 5;
            return p;
        }
        ++*g;
        *off = 0;
    }
    return 0;
}

/* Counter reads already rewritten to plain timer reads. The kernel is patched while the loader
 * is still filling it in, before anyone knows it is the kernel; these are upgraded afterwards. */
static volatile uint32_t *read_site[4096];
static uint32_t read_sites, read_lost;

/* Turns the read at site into a branch to a stub that reads, scales and returns. */
static int scaled_read(volatile uint32_t *site, uint32_t rt)
{
    volatile uint32_t *stub = next_slot(&gap_i, &gap_off);
    int64_t back, out;
    if (!stub) return 0;
    back = (int64_t)(uint64_t)(site + 1) - (int64_t)(uint64_t)(stub + 4);
    out = (int64_t)(uint64_t)stub - (int64_t)(uint64_t)site;
    stub[0] = 0xd53be040u | rt;                                          /* mrs xN, CNTVCT_EL0 */
    emit_scale(stub + 1, rt);
    stub[4] = 0x14000000u | ((uint32_t)(back >> 2) & 0x03ffffffu);      /* b site + 4 */
    flush_code((void *)stub, 20);
    *site = 0x14000000u | ((uint32_t)(out >> 2) & 0x03ffffffu);         /* b stub */
    flush_code((void *)site, 4);
    return 1;
}

/* A run of free padding for a trap stub: the first gap big enough, taken from its start.
 * Only non-paged, kept code qualifies, because the trap stubs run inside exception handling. */
static uint8_t *take_space(uint32_t bytes)
{
    uint32_t g, words = (bytes + 3) / 4;
    for (g = 0; g < gaps; ++g) {
        uint8_t *p;
        if (gap_words[g] < words) continue;
        p = (uint8_t *)gap_at[g];
        gap_at[g] += words;
        gap_words[g] -= words;
        return p;
    }
    return 0;
}

static uint32_t count_reads(uint64_t base)
{
    uint32_t pe = *(const uint32_t *)(base + 0x3c), nsec, optsz, i, total = 0;
    const uint8_t *hdr = (const uint8_t *)(base + pe);
    nsec = *(const uint16_t *)(hdr + 6);
    optsz = *(const uint16_t *)(hdr + 20);
    for (i = 0; i < nsec && i < 32; ++i) {
        const uint8_t *sec = hdr + 24 + optsz + i * 40;
        uint32_t vsize = *(const uint32_t *)(sec + 8), va = *(const uint32_t *)(sec + 12), n;
        const volatile uint32_t *w = (const volatile uint32_t *)(base + va);
        if (!(*(const uint32_t *)(sec + 36) & 0x20000000u) || vsize > 0x2000000u) continue;
        for (n = 0; n < vsize / 4; ++n) if ((w[n] & 0xffffffe0u) == 0xd53b9d00u) ++total;
    }
    return total;
}
#endif

/* AVF runs crosvm with --no-pmu, so any access to a PMU register is an UNDEF. Windows boot
 * code reads the cycle counter (PMCCNTR_EL0) as its clock and programs PMCR_EL0 and friends
 * without checking for a PMU. Rewrite those instructions in the loaded copy: cycle-counter
 * reads become virtual-counter reads, other PMU reads return zero, PMU writes become NOPs.
 * Debug and OS-lock registers (op0 = 2) are left alone: KVM emulates them. */
static uint32_t patch_pmu(uint64_t base)
{
    uint32_t pe = *(const uint32_t *)(base + 0x3c), nsec, optsz, i, total = 0;
    const uint8_t *hdr = (const uint8_t *)(base + pe);
    if (*(const uint32_t *)hdr != 0x4550u) return 0;
    nsec = *(const uint16_t *)(hdr + 6);
    optsz = *(const uint16_t *)(hdr + 20);
    for (i = 0; i < nsec && i < 32; ++i) {
        const uint8_t *sec = hdr + 24 + optsz + i * 40;
        uint32_t vsize = *(const uint32_t *)(sec + 8), va = *(const uint32_t *)(sec + 12);
        uint32_t flags = *(const uint32_t *)(sec + 36), n;
        volatile uint32_t *w = (volatile uint32_t *)(base + va);
        if (!(flags & 0x20000000u) || vsize > 0x2000000u) continue;
        for (n = 0; n < vsize / 4; ++n) {
            uint32_t x = w[n], top = x & 0xfff80000u, rt = x & 31u, op1, crn, crm, op2;
#ifdef HYPERWIN_STANDALONE_COMPAT
            /* No EL3 on this machine, so SMC is an undefined instruction.
             * PSCI uses HVC. Every other SMCCC call gets NOT_SUPPORTED in x0. */
            if ((x & 0xffe0001fu) == 0xd4000003u) {
                w[n] = 0x92800000u; /* mov x0, #-1 */
                ++total;
                continue;
            }
#endif
            op1 = (x >> 16) & 7u; crn = (x >> 12) & 15u; crm = (x >> 8) & 15u; op2 = (x >> 5) & 7u;
            if (top != 0xd5380000u && top != 0xd5180000u) continue;   /* MRS / MSR, op0 = 3 */
            if (!((crn == 9 && (op1 == 3 || op1 == 0)) || (crn == 14 && op1 == 3 && crm >= 8))) continue;
            if (top == 0xd5180000u) w[n] = 0xd503201fu;                              /* msr -> nop */
            else if (crn == 9 && crm == 13 && op2 == 0 && op1 == 3) {                 /* PMCCNTR -> CNTVCT */
#ifdef HYPERWIN_STANDALONE_COMPAT
                if (scale_on && base == kernel_base && rt != 31 && scaled_read(&w[n], rt)) {
                    ++total;
                    continue;
                }
                if (read_sites < 4096) read_site[read_sites++] = &w[n]; else read_lost = 1;
#endif
                w[n] = 0xd53be040u | rt;
            }
            else if (crn == 9 && crm == 12 && op2 == 0 && op1 == 3)
                w[n] = rt == 31 ? 0xd503201fu : (0xd2800020u | rt);                  /* PMCR.E reads as 1 */
            else if (crn == 9 && crm == 12 && op2 == 1 && op1 == 3)
                w[n] = rt == 31 ? 0xd503201fu : (0xd2b00000u | rt);                  /* PMCNTENSET cycle bit */
            else w[n] = rt == 31 ? 0xd503201fu : (0xaa1f03e0u | rt);                 /* mrs -> mov xN, xzr */
            ++total;
        }
        flush_code((void *)(base + va), vsize);
    }
    return total;
}

#ifdef HYPERWIN_STANDALONE_COMPAT
/* smss.exe reads PMCCNTR_EL0 from EL0. There is no PMU, so that is an illegal
 * instruction and a critical process dies. Catch that one trap and return the
 * virtual counter instead. Signed images stay unmodified. */
#include "el0_pmccntr.h"   /* generated from guest/efi/el0_pmccntr.S */
#include "el1_pmu_trap.h"  /* generated from guest/efi/el1_pmu_trap.S */
#define pmcc_stub HyperwinEl0Pmccntr
#define el1_stub HyperwinEl1PmuTrap
/* Result of the install, for the host log when the console is quiet:
 * 1 installed, 2 kernel entry pattern not found, 3 no spare space found. */
static uint32_t el0_stub_status;
static uint32_t el1_stub_status;
static uint8_t *el0_vec_addr;
static uint8_t *el0_cave_addr;
static uint8_t *el1_cave_addr;

static void write_b(uint32_t *site, uint64_t target)
{
    int64_t delta = (int64_t)target - (int64_t)(uint64_t)site;
    *site = 0x14000000u | ((uint32_t)(delta >> 2) & 0x03ffffffu);
}

/* The trap stubs answer counter reads the in-place rewrite cannot reach (user programs, drivers
 * loaded later). They scale the same way: the three NOPs after their timer read are the slots. */
static void scale_stub(uint8_t *stub, uint32_t bytes)
{
    uint32_t n;
    if (!scale_on || !stub) return;
    for (n = 0; n + 16 <= bytes; n += 4)
        if (*(const uint32_t *)(stub + n) == (0xd53be040u | 17)) {
            emit_scale((volatile uint32_t *)(stub + n + 4), 17);
            flush_code(stub + n + 4, 12);
            return;
        }
}

static void install_el0_pmccntr(uint64_t base)
{
    static int installed;
    uint32_t pe, nsec, optsz, i;
    const uint8_t vec_sig[12] = {
        0xff,0xc3,0x0d,0xd1, 0xf2,0x7b,0x13,0xa9, 0x12,0x52,0x38,0xd5
    };
    const uint8_t cave_sig[16] = {
        0xe0,0x03,0x9f,0xd6, 0x9f,0x3f,0x03,0xd5, 0xdf,0x3f,0x03,0xd5, 0xff,0x30,0x03,0xd5
    };
    uint8_t *vec = 0, *cave = 0;
    if (installed) return;
    pe = *(const uint32_t *)(base + 0x3c);
    if (pe < 0x40u || pe > 0x400u || *(const uint32_t *)(base + pe) != 0x4550u) return;
    nsec = *(const uint16_t *)(base + pe + 6);
    optsz = *(const uint16_t *)(base + pe + 20);
    for (i = 0; i < nsec && i < 32; ++i) {
        const uint8_t *sec = (const uint8_t *)(base + pe + 24 + optsz + i * 40);
        uint32_t vsize = *(const uint32_t *)(sec + 8), va = *(const uint32_t *)(sec + 12);
        uint32_t flags = *(const uint32_t *)(sec + 36);
        uint8_t *p = (uint8_t *)(base + va);
        uint32_t n;
        if (!(flags & 0x20000000u) || vsize < 400u || vsize > 0x2000000u) continue;
        for (n = 0; n + 400 < vsize; n += 4) {
            if (!vec && n + 12 < vsize && p[n] == vec_sig[0] &&
                p[n+1] == vec_sig[1] && p[n+2] == vec_sig[2] && p[n+3] == vec_sig[3] &&
                *(const uint32_t *)(p + n + 4) == *(const uint32_t *)(vec_sig + 4) &&
                *(const uint32_t *)(p + n + 8) == *(const uint32_t *)(vec_sig + 8))
                vec = p + n;
            if (!cave && n + 16 + sizeof(pmcc_stub) < vsize &&
                *(const uint32_t *)(p + n) == *(const uint32_t *)cave_sig &&
                *(const uint32_t *)(p + n + 4) == *(const uint32_t *)(cave_sig + 4) &&
                *(const uint32_t *)(p + n + 8) == *(const uint32_t *)(cave_sig + 8) &&
                *(const uint32_t *)(p + n + 12) == *(const uint32_t *)(cave_sig + 12)) {
                uint32_t z;
                for (z = 0; z < sizeof(pmcc_stub); ++z) if (p[n + 16 + z]) break;
                if (z == sizeof(pmcc_stub)) cave = p + n + 16;
            }
        }
    }
    if (!vec) { el0_stub_status = 2; return; }
    /* Kernels differ (another Windows build, or the recovery environment's own kernel): when the
     * usual spot is not there, any free padding in the kernel's resident code does. */
    if (!cave) cave = take_space(sizeof(pmcc_stub));
    if (!cave) { el0_stub_status = 3; return; }
    for (i = 0; i < sizeof(pmcc_stub); ++i) cave[i] = pmcc_stub[i];
    write_b((uint32_t *)(cave + sizeof(pmcc_stub) - 4), (uint64_t)(vec + 4));
    write_b((uint32_t *)vec, (uint64_t)cave);
    flush_code(cave, sizeof(pmcc_stub));
    flush_code(vec, 4);
    installed = 1;
    el0_stub_status = 1;
    el0_vec_addr = vec;
    el0_cave_addr = cave;
    put("PMU-PATCH el0 pmccntr stub\r\n");
}

static void install_el1_trap_logger(uint64_t base)
{
    static int installed;
    uint32_t pe, nsec, optsz, i;
    uint8_t *el1_vec, *cave = 0;
    if (installed || !el0_vec_addr) return;
    /* Windows runs normal kernel code at EL1t (SP0), not EL1h (SPx).
     * Intercept before X18 is clobbered. The stub uses SP0 temporarily and
     * restores SPSel before ERET or the original prologue. */
    el1_vec = el0_vec_addr - 0x400;
    if (*(const uint32_t *)(el1_vec + 0) != 0xd5384112u ||
        *(const uint32_t *)(el1_vec + 4) != 0x927cee5fu ||
        *(const uint32_t *)(el1_vec + 8) != 0xd10dc3ffu) {
        el1_stub_status = 4;
        return;
    }
    pe = *(const uint32_t *)(base + 0x3c);
    if (pe < 0x40u || pe > 0x400u || *(const uint32_t *)(base + pe) != 0x4550u) return;
    nsec = *(const uint16_t *)(base + pe + 6);
    optsz = *(const uint16_t *)(base + pe + 20);
    (void)nsec; (void)optsz; (void)i;
    /* Resident padding only: this stub runs while handling an exception, where a page fault on
     * pageable code would recurse until the kernel gives up (PANIC_STACK_SWITCH). */
    cave = take_space(sizeof(el1_stub));
    if (!cave) { el1_stub_status = 3; return; }
    for (i = 0; i < sizeof(el1_stub); ++i) cave[i] = el1_stub[i];
    *(volatile uint32_t *)(cave + sizeof(el1_stub) - 8) = *(const uint32_t *)el1_vec;
    write_b((uint32_t *)(cave + sizeof(el1_stub) - 4), (uint64_t)(el1_vec + 4));
    write_b((uint32_t *)el1_vec, (uint64_t)cave);
    flush_code(cave, sizeof(el1_stub));
    flush_code(el1_vec, 4);
    installed = 1;
    el1_stub_status = 1;
    el1_cave_addr = cave;
    put("PMU-PATCH el1 trap stub cave="); puthex((uint64_t)cave); put("\r\n");
}
#endif

static void patch_image_by_handle(uint64_t handle)
{
    static const uint8_t li_guid[16] = {0xa1, 0x31, 0x1b, 0x5b, 0x62, 0x95, 0xd2, 0x11, 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b};
    uint64_t li = 0, base;
    if (orig[16](handle, (uint64_t)li_guid, (uint64_t)&li, 0, 0, 0) || !li) return;
    base = *(const uint64_t *)(li + 0x40);
    { uint32_t n = patch_pmu(base); total_patched += n;
    put("PMU-PATCH image="); puthex(base); put(" size="); puthex(*(const uint64_t *)(li + 0x48));
    put(" patched="); puthex(n); put("\r\n"); }
}

/* Marks in the shared window, written only while firmware context is valid.
 * [0] stage: 1 ExitBootServices entered, 2 it returned, [1] its status, [2] images patched,
 * [3] instructions patched, [4] live allocations scanned, [5] ExitBootServices call count. */
#if defined(HYPERWIN_STANDALONE_COMPAT) && !defined(HYPERWIN_MARKS_IN_WINDOW)
static volatile uint64_t standalone_marks[32];
#define DBG_MARK standalone_marks
#else
#define DBG_MARK ((volatile uint64_t *)(HW_WINDOW_PA + 0x7f00000ull + 0x100))
#endif

static int is_arm64_image(const uint8_t *p, uint64_t room, uint32_t *size)
{
    uint32_t pe;
    if (room < 0x1000 || p[0] != 'M' || p[1] != 'Z') return 0;
    pe = *(const uint32_t *)(p + 0x3c);
    if (pe > 0x400 || *(const uint32_t *)(p + pe) != 0x4550u || *(const uint16_t *)(p + pe + 4) != 0xaa64u) return 0;
    *size = *(const uint32_t *)(p + pe + 24 + 56);
    return *size && *size <= room;
}

/* winload.efi: the boot manager maps it itself, so watch its large allocations until one
 * holds a populated mapped image. Raw file buffers are smaller than SizeOfImage and are
 * never touched, because the boot manager checksums them. */
static uint64_t pending[8], pending_bytes[8];
static int loader_patched;
/* winload is filled in after the first sections match. Remember every image and
 * patch it again as later pages arrive; the first hit is not the whole file. */
static uint64_t loader_bases[8];
static uint64_t loader_spans[8];
static uint32_t loader_patched_count[8];
static uint32_t loader_base_n;

/* Only boot applications (winload, winresume) run before ExitBootServices, so only they need
 * patching while they load. The kernel and boot drivers wait until ExitBootServices: the loader
 * checks their signatures after loading them, and an image changed before that check fails it
 * (0xc0000221, "a critical system driver is missing or contains errors"). */
static int is_boot_application(uint64_t base)
{
    uint32_t pe = *(const uint32_t *)(base + 0x3c);
    uint16_t subsystem;
    if (pe < 0x40u || pe > 0x400u || *(const uint32_t *)(base + pe) != 0x4550u) return 0;
    subsystem = *(const uint16_t *)(base + pe + 24 + 68);
    /* 16: Windows boot application (bootmgr, winload, winresume); 10-13: plain EFI images */
    return subsystem == 16 || (subsystem >= 10 && subsystem <= 13);
}

static void repatch_loader(uint64_t base, uint64_t span)
{
    uint32_t i, n;
    if (!is_boot_application(base)) return;
    n = patch_pmu(base);
    for (i = 0; i < loader_base_n; ++i) if (loader_bases[i] == base) {
        if (n > loader_patched_count[i]) {
            total_patched += n - loader_patched_count[i];
            loader_patched_count[i] = n;
            put("PMU-PATCH reload image="); puthex(base); put(" patched="); puthex(n); put("\r\n");
        }
        return;
    }
    if (!n || loader_base_n >= 8) return;
    total_patched += n;
    loader_bases[loader_base_n] = base;
    loader_spans[loader_base_n] = span;
    loader_patched_count[loader_base_n++] = n;
    loader_patched = 1;
    put("PMU-PATCH loaded image="); puthex(base); put(" patched="); puthex(n); put("\r\n");
}

static void patch_pending_images(void)
{
    uint32_t i, size;
    for (i = 0; i < loader_base_n; ++i) repatch_loader(loader_bases[i], loader_spans[i]);
    for (i = 0; i < 8; ++i) {
        if (!pending[i]) continue;
        if (((const uint8_t *)pending[i])[0] != 'M') continue;
        if (!is_arm64_image((const uint8_t *)pending[i], pending_bytes[i], &size)) { pending[i] = 0; continue; }
        repatch_loader(pending[i], pending_bytes[i]);
    }
}

/* Live page allocations made after winload started. Freed ranges are removed. */
static struct { uint64_t addr, pages; } os_alloc[2048];
static uint32_t os_allocs;

static void os_alloc_free(uint64_t addr, uint64_t pages)
{
    uint32_t i;
    for (i = 0; i < os_allocs; ++i) {
        uint64_t a = os_alloc[i].addr, e = a + (os_alloc[i].pages << 12), fe = addr + (pages << 12);
        if (addr >= e || fe <= a) continue;
        if (addr <= a && fe >= e) { os_alloc[i] = os_alloc[--os_allocs]; --i; }   /* whole range */
        else if (addr <= a) { os_alloc[i].addr = fe; os_alloc[i].pages = (e - fe) >> 12; }
        else {
            os_alloc[i].pages = (addr - a) >> 12;                                  /* keep the head */
            if (fe < e && os_allocs < 2048) { os_alloc[os_allocs].addr = fe; os_alloc[os_allocs++].pages = (e - fe) >> 12; }
        }
    }
}

static void patch_os_images(void)
{
    uint32_t i, images = 0, total = 0, size;
    for (i = 0; i < os_allocs; ++i) {
        uint64_t off, bytes = os_alloc[i].pages << 12;
        for (off = 0; off < bytes; off += 4096) {
            const uint8_t *p = (const uint8_t *)(os_alloc[i].addr + off);
            if (!is_arm64_image(p, bytes - off, &size)) continue;
            total += patch_pmu((uint64_t)p);
            /* [32 + n]: TimeDateStamp << 32 | SizeOfImage of each OS image, to tell which drivers winload loaded. */
            if (images < 128) DBG_MARK[32 + images] = (uint64_t)*(const uint32_t *)(p + *(const uint32_t *)(p + 0x3c) + 8) << 32 | size;
            ++images;
            off += (((uint64_t)size + 4095u) & ~4095ull) - 4096;
        }
    }
    total_patched += total;
    DBG_MARK[2] = images; DBG_MARK[3] = total; DBG_MARK[4] = os_allocs;
    DBG_MARK[8] = total_patched;
    put("PMU-PATCH os images="); puthex(images); put(" instructions="); puthex(total);
    put(" allocations="); puthex(os_allocs); put(" cumulative="); puthex(total_patched); put("\r\n");
}

/* Log the largest image's entry instruction. Do not rewrite it: that image is
 * ntoskrnl, and it is entered on its own page tables. */
static void mark_kernel_entry(void)
{
    uint32_t i, size, best_size = 0;
    uint64_t best = 0;
    for (i = 0; i < os_allocs; ++i) {
        uint64_t off, bytes = os_alloc[i].pages << 12;
        for (off = 0; off < bytes; off += 4096) {
            const uint8_t *p = (const uint8_t *)(os_alloc[i].addr + off);
            if (!is_arm64_image(p, bytes - off, &size)) continue;
            if (size > best_size) { best_size = size; best = (uint64_t)p; }
            off += (((uint64_t)size + 4095u) & ~4095ull) - 4096;
        }
    }
    if (!best) return;
    {
        uint32_t pe = *(const uint32_t *)(best + 0x3c);
        const uint8_t *hdr = (const uint8_t *)(best + pe);
        uint32_t entry;
        if (pe < 0x40u || pe > 0x400u || *(const uint32_t *)hdr != 0x4550u) return;
        entry = *(const uint32_t *)(hdr + 24 + 16);
        if (entry >= best_size) return;
        put("KERNEL-ENTRY base="); puthex(best); put(" size="); puthex(best_size);
        put(" rva="); puthex(entry); put(" insn="); puthex(*(const uint32_t *)(best + entry));
        put("\r\n");
#ifdef HYPERWIN_STANDALONE_COMPAT
        /* Diagnostic for ntkrnlmp E6D05F65-3C73-B225-54C8-324CE63C3E2C:
         * BgpFwDisplayBugCheckScreen's parameter renderer normally checks
         * BcpDisplayParameters. Force just that display branch, never suppress
         * the exception or alter bugcheck arguments. Refuse unknown images. */
        if (*(const uint32_t *)(hdr + 8) == 0x0a307f2cu &&
            best_size > 0x5ac660u &&
            *(const uint32_t *)(best + 0x5ac658u) == 0x396001efu &&
            *(const uint32_t *)(best + 0x5ac65cu) == 0x3500008fu) {
            *(uint32_t *)(best + 0x5ac65cu) = 0x14000004u;
            flush_code((void *)(best + 0x5ac65cu), 4);
            DBG_MARK[8] |= 0x100000000ull;
        }
        /* Scaling needs a place for every counter read in the kernel, with room to spare for
         * the two trap stubs, which take their space first. Otherwise nothing is scaled. */
        kernel_base = best;
        plan_scale();
        find_gaps(best);                      /* free padding: the trap stubs take theirs first */
        install_el0_pmccntr(best);
        install_el1_trap_logger(best);
        find_gaps(best);                      /* again: what is left is for the scaled reads */
        {
            /* All or nothing: a counter scaled at some reads and not at others runs backwards. */
            uint32_t g = 0, off = 0, room = 0, reads = count_reads(best), k;
            for (k = 0; k < read_sites; ++k)
                if ((uint64_t)read_site[k] >= best && (uint64_t)read_site[k] < best + best_size) ++reads;
            while (next_slot(&g, &off)) ++room;
            scale_on = scale_ready && !read_lost && el0_stub_status == 1 && el1_stub_status == 1 && room >= reads;
            DBG_MARK[10] = (uint64_t)room << 32 | reads;
        }
        scale_stub(el0_cave_addr, sizeof(pmcc_stub));
        scale_stub(el1_cave_addr, sizeof(el1_stub));
        if (scale_on) {
            uint32_t k, done = 0;
            for (k = 0; k < read_sites; ++k) {
                volatile uint32_t *site = read_site[k];
                if ((uint64_t)site < best || (uint64_t)site >= best + best_size) continue;
                if ((*site & 0xffffffe0u) != 0xd53be040u || (*site & 31u) == 31u) continue;
                done += scaled_read(site, *site & 31u);
            }
            DBG_MARK[12] = done;
        }
        DBG_MARK[11] = (uint64_t)scale_on << 32 | scale_shift << 24 | (scale_op[0] >> 10 & 0x3f) << 8 | (scale_op[1] >> 10 & 0x3f);
        DBG_MARK[6] = el0_stub_status;
        DBG_MARK[7] = best;
        DBG_MARK[9] = el1_stub_status;
#endif
    }
}

static uint64_t hook_alloc(uint64_t type, uint64_t mem, uint64_t pages, uint64_t out, uint64_t e, uint64_t f)
{
    uint64_t r = orig[2](type, mem, pages, out, e, f);
    if (!r && out && !after_exit) {
        uint64_t addr = *(const uint64_t *)out;
        if (loader_patched) {
            if (os_allocs < 2048) { os_alloc[os_allocs].addr = addr; os_alloc[os_allocs++].pages = pages; }
        }
        if (pages >= 0x100) {
            static uint32_t slot;
            pending_bytes[slot & 7] = pages << 12;
            pending[slot++ & 7] = addr;
        }
    }
    if (!after_exit) patch_pending_images();
    return r;
}

static uint64_t hook_free(uint64_t addr, uint64_t pages, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
    uint64_t r = orig[3](addr, pages, c, d, e, f);
    if (!r && !after_exit) {
        uint32_t k;
        /* A candidate must remain one wholly live allocation. Any overlapping
         * free invalidates it, including a released interior or tail page. */
        for (k = 0; k < 8; ++k) {
            uint64_t bytes = pages << 12;
            if (pending[k] && addr < pending[k] + pending_bytes[k] &&
                pending[k] < addr + bytes) {
                pending[k] = 0;
                pending_bytes[k] = 0;
            }
        }
        for (k = 0; k < loader_base_n;) {
            uint64_t bytes = pages << 12;
            if (addr < loader_bases[k] + loader_spans[k] && loader_bases[k] < addr + bytes) {
                --loader_base_n;
                loader_bases[k] = loader_bases[loader_base_n];
                loader_spans[k] = loader_spans[loader_base_n];
                loader_patched_count[k] = loader_patched_count[loader_base_n];
            } else {
                ++k;
            }
        }
        os_alloc_free(addr, pages);
    }
    if (!after_exit) patch_pending_images();
    return r;
}

static uint64_t hook_handle_protocol(uint64_t handle, uint64_t guid, uint64_t out, uint64_t d, uint64_t e, uint64_t f)
{
    static int patched;
    uint64_t r = orig[16](handle, guid, out, d, e, f);
    /* First loaded-image query after our driver: the boot manager looking itself up. */
    if (!r && !patched && guid && out && *(const uint32_t *)guid == 0x5b1b31a1u) {
        uint64_t base = *(const uint64_t *)(*(const uint64_t *)out + 0x40);
        patched = 1;
        put("PMU-PATCH bootmgr image="); puthex(base); put(" patched="); puthex(patch_pmu(base)); put("\r\n");
    }
    return r;
}

static uint64_t hook_start_image(uint64_t handle, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
    patch_image_by_handle(handle);
    return orig[23](handle, b, c, d, e, f);
}

static uint64_t hook_exit_boot_services(uint64_t image, uint64_t key, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
    uint64_t r;
    uint32_t k;
    /* Console writes can allocate, invalidating the caller's memory-map key. */
    quiet_output = 1;
    DBG_MARK[5] = DBG_MARK[5] + 1;
    patch_pending_images();
    for (k = 0; k < loader_base_n; ++k) {
        uint64_t base = loader_bases[k];
        uint32_t pe = *(const uint32_t *)(base + 0x3c);
        if (pe < 0x40u || pe > 0x400u) continue;
        uint32_t stamp = *(const uint32_t *)(base + pe + 8);
        uint32_t image_bytes = *(const uint32_t *)(base + pe + 24 + 56);
        put("PMU-PATCH stamp="); puthex(stamp); put(" soi="); puthex(image_bytes); put("\r\n");
    }
    mark_kernel_entry();       /* first: it finds the kernel and decides how its counter reads are patched */
    patch_os_images();
    DBG_MARK[0] = 1;
    r = orig[ID_EXIT_BOOT_SERVICES](image, key, c, d, e, f);
    DBG_MARK[1] = r;
    DBG_MARK[0] = 2;
    if (!r) after_exit = 1;   /* console and boot services are gone from here on */
    else quiet_output = 0;
    return r;
}

/* Kept for the GOP driver's call sites; quiet in the baseline. */
void HyperwinTraceNote(const char *what, uint64_t a, uint64_t b, uint64_t c)
{
    (void)what; (void)a; (void)b; (void)c;
}

void HyperwinTraceInstall(void *system)
{
    void *bs = *(void **)((uint8_t *)system + 0x60);
    efi_crc crc = *(efi_crc *)((uint8_t *)bs + 24 + 40 * 8);
    uint32_t size = *(uint32_t *)((uint8_t *)bs + 12), sum = 0, k;
    con = *(void **)((uint8_t *)system + 0x40);
    for (k = 0; k < 32; ++k) DBG_MARK[k] = 0;
    orig[2] = *BS_SLOT(bs, 2);   *BS_SLOT(bs, 2) = hook_alloc;
    orig[3] = *BS_SLOT(bs, 3);   *BS_SLOT(bs, 3) = hook_free;
    orig[16] = *BS_SLOT(bs, 16); *BS_SLOT(bs, 16) = hook_handle_protocol;
    orig[23] = *BS_SLOT(bs, 23); *BS_SLOT(bs, 23) = hook_start_image;
    orig[ID_EXIT_BOOT_SERVICES] = *BS_SLOT(bs, ID_EXIT_BOOT_SERVICES);
    *BS_SLOT(bs, ID_EXIT_BOOT_SERVICES) = hook_exit_boot_services;
    /* The table header CRC covers these pointers; keep it valid for strict loaders. */
    *(uint32_t *)((uint8_t *)bs + 16) = 0;
    if (!crc(bs, size, &sum)) *(uint32_t *)((uint8_t *)bs + 16) = sum;
    put("TRACE boot services hooked (baseline)\r\n");
}

#ifdef HYPERWIN_STANDALONE_COMPAT
/* Gunyah advertises a PSCI idle state whose power_state is 0x04000000.
 * QEMU rejects any state with bits above bit 16, and Windows bugchecks
 * INTERNAL_POWER_ERROR. A zero state is a WFI, which QEMU accepts. */
static void patch_idle_state(void *system)
{
    static const uint8_t acpi20[16] = {
        0x71, 0xe8, 0x68, 0x88, 0xf1, 0xe4, 0xd3, 0x11,
        0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81
    };
    static const uint8_t gas[12] = {
        0x7f, 0x20, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00
    };
    uint64_t n = *(uint64_t *)((uint8_t *)system + 0x68);
    uint8_t *tables = *(uint8_t **)((uint8_t *)system + 0x70);
    uint64_t i, rsdp = 0, xsdt, count, t;
    uint32_t patched = 0;
    for (i = 0; i < n && i < 64; ++i) {
        uint8_t *g = tables + i * 24;
        uint32_t k;
        for (k = 0; k < 16 && g[k] == acpi20[k]; ++k) {}
        if (k == 16) { rsdp = *(uint64_t *)(g + 16); break; }
    }
    if (!rsdp) { put("IDLE no ACPI table\r\n"); return; }
    if (*(const uint8_t *)(rsdp + 15) < 2) { put("IDLE ACPI rev\r\n"); return; }
    xsdt = *(uint64_t *)(rsdp + 24);
    count = (*(uint32_t *)(xsdt + 4) - 36) / 8;
    for (t = 0; t < count && t < 32; ++t) {
        uint8_t *tbl = *(uint8_t **)(xsdt + 36 + t * 8);
        uint32_t len, o;
        if (tbl[0] != 'S' || tbl[1] != 'S' || tbl[2] != 'D' || tbl[3] != 'T') continue;
        if (tbl[1] == 'S' && tbl[0] == 'D') continue;
        len = *(uint32_t *)(tbl + 4);
        if (len < 48 || len > 0x20000) continue;
        for (o = 36; o + 12 <= len; ++o) {
            uint32_t k;
            for (k = 0; k < 12 && tbl[o + k] == gas[k]; ++k) {}
            if (k != 12) continue;
            *(uint64_t *)(tbl + o + 4) = 0;
            ++patched;
        }
    }
    put("IDLE states="); puthex(patched); put("\r\n");
}

uint64_t StandaloneCompatEntry(void *image, void *system)
{
    (void)image;
    HyperwinTraceInstall(system);
    patch_idle_state(system);
    return 0;
}
#endif
