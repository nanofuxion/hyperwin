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

static uint8_t *cpu_device(uint8_t *p, uint32_t index, uint32_t uid, uint32_t mhz)
{
    static const char hex[] = "0123456789ABCDEF";
    static uint8_t dev[512], pkg[400];
    uint8_t *d = dev, *k = pkg;
    int64_t desired = 16 + 16 * (int64_t)index, limited = desired + 8;
    *k++ = 23;                                            /* elements */
    k = dword(k, 23);                                     /* NumEntries */
    k = dword(k, 3);                                      /* Revision */
    k = dword(k, PERF);                                   /* Highest */
    k = dword(k, PERF);                                   /* Nominal */
    k = dword(k, PERF_LOW);                               /* Lowest nonlinear */
    k = dword(k, PERF_LOW);                               /* Lowest */
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
    k = dword(k, PERF);                                   /* Reference performance */
    k = dword(k, mhz * PERF_LOW / PERF);                  /* Lowest frequency, MHz */
    k = dword(k, mhz);                                    /* Nominal frequency, MHz */
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

/* The MADT's enabled GIC CPU interfaces, as ACPI processor UIDs. */
static uint32_t processor_uids(void *system, uint32_t *uid, uint32_t max)
{
    static const uint8_t acpi20[16] = {
        0x71, 0xe8, 0x68, 0x88, 0xf1, 0xe4, 0xd3, 0x11, 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81
    };
    uint64_t n = *(uint64_t *)((uint8_t *)system + 0x68), i, xsdt = 0, count;
    uint8_t *tables = *(uint8_t **)((uint8_t *)system + 0x70);
    uint32_t found = 0;
    for (i = 0; i < n && i < 64 && !xsdt; ++i)
        if (guid_is(tables + i * 24, acpi20)) {
            const uint8_t *rsdp = *(const uint8_t **)(tables + i * 24 + 16);
            if (rsdp[15] >= 2) xsdt = *(const uint64_t *)(rsdp + 24);
        }
    if (!xsdt) return 0;
    count = (*(const uint32_t *)(xsdt + 4) - 36) / 8;
    for (i = 0; i < count && i < 64; ++i) {
        const uint8_t *t = *(const uint8_t **)(xsdt + 36 + i * 8);
        uint32_t len, off;
        if (t[0] != 'A' || t[1] != 'P' || t[2] != 'I' || t[3] != 'C') continue;
        len = *(const uint32_t *)(t + 4);
        for (off = 44; off + 2 <= len && t[off + 1]; off += t[off + 1]) {
            if (t[off] == 0x0b && t[off + 1] >= 16 && (*(const uint32_t *)(t + off + 12) & 1) && found < max)
                uid[found++] = *(const uint32_t *)(t + off + 8);
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
    const struct hw_header *h = (const struct hw_header *)HW_WINDOW_PA;
    void *bs = *(void **)((uint8_t *)system + 0x60), *protocol = 0;
    uint32_t cpus, i, size, sum = 0, mhz;
    uint64_t key = 0;
    uint8_t *p;
    if (h->magic[0] != 'H' || h->magic[1] != 'Y' || h->header_bytes < sizeof *h || !h->cpu_mhz) return -1;
    mhz = h->cpu_mhz;
    cpus = processor_uids(system, uid, HW_CPPC_MAX_CPUS);
    if (!cpus) return -2;
    p = body + 16;                                        /* devices first, then wrapped in Scope */
    p = put(p, "\\_SB_", 5);
    for (i = 0; i < cpus; ++i) p = cpu_device(p, i, uid[i], mhz);
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
    for (i = 0; i < 8; ++i) p = cpu_device(p, i, i, 2802);
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
