/* Builds an SSDT that describes each processor (ACPI0007) with a _CPC package, and installs it
 * with the PCC table that carries the _CPC registers.
 *
 * The firmware describes no processors to Windows, so Windows has nothing to read a speed from:
 * it times its cycle counter instead, which this VM can only back with the 24.576 MHz timer, and
 * reports a 25 MHz processor. With _CPC, Windows' processor driver takes the rated speed from
 * NominalFrequency. Windows' processor driver (fxppm.sys, ValidateAcpiCPCRegister) accepts
 * registers only in PCC, FFH or port space, so they sit in a PCC subspace in the shared window
 * (include/hw_abi.h HW_CPPC). There are no idle states (_LPI) beyond the default WFI. The speed
 * is the phone's, from the window header (cpu_mhz). */
#include "hw_abi.h"
#include "osc_ssdt.h"   /* generated from guest/efi/osc_ssdt.asl: grants Windows CPPC */
#include "gpu_ssdt.h"   /* generated from guest/efi/gpu_ssdt.asl: the GPU device and its interrupt */

typedef uint64_t (*efi_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

#define PERF 100u              /* highest and nominal performance; lower levels scale linearly */
#define PERF_LOW 10u           /* lowest performance: Windows rejects a range of one level */
#define SPACE_MEMORY 0u        /* generic register address spaces */
#define SPACE_PCC 0x0au
#define PCC_SUBSPACE 0u

static uint8_t *put(uint8_t *p, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    while (n--) *p++ = *s++;
    return p;
}

static uint8_t *dword(uint8_t *p, uint32_t v)                     /* DWordConst */
{
    *p++ = 0x0c;
    *p++ = (uint8_t)v; *p++ = (uint8_t)(v >> 8); *p++ = (uint8_t)(v >> 16); *p++ = (uint8_t)(v >> 24);
    return p;
}

/* PkgLength for a body of n bytes, counting the length bytes themselves. */
static uint32_t pkglen(uint8_t *dst, uint32_t n)
{
    uint32_t t;
    if (n + 1 < 64) { dst[0] = (uint8_t)(n + 1); return 1; }
    if (n + 2 < (1u << 12)) { t = n + 2; dst[0] = 0x40 | (t & 15); dst[1] = (uint8_t)(t >> 4); return 2; }
    if (n + 3 < (1u << 20)) { t = n + 3; dst[0] = 0x80 | (t & 15); dst[1] = (uint8_t)(t >> 4); dst[2] = (uint8_t)(t >> 12); return 3; }
    t = n + 4; dst[0] = 0xc0 | (t & 15); dst[1] = (uint8_t)(t >> 4); dst[2] = (uint8_t)(t >> 12); dst[3] = (uint8_t)(t >> 20);
    return 4;
}

/* Opcode, PkgLength, then the body that is already written at body (may overlap the output). */
static uint8_t *wrap(uint8_t *p, const uint8_t *op, uint32_t oplen, const uint8_t *body, uint32_t n)
{
    uint8_t len[4];
    uint32_t l = pkglen(len, n), i;
    uint8_t *start = p + oplen + l;
    if (start < body) for (i = 0; i < n; ++i) start[i] = body[i];        /* body may overlap: memmove */
    else for (i = n; i > 0; --i) start[i - 1] = body[i - 1];
    p = put(p, op, oplen);
    p = put(p, len, l);
    return p + n;
}

/* Buffer holding a Generic Register descriptor and an end tag: ResourceTemplate { Register(...) }.
 * offset >= 0: a 64-bit register at that byte of the PCC register space; -1: not implemented. */
static uint8_t *reg(uint8_t *p, int64_t offset)
{
    uint8_t b[19], *q = b;
    uint32_t i;
    uint64_t addr = offset < 0 ? 0 : (uint64_t)offset;
    *q++ = 0x0a; *q++ = 17;                               /* buffer size */
    *q++ = 0x82; *q++ = 12; *q++ = 0;                     /* Generic Register, 12 bytes */
    if (offset < 0) { *q++ = SPACE_MEMORY; *q++ = 0; *q++ = 0; *q++ = 0; }
    else { *q++ = SPACE_PCC; *q++ = 64; *q++ = 0; *q++ = PCC_SUBSPACE; }   /* access size = subspace */
    for (i = 0; i < 8; ++i) *q++ = (uint8_t)(addr >> (8 * i));
    *q++ = 0x79; *q++ = 0;                                /* end tag */
    *p++ = 0x11;
    p += pkglen(p, sizeof b);
    return put(p, b, sizeof b);
}

static uint8_t *cpu_device(
    uint8_t *p, uint32_t index, uint32_t uid,
    uint32_t perf_high, uint32_t perf_low, uint32_t max_mhz, uint32_t min_mhz)
{
    static const char hex[] = "0123456789ABCDEF";
    static uint8_t dev[512], pkg[400];
    uint8_t *d = dev, *k = pkg;
    int64_t desired = 16 + 16 * (int64_t)index, limited = desired + 8;
    if (perf_high < 2u) perf_high = PERF;
    if (perf_low == 0u || perf_low >= perf_high) perf_low = perf_high > 1u ? perf_high - 1u : 1u;
    if (min_mhz == 0u || min_mhz >= max_mhz) min_mhz = max_mhz > 1u ? max_mhz * perf_low / perf_high : 1u;
    *k++ = 23;                                            /* elements */
    k = dword(k, 23);                                     /* NumEntries */
    k = dword(k, 3);                                      /* Revision */
    k = dword(k, perf_high);                              /* Highest */
    k = dword(k, perf_high);                              /* Nominal */
    k = dword(k, perf_low);                               /* Lowest nonlinear */
    k = dword(k, perf_low);                               /* Lowest */
    k = reg(k, -1);                                       /* Guaranteed performance */
    k = reg(k, desired);                                  /* Desired performance */
    k = reg(k, -1);                                       /* Minimum performance */
    k = reg(k, -1);                                       /* Maximum performance */
    k = reg(k, -1);                                       /* Performance reduction tolerance */
    k = reg(k, -1);                                       /* Time window */
    k = reg(k, -1);                                       /* Counter wraparound time */
    k = reg(k, 0);                                        /* Reference performance counter */
    k = reg(k, 8);                                        /* Delivered performance counter */
    k = reg(k, limited);                                  /* Performance limited */
    k = reg(k, -1);                                       /* CPPC enable */
    k = dword(k, 0);                                      /* Autonomous selection: off */
    k = reg(k, -1);                                       /* Autonomous activity window */
    k = reg(k, -1);                                       /* Energy performance preference */
    k = dword(k, perf_high);                              /* Reference performance */
    k = dword(k, min_mhz);                                /* Lowest frequency, MHz */
    k = dword(k, max_mhz);                                /* Nominal frequency, MHz */
    *d++ = 'H'; *d++ = 'C'; *d++ = (uint8_t)hex[(index >> 4) & 15]; *d++ = (uint8_t)hex[index & 15];
    d = put(d, "\x08_HID\x0d" "ACPI0007", 14); *d++ = 0;
    d = put(d, "\x08_UID", 5); d = dword(d, uid);
    d = put(d, "\x08_CPC", 5);
    d = wrap(d, (const uint8_t *)"\x12", 1, pkg, (uint32_t)(k - pkg));
    return wrap(p, (const uint8_t *)"\x5b\x82", 2, dev, (uint32_t)(d - dev));
}

static int guid_is(const uint8_t *g, const uint8_t *want)
{
    uint32_t i;
    for (i = 0; i < 16 && g[i] == want[i]; ++i) {}
    return i == 16;
}

static uint32_t fdt_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int fdt_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static int fdt_has_compat(const uint8_t *v, uint32_t len, const char *want)
{
    uint32_t i = 0;
    while (i < len) {
        uint32_t j = 0;
        while (i + j < len && want[j] && v[i + j] == (uint8_t)want[j]) ++j;
        if (i + j < len && !want[j] && !v[i + j]) return 1;
        while (i < len && v[i]) ++i;
        ++i;
    }
    return 0;
}

static uint64_t fdt_cells(const uint8_t *p, uint32_t n)
{
    uint64_t v = 0;
    while (n--) { v = (v << 32) | fdt_be32(p); p += 4; }
    return v;
}

/* Discover AVF/crosvm virtual cpufreq MMIO from the live FDT without accessing it.
 * Record in hw_header; protocol 1 = android-v-only, 2 = upstream, 0 = absent/unsupported. */
static uint32_t discover_virt_cpufreq(void *system, struct hw_header *h)
{
    static const uint8_t fdt_guid[16] = {
        0xd5, 0x21, 0xb6, 0xb1, 0x9c, 0xf1, 0xa5, 0x41,
        0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0
    };
    uint64_t n = *(uint64_t *)((uint8_t *)system + 0x68), i;
    const uint8_t *tables = *(const uint8_t **)((uint8_t *)system + 0x70), *fdt = 0;
    uint32_t off_struct, off_strings, proto = 0, total, struct_size, strings_size;
    const uint8_t *p, *strings, *end;
    uint32_t ac[16], sc[16];
    const uint8_t *reg_v[16];
    uint32_t reg_len[16], node_proto[16];
    int depth = -1;
    uint64_t base = 0, size = 0;

    h->virt_cpufreq_base = 0;
    h->virt_cpufreq_size = 0;
    h->virt_cpufreq_proto = 0;

    for (i = 0; i < n && i < 64 && !fdt; ++i)
        if (guid_is(tables + i * 24, fdt_guid))
            fdt = *(const uint8_t **)(tables + i * 24 + 16);
    if (!fdt || fdt_be32(fdt) != 0xd00dfeedu) return 0;

    off_struct = fdt_be32(fdt + 8);
    off_strings = fdt_be32(fdt + 12);
    total = fdt_be32(fdt + 4);
    strings_size = fdt_be32(fdt + 32);
    struct_size = fdt_be32(fdt + 36);
    if (total < 40 || total > 16u * 1024u * 1024u ||
        off_struct > total || struct_size > total - off_struct ||
        off_strings > total || strings_size > total - off_strings) return 0;
    p = fdt + off_struct;
    end = p + struct_size;
    strings = fdt + off_strings;

    for (;;) {
        if ((uint64_t)(end - p) < 4) return 0;
        uint32_t tok = fdt_be32(p);
        p += 4;
        if (tok == 1) {
            const char *name = (const char *)p;
            uint32_t len = 0;
            while (p + len < end && name[len]) ++len;
            if (p + len == end || (uint64_t)(end - p) < (((uint64_t)len + 4u) & ~3ull)) return 0;
            p += (len + 4) & ~3u;
            if (++depth >= 16) break;
            ac[depth] = 2; sc[depth] = 1;
            reg_v[depth] = 0; reg_len[depth] = 0; node_proto[depth] = 0;
        } else if (tok == 3) {
            if ((uint64_t)(end - p) < 8) return 0;
            uint32_t len = fdt_be32(p), nameoff = fdt_be32(p + 4);
            const uint8_t *v = p + 8;
            if ((((uint64_t)len + 3u) & ~3ull) > (uint64_t)(end - v) || nameoff >= strings_size) return 0;
            const char *pn = (const char *)strings + nameoff;
            uint32_t name_len = nameoff;
            while (name_len < strings_size && strings[name_len]) ++name_len;
            if (name_len == strings_size) return 0;
            if (depth >= 0 && depth < 16) {
                if (fdt_streq(pn, "#address-cells") && len == 4) ac[depth] = fdt_be32(v);
                else if (fdt_streq(pn, "#size-cells") && len == 4) sc[depth] = fdt_be32(v);
                else if (fdt_streq(pn, "reg")) { reg_v[depth] = v; reg_len[depth] = len; }
                else if (fdt_streq(pn, "compatible")) {
                    if (fdt_has_compat(v, len, "qemu,virtual-cpufreq")) node_proto[depth] = 2;
                    else if (fdt_has_compat(v, len, "virtual,android-v-only-cpufreq")) node_proto[depth] = 1;
                }
            }
            p += 8 + ((len + 3) & ~3u);
        } else if (tok == 2) {
            if (depth >= 0 && depth < 16 && node_proto[depth] && reg_v[depth]) {
                /* Nested buses need ranges translation; unsupported means no control. */
                if (depth != 1) return 0;
                uint32_t pa = depth > 0 ? ac[depth - 1] : 2;
                uint32_t ps = depth > 0 ? sc[depth - 1] : 1;
                if (pa <= 2 && ps <= 2 && reg_len[depth] >= 4 * (pa + ps)) {
                    proto = node_proto[depth];
                    base = fdt_cells(reg_v[depth], pa);
                    size = fdt_cells(reg_v[depth] + 4 * pa, ps);
                    break;
                }
            }
            --depth;
        } else if (tok == 4) {
        } else {
            break;
        }
    }

    /* Root-child registers are guest physical addresses. Do not truncate a region size. */
    if (!proto || !base || !size || size > 0xffffffffu || base > UINT64_MAX - size) return 0;
    h->virt_cpufreq_base = base;
    h->virt_cpufreq_size = (uint32_t)size;
    h->virt_cpufreq_proto = proto;
    return proto;
}

/* The MADT's enabled GIC CPU interfaces, as ACPI processor UIDs.
 * When eff_class != NULL and its count matches the enabled GICC entries, also writes each
 * GICC's ProcessorPowerEfficiencyClass (ACPI 6.0+ offset 76) and updates the MADT checksum. */
static uint32_t processor_uids(void *system, uint32_t *uid, uint32_t max,
                               const uint8_t *eff_class, uint32_t eff_count)
{
    static const uint8_t acpi20[16] = {
        0x71, 0xe8, 0x68, 0x88, 0xf1, 0xe4, 0xd3, 0x11, 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81
    };
    uint64_t n = *(uint64_t *)((uint8_t *)system + 0x68), i, xsdt = 0, count;
    uint8_t *tables = *(uint8_t **)((uint8_t *)system + 0x70);
    uint8_t *madt = 0;
    uint32_t madt_len = 0, found = 0;
    for (i = 0; i < n && i < 64 && !xsdt; ++i)
        if (guid_is(tables + i * 24, acpi20)) {
            const uint8_t *rsdp = *(const uint8_t **)(tables + i * 24 + 16);
            if (rsdp[15] >= 2) xsdt = *(const uint64_t *)(rsdp + 24);
        }
    if (!xsdt) return 0;
    count = (*(const uint32_t *)(xsdt + 4) - 36) / 8;
    for (i = 0; i < count && i < 64; ++i) {
        uint8_t *t = *(uint8_t **)(xsdt + 36 + i * 8);
        uint32_t len, off;
        if (t[0] != 'A' || t[1] != 'P' || t[2] != 'I' || t[3] != 'C') continue;
        len = *(const uint32_t *)(t + 4);
        madt = t;
        madt_len = len;
        for (off = 44; off + 2 <= len && t[off + 1]; off += t[off + 1]) {
            if (t[off] == 0x0b && t[off + 1] >= 16 && (*(const uint32_t *)(t + off + 12) & 1) && found < max)
                uid[found++] = *(const uint32_t *)(t + off + 8);
        }
    }
    if (madt && madt_len >= 44 && eff_class && eff_count == found && found > 1) {
        uint32_t off, idx = 0, patched = 0;
        for (off = 44; off + 2 <= madt_len && madt[off + 1]; off += madt[off + 1]) {
            if (madt[off] == 0x0b && madt[off + 1] >= 77 && (*(const uint32_t *)(madt + off + 12) & 1) && idx < found) {
                madt[off + 76] = eff_class[idx];
                patched = 1;
                idx++;
            }
        }
        if (patched) {
            uint32_t sum = 0;
            madt[9] = 0;
            for (off = 0; off < madt_len; ++off) sum += madt[off];
            madt[9] = (uint8_t)(0x100 - (sum & 0xff));
        }
    }
    return found;
}

static void checksum(uint8_t *t, uint32_t size)
{
    uint32_t i, sum = 0;
    t[9] = 0;
    for (i = 0; i < size; ++i) sum += t[i];
    t[9] = (uint8_t)(0x100 - (sum & 0xff));
}

static uint8_t *header(uint8_t *p, const char *sig, uint32_t size, uint8_t rev, const char *table_id)
{
    p = put(p, sig, 4);
    *p++ = (uint8_t)size; *p++ = (uint8_t)(size >> 8); *p++ = (uint8_t)(size >> 16); *p++ = 0;
    *p++ = rev; *p++ = 0;
    p = put(p, "HYPRWN", 6);
    p = put(p, table_id, 8);
    *p++ = 1; *p++ = 0; *p++ = 0; *p++ = 0;
    p = put(p, "HYWN", 4);
    *p++ = 1; *p++ = 0; *p++ = 0; *p++ = 0;
    return p;
}

static uint8_t *u64le(uint8_t *p, uint64_t v)
{
    uint32_t i;
    for (i = 0; i < 8; ++i) *p++ = (uint8_t)(v >> (8 * i));
    return p;
}

/* PCCT with one generic subspace: the HW_CPPC page. Its doorbell is the page's own status word,
 * written as "command complete", so every command Windows sends finishes as it rings. */
static uint32_t build_pcct(uint8_t *t)
{
    uint64_t base = HW_WINDOW_PA + HW_CPPC, status = base + 6;
    uint8_t *p = header(t, "PCCT", 48 + 62, 2, "HWPCC   ");
    volatile uint32_t *sig = (volatile uint32_t *)base;
    volatile uint16_t *st = (volatile uint16_t *)status;
    *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0;              /* flags: no platform interrupt */
    p = u64le(p, 0);                                      /* reserved */
    *p++ = 0; *p++ = 62;                                  /* subspace type 0 (generic), length */
    *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0;
    p = u64le(p, base);                                   /* shared memory */
    p = u64le(p, 0x1000);
    *p++ = SPACE_MEMORY; *p++ = 16; *p++ = 0; *p++ = 2;   /* doorbell: the status word */
    p = u64le(p, status);
    p = u64le(p, 0);                                      /* doorbell preserve: nothing */
    p = u64le(p, 1);                                      /* doorbell write: command complete */
    *p++ = 1; *p++ = 0; *p++ = 0; *p++ = 0;              /* nominal latency, us */
    *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0;              /* no access rate limit */
    *p++ = 0; *p++ = 0;                                   /* no turnaround time */
    *sig = 0x50434300u | PCC_SUBSPACE;                    /* "PCC" signature + subspace id */
    *st = 1;                                              /* idle: last command complete */
    checksum(t, 48 + 62);
    return 48 + 62;
}

/* 0 installed; negative: no speed from the host (-1), no processors (-2), no table protocol (-3). */
int64_t HyperwinInstallCpuSsdt(void *system)
{
    static const uint8_t acpi_table_guid[16] = {
        0xdd, 0x6b, 0xe0, 0xff, 0x07, 0x61, 0xa6, 0x46, 0x7b, 0xb2, 0x5a, 0x9c, 0x7e, 0xc5, 0x27, 0x5c
    };
    static uint8_t table[36 + 64 * 420], body[sizeof table];
    static uint32_t uid[HW_CPPC_MAX_CPUS];
    struct hw_header *h = (struct hw_header *)HW_WINDOW_PA;
    void *bs = *(void **)((uint8_t *)system + 0x60), *protocol = 0;
    uint32_t cpus, i, size, sum = 0, mhz;
    uint64_t key = 0;
    uint8_t *p;
    if (h->magic[0] != 'H' || h->magic[1] != 'Y' || h->header_bytes < sizeof *h || !h->cpu_mhz) return -1;
    discover_virt_cpufreq(system, h);
    mhz = h->cpu_mhz;
    cpus = processor_uids(system, uid, HW_CPPC_MAX_CPUS,
                          h->cpu_topo_count > 1 ? h->cpu_eff_class : 0,
                          h->cpu_topo_count);
    if (!cpus) return -2;
    p = body + 16;                                        /* devices first, then wrapped in Scope */
    p = put(p, "\\_SB_", 5);
    for (i = 0; i < cpus; ++i) {
        uint32_t perf_high = PERF, perf_low = PERF_LOW;
        uint32_t max_mhz = mhz, min_mhz = mhz * PERF_LOW / PERF;
        if (h->cpu_topo_count == cpus && cpus > 1 && h->cpu_capacity[i] >= 2 && h->cpu_max_mhz[i] > 0) {
            perf_high = h->cpu_capacity[i];
            max_mhz = h->cpu_max_mhz[i];
            min_mhz = h->cpu_min_mhz[i];
            if (min_mhz == 0 || min_mhz >= max_mhz) min_mhz = max_mhz > 10 ? max_mhz / 10 : 1;
            perf_low = perf_high * min_mhz / max_mhz;
            if (perf_low == 0) perf_low = 1;
            if (perf_low >= perf_high) perf_low = perf_high - 1;
        }
        p = cpu_device(p, i, uid[i], perf_high, perf_low, max_mhz, min_mhz);
    }
    p = wrap(body, (const uint8_t *)"\x10", 1, body + 16, (uint32_t)(p - body - 16));
    size = 36 + (uint32_t)(p - body);
    p = table;
    p = put(p, "SSDT", 4);
    *p++ = (uint8_t)size; *p++ = (uint8_t)(size >> 8); *p++ = (uint8_t)(size >> 16); *p++ = 0;
    *p++ = 2; *p++ = 0;
    p = put(p, "HYPRWN", 6);
    p = put(p, "HWCPU   ", 8);
    *p++ = 1; *p++ = 0; *p++ = 0; *p++ = 0;
    p = put(p, "HYWN", 4);
    *p++ = 1; *p++ = 0; *p++ = 0; *p++ = 0;
    put(p, body, size - 36);
    for (i = 0; i < size; ++i) sum += table[i];
    table[9] = (uint8_t)(0x100 - (sum & 0xff));
    if ((*(efi_fn *)((uint8_t *)bs + 24 + 37 * 8))((uint64_t)acpi_table_guid, 0, (uint64_t)&protocol, 0, 0, 0) || !protocol)
        return -3;
    if ((*(efi_fn *)protocol)((uint64_t)protocol, (uint64_t)table, size, (uint64_t)&key, 0, 0)) return -4;
    {
        static uint8_t osc[sizeof HyperwinOscSsdt], pcct[128];   /* the protocol may keep the buffers */
        uint32_t n = build_pcct(pcct);
        for (i = 0; i < sizeof osc; ++i) osc[i] = HyperwinOscSsdt[i];
        key = 0;
        if ((*(efi_fn *)protocol)((uint64_t)protocol, (uint64_t)osc, sizeof osc, (uint64_t)&key, 0, 0)) return -5;
        key = 0;
        if ((*(efi_fn *)protocol)((uint64_t)protocol, (uint64_t)pcct, n, (uint64_t)&key, 0, 0)) return -6;
#ifdef HWGPU_ACPI_DEVICE   /* guest/efi/gpu_ssdt.asl: off until the driver takes that device (wddm/README.md) */
        static uint8_t gpu[sizeof HyperwinGpuSsdt];
        for (i = 0; i < sizeof gpu; ++i) gpu[i] = HyperwinGpuSsdt[i];
        key = 0;
        if ((*(efi_fn *)protocol)((uint64_t)protocol, (uint64_t)gpu, sizeof gpu, (uint64_t)&key, 0, 0)) return -7;
#endif
    }
    return (int64_t)cpus;
}

#ifdef HWCPU_SSDT_TEST
#include <stdio.h>
/* Host test: writes the SSDT for 8 processors at 2802 MHz to argv[1], for iasl -d. */
int main(int argc, char **argv)
{
    static uint8_t body[sizeof(((uint8_t[36 + 64 * 420]){0}))];
    uint8_t *p = body + 16, out[36 + 64 * 420];
    uint32_t i, size;
    FILE *f;
    (void)argc;
    p = put(p, "\\_SB_", 5);
    for (i = 0; i < 8; ++i) p = cpu_device(p, i, i, PERF, PERF_LOW, 2802, 280);
    p = wrap(body, (const uint8_t *)"\x10", 1, body + 16, (uint32_t)(p - body - 16));
    size = 36 + (uint32_t)(p - body);
    for (i = 0; i < 36; ++i) out[i] = 0;
    put(out, "SSDT", 4); out[4] = (uint8_t)size; out[5] = (uint8_t)(size >> 8); out[8] = 2;
    put(out + 10, "HYPRWN", 6); put(out + 16, "HWCPU   ", 8);
    put(out + 36, body, size - 36);
    { uint32_t s = 0; for (i = 0; i < size; ++i) s += out[i]; out[9] = (uint8_t)(0x100 - (s & 0xff)); }
    f = fopen(argv[1], "wb"); fwrite(out, 1, size, f); fclose(f);
    return 0;
}
#endif
