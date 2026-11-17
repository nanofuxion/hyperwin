/* Builds the HWPC0001 SSDT for hwpci.sys from the live FDT and installs it.
 * Contract: guest/drivers/hwpci/CONTRACT.md. Windows on ARM only enumerates ECAM,
 * and AVF's crosvm exposes a CAM window, so Windows gets the bus from hwpci.sys instead. */
#include <stdint.h>

typedef uint64_t (*efi_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

void *memset(void *d, int c, unsigned long long n)
{
    uint8_t *p = d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}

#ifndef HWPCI_NO_IRQ_CRS
#define HWPCI_NO_IRQ_CRS 0
#endif
#define MAX_IRQ 32
#define MAX_ROUTE 128

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static int has_compat(const uint8_t *v, uint32_t len, const char *want)
{
    uint32_t i = 0;
    while (i < len) {
        if (streq((const char *)v + i, want)) return 1;
        while (i < len && v[i]) ++i;
        ++i;
    }
    return 0;
}

struct node_props {
    const uint8_t *reg, *map, *mask;
    uint32_t reg_len, map_len, mask_len;
    uint32_t addr_cells, size_cells, int_cells, phandle;
    int is_pci, has_addr_cells;
};

struct pci_info {
    uint64_t cam_base, cam_size;
    uint32_t irq[MAX_IRQ], irq_flags[MAX_IRQ], nirq;
    uint8_t route[MAX_ROUTE][3];
    uint32_t nroute;
};

/* Calls visit for every node with its properties and the #address-cells/#size-cells of its parent. */
static int walk(const uint8_t *fdt, int (*visit)(struct node_props *, uint32_t, uint32_t, void *), void *ctx)
{
    uint32_t off_struct = be32(fdt + 8), off_strings = be32(fdt + 12);
    const uint8_t *p = fdt + off_struct, *strings = fdt + off_strings;
    static struct node_props stack[16];
    int depth = -1;
    for (;;) {
        uint32_t tok = be32(p);
        p += 4;
        if (tok == 1) {
            const char *name = (const char *)p;
            uint32_t n = 0;
            while (name[n]) ++n;
            p += (n + 4) & ~3u;
            if (++depth >= 16) return -1;
            stack[depth] = (struct node_props){ 0 };
            stack[depth].addr_cells = 2;
            stack[depth].size_cells = 1;
        } else if (tok == 3) {
            uint32_t len = be32(p), nameoff = be32(p + 4);
            const uint8_t *v = p + 8;
            const char *pn = (const char *)strings + nameoff;
            struct node_props *n = &stack[depth];
            if (streq(pn, "#address-cells") && len == 4) { n->addr_cells = be32(v); n->has_addr_cells = 1; }
            else if (streq(pn, "#size-cells") && len == 4) n->size_cells = be32(v);
            else if (streq(pn, "#interrupt-cells") && len == 4) n->int_cells = be32(v);
            else if (streq(pn, "phandle") && len == 4) n->phandle = be32(v);
            else if (streq(pn, "reg")) { n->reg = v; n->reg_len = len; }
            else if (streq(pn, "interrupt-map")) { n->map = v; n->map_len = len; }
            else if (streq(pn, "interrupt-map-mask")) { n->mask = v; n->mask_len = len; }
            else if (streq(pn, "compatible") && has_compat(v, len, "pci-host-cam-generic")) n->is_pci = 1;
            p += 8 + ((len + 3) & ~3u);
        } else if (tok == 2) {
            uint32_t pa = depth > 0 ? stack[depth - 1].addr_cells : 2, ps = depth > 0 ? stack[depth - 1].size_cells : 1;
            if (visit(&stack[depth], pa, ps, ctx)) return 0;
            --depth;
        } else if (tok == 4) {
        } else {
            return tok == 9 ? 1 : -1;
        }
    }
}

struct find_pci { struct node_props pci; uint32_t parent_ac, parent_sc; int found; };
static int visit_pci(struct node_props *n, uint32_t pa, uint32_t ps, void *ctx)
{
    struct find_pci *f = ctx;
    if (!n->is_pci) return 0;
    f->pci = *n; f->parent_ac = pa; f->parent_sc = ps; f->found = 1;
    return 1;
}

struct find_phandle { uint32_t phandle, addr_cells, int_cells; int found; };
static int visit_phandle(struct node_props *n, uint32_t pa, uint32_t ps, void *ctx)
{
    struct find_phandle *f = ctx;
    (void)pa; (void)ps;
    if (n->phandle != f->phandle) return 0;
    f->addr_cells = n->has_addr_cells ? n->addr_cells : 0;
    f->int_cells = n->int_cells;
    f->found = 1;
    return 1;
}

static uint64_t cells(const uint8_t *p, uint32_t n)
{
    uint64_t v = 0;
    while (n--) { v = v << 32 | be32(p); p += 4; }
    return v;
}

static int parse(const uint8_t *fdt, struct pci_info *out)
{
    struct find_pci f = { 0 };
    const uint8_t *m, *end;
    uint32_t mask[4] = { 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu };
    if (be32(fdt) != 0xd00dfeedu || walk(fdt, visit_pci, &f) < 0 || !f.found) return -1;
    if (!f.pci.reg || f.pci.reg_len < 4 * (f.parent_ac + f.parent_sc)) return -2;
    out->cam_base = cells(f.pci.reg, f.parent_ac);
    out->cam_size = cells(f.pci.reg + 4 * f.parent_ac, f.parent_sc);
    if (f.pci.mask && f.pci.mask_len == 16) {
        uint32_t i;
        for (i = 0; i < 4; ++i) mask[i] = be32(f.pci.mask + 4 * i);
    }
    if (!f.pci.map) return 0;
    m = f.pci.map;
    end = m + f.pci.map_len;
    while (m + 20 <= end) {
        struct find_phandle ph = { 0 };
        uint32_t hi = be32(m) & mask[0], pin = be32(m + 12) & mask[3], spi, flags, k;
        ph.phandle = be32(m + 16);
        if (walk(fdt, visit_phandle, &ph) < 0 || !ph.found || ph.int_cells < 3) return -3;
        m += 20 + 4 * ph.addr_cells;
        if (m + 4 * ph.int_cells > end) return -4;
        spi = be32(m + 4) + (be32(m) == 0 ? 32u : 16u);
        flags = be32(m + 8);
        m += 4 * ph.int_cells;
        for (k = 0; k < out->nirq && out->irq[k] != spi; ++k) {}
        if (k == out->nirq) {
            if (out->nirq == MAX_IRQ) return -5;
            out->irq[k] = spi;
            out->irq_flags[k] = flags;
            ++out->nirq;
        }
        if (out->nroute == MAX_ROUTE) return -6;
        out->route[out->nroute][0] = (uint8_t)((hi >> 11) & 0x1f);
        out->route[out->nroute][1] = (uint8_t)pin;
        out->route[out->nroute][2] = (uint8_t)k;
        ++out->nroute;
    }
    return 0;
}

/* PkgLength for a body of n bytes: returns encoded size, writes it to dst. */
static uint32_t pkglen(uint8_t *dst, uint32_t n)
{
    uint32_t total;
    if (n + 1 < 64) { dst[0] = (uint8_t)(n + 1); return 1; }
    if (n + 2 < 0x1000) { total = n + 2; dst[0] = 0x40 | (total & 15); dst[1] = (uint8_t)(total >> 4); return 2; }
    total = n + 3;
    dst[0] = 0x80 | (total & 15); dst[1] = (uint8_t)(total >> 4); dst[2] = (uint8_t)(total >> 12);
    return 3;
}

static uint8_t *put(uint8_t *p, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    while (n--) *p++ = *s++;
    return p;
}

static uint8_t *put64(uint8_t *p, uint64_t v)
{
    uint32_t i;
    for (i = 0; i < 8; ++i) *p++ = (uint8_t)(v >> (8 * i));
    return p;
}

/* Wraps body (already at dst+room) with an opcode prefix and PkgLength, moving the body down. */
static uint32_t wrap(uint8_t *dst, const uint8_t *op, uint32_t oplen, const uint8_t *body, uint32_t n)
{
    uint8_t len[3];
    uint32_t l = pkglen(len, n), i;
    static uint8_t tmp[2048];
    if (n > sizeof tmp) return 0;
    for (i = 0; i < n; ++i) tmp[i] = body[i];
    dst = put(dst, op, oplen);
    dst = put(dst, len, l);
    put(dst, tmp, n);
    return oplen + l + n;
}

static uint32_t build_ssdt(uint8_t *out, const struct pci_info *pci)
{
    static uint8_t crs[46 + 9 * MAX_IRQ + 2], dev[1536], body[1600];
    uint8_t *p;
    uint32_t ncrs, i, ndev, n, sum = 0;
    p = crs;
    if (pci->cam_base + pci->cam_size <= 0x100000000ull) {
        /* Memory32Fixed, ReadWrite */
        *p++ = 0x86; *p++ = 9; *p++ = 0; *p++ = 1;
        for (i = 0; i < 4; ++i) *p++ = (uint8_t)(pci->cam_base >> (8 * i));
        for (i = 0; i < 4; ++i) *p++ = (uint8_t)(pci->cam_size >> (8 * i));
    } else {
        *p++ = 0x8a; *p++ = 43; *p++ = 0;
        *p++ = 0; *p++ = 0x0d; *p++ = 0x01;
        p = put64(p, 0);
        p = put64(p, pci->cam_base);
        p = put64(p, pci->cam_base + pci->cam_size - 1);
        p = put64(p, 0);
        p = put64(p, pci->cam_size);
    }
    for (i = 0; i < pci->nirq && !HWPCI_NO_IRQ_CRS; ++i) {
        uint32_t f = pci->irq_flags[i];
        uint8_t flags = 0x01 | 0x08;
        if (f & 3) flags |= 0x02;
        if (f & 0xa) flags |= 0x04;
        *p++ = 0x89; *p++ = 6; *p++ = 0; *p++ = flags; *p++ = 1;
        *p++ = (uint8_t)pci->irq[i]; *p++ = (uint8_t)(pci->irq[i] >> 8);
        *p++ = (uint8_t)(pci->irq[i] >> 16); *p++ = (uint8_t)(pci->irq[i] >> 24);
    }
    *p++ = 0x79; *p++ = 0;
    ncrs = (uint32_t)(p - crs);

    p = dev;
    p = put(p, "HWPC", 4);
    p = put(p, "\x08_HID\x0dHWPC0001", 14); *p++ = 0;
    p = put(p, "\x08_UID\x00", 6);
    p = put(p, "\x08_CCA\x01", 6);
    p = put(p, "\x08_CRS", 5);
    {
        static uint8_t buf[sizeof crs + 4];
        uint8_t *b = buf;
        if (ncrs < 256) { *b++ = 0x0a; *b++ = (uint8_t)ncrs; }
        else { *b++ = 0x0b; *b++ = (uint8_t)ncrs; *b++ = (uint8_t)(ncrs >> 8); }
        b = put(b, crs, ncrs);
        p += wrap(p, (const uint8_t *)"\x11", 1, buf, (uint32_t)(b - buf));
    }
    if (pci->nroute) {
        /* _PRT: Package { Package { slot << 16 | 0xFFFF, pin (0 = INTA), 0, GSIV } ... }, so Windows routes
         * the children's INTx through its own interrupt arbiter. */
        static uint8_t prt[2 + MAX_ROUTE * 24];
        uint8_t *q = prt;
        *q++ = (uint8_t)pci->nroute;
        for (i = 0; i < pci->nroute; ++i) {
            static uint8_t ent[24];
            uint8_t *e = ent;
            uint32_t addr = (uint32_t)pci->route[i][0] << 16 | 0xffff, gsiv = pci->irq[pci->route[i][2]];
            uint32_t k;
            *e++ = 4;
            *e++ = 0x0c; for (k = 0; k < 4; ++k) *e++ = (uint8_t)(addr >> (8 * k));
            *e++ = 0x0a; *e++ = (uint8_t)(pci->route[i][1] ? pci->route[i][1] - 1 : 0);
            *e++ = 0x00;
            *e++ = 0x0c; for (k = 0; k < 4; ++k) *e++ = (uint8_t)(gsiv >> (8 * k));
            q += wrap(q, (const uint8_t *)"\x12", 1, ent, (uint32_t)(e - ent));
        }
        p = put(p, "\x08_PRT", 5);
        p += wrap(p, (const uint8_t *)"\x12", 1, prt, (uint32_t)(q - prt));
    }
    if (pci->nroute) {
        static uint8_t pkg[1 + MAX_ROUTE * 6];
        uint8_t *b = pkg;
        *b++ = (uint8_t)(pci->nroute * 3);
        for (i = 0; i < pci->nroute; ++i) {
            *b++ = 0x0a; *b++ = pci->route[i][0];
            *b++ = 0x0a; *b++ = pci->route[i][1];
            *b++ = 0x0a; *b++ = pci->route[i][2];
        }
        p = put(p, "\x08RTAB", 5);
        p += wrap(p, (const uint8_t *)"\x12", 1, pkg, (uint32_t)(b - pkg));
    }
    ndev = (uint32_t)(p - dev);

    n = wrap(body + 5, (const uint8_t *)"\x5b\x82", 2, dev, ndev);
    {
        static uint8_t scope[1600];
        uint32_t k;
        scope[0] = '\\'; scope[1] = '_'; scope[2] = 'S'; scope[3] = 'B'; scope[4] = '_';
        for (k = 0; k < n; ++k) scope[5 + k] = body[5 + k];
        n = wrap(body, (const uint8_t *)"\x10", 1, scope, n + 5);
    }

    p = out;
    p = put(p, "SSDT", 4);
    i = 36 + n;
    *p++ = (uint8_t)i; *p++ = (uint8_t)(i >> 8); *p++ = 0; *p++ = 0;
    *p++ = 2; *p++ = 0;
    p = put(p, "HYPRWN", 6);
    p = put(p, "HWPCI   ", 8);
    *p++ = 1; *p++ = 0; *p++ = 0; *p++ = 0;
    p = put(p, "HYWN", 4);
    *p++ = 1; *p++ = 0; *p++ = 0; *p++ = 0;
    p = put(p, body, n);
    for (i = 0; i < 36 + n; ++i) sum += out[i];
    out[9] = (uint8_t)(0x100 - (sum & 0xff));
    return 36 + n;
}

/* Returns 0 on success, otherwise a negative stage code. */
int64_t HyperwinInstallPciSsdt(void *system)
{
    static const uint8_t fdt_guid[16] = {
        0xd5, 0x21, 0xb6, 0xb1, 0x9c, 0xf1, 0xa5, 0x41, 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0
    };
    static const uint8_t acpi_table_guid[16] = {
        0xdd, 0x6b, 0xe0, 0xff, 0x07, 0x61, 0xa6, 0x46, 0x7b, 0xb2, 0x5a, 0x9c, 0x7e, 0xc5, 0x27, 0x5c
    };
    static struct pci_info pci;
    static uint8_t table[4096];
    void *bs = *(void **)((uint8_t *)system + 0x60);
    uint64_t n = *(uint64_t *)((uint8_t *)system + 0x68), i, key = 0;
    const uint8_t *tables = *(const uint8_t **)((uint8_t *)system + 0x70), *fdt = 0;
    void *protocol = 0;
    uint32_t size;
    int rc;
    for (i = 0; i < n && i < 64 && !fdt; ++i) {
        uint32_t k;
        for (k = 0; k < 16 && tables[i * 24 + k] == fdt_guid[k]; ++k) {}
        if (k == 16) fdt = *(const uint8_t **)(tables + i * 24 + 16);
    }
    if (!fdt) return -10;
    pci = (struct pci_info){ 0 };
    rc = parse(fdt, &pci);
    if (rc) return rc - 20;
    size = build_ssdt(table, &pci);
    if (!size) return -30;
    if (*(efi_fn *)((uint8_t *)bs + 24 + 37 * 8) == 0) return -40;
    if ((*(efi_fn *)((uint8_t *)bs + 24 + 37 * 8))((uint64_t)acpi_table_guid, 0, (uint64_t)&protocol, 0, 0, 0) || !protocol)
        return -41;
    if ((*(efi_fn *)protocol)((uint64_t)protocol, (uint64_t)table, size, (uint64_t)&key, 0, 0)) return -42;
    return (int64_t)(pci.nirq << 8 | pci.nroute);
}

#ifdef HWPCI_SSDT_TEST
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    static uint8_t fdt[1 << 20], out[4096];
    struct pci_info pci = { 0 };
    FILE *f = fopen(argv[1], "rb");
    size_t len = fread(fdt, 1, sizeof fdt, f);
    int rc = parse(fdt, &pci);
    uint32_t i, size;
    (void)len;
    printf("parse=%d cam=%llx/%llx irqs=%u routes=%u\n", rc, (unsigned long long)pci.cam_base,
        (unsigned long long)pci.cam_size, pci.nirq, pci.nroute);
    for (i = 0; i < pci.nirq; ++i) printf(" gsiv %u flags %x\n", pci.irq[i], pci.irq_flags[i]);
    for (i = 0; i < pci.nroute; ++i) printf(" dev %u pin %u -> %u\n", pci.route[i][0], pci.route[i][1], pci.route[i][2]);
    size = build_ssdt(out, &pci);
    f = fopen(argv[2], "wb");
    fwrite(out, 1, size, f);
    fclose(f);
    return rc;
}
#endif
