#include "hw_abi.h"

typedef uint64_t (*efi_output_string)(void *protocol, const uint16_t *string);
typedef uint64_t (*efi_locate_protocol)(const void *guid, void *registration, void **interface);
typedef uint64_t (*efi_handle_protocol)(void *handle, const void *guid, void **interface);
typedef uint64_t (*efi_free_pool)(void *buffer);

struct efi_guid { uint32_t a; uint16_t b, c; uint8_t d[8]; };
struct efi_config_table { struct efi_guid guid; void *table; };
#pragma pack(push, 1)
struct gop_memory_path {
    uint8_t type, subtype;
    uint16_t length;
    uint32_t memory_type;
    uint64_t start, end;
    uint8_t end_type, end_subtype;
    uint16_t end_length;
};
struct acpi_header {
    uint32_t signature, length;
    uint8_t revision, checksum;
    char oem_id[6], oem_table_id[8];
    uint32_t oem_revision, creator_id, creator_revision;
};
struct acpi_rsdp_v2 {
    uint8_t signature[8], checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address, length;
    uint64_t xsdt_address;
    uint8_t extended_checksum, reserved[3];
};
#pragma pack(pop)
_Static_assert(sizeof(struct acpi_header) == 36, "ACPI header layout");
_Static_assert(sizeof(struct acpi_rsdp_v2) == 36, "RSDP 2.0 layout");
_Static_assert(sizeof(struct gop_memory_path) == 28, "GOP memory device path layout");
struct gop_pixel { uint8_t blue, green, red, reserved; };
struct gop_info {
    uint32_t version, width, height, pixel_format;
    uint32_t red_mask, green_mask, blue_mask, reserved_mask;
    uint32_t pixels_per_scanline;
};
struct gop_mode {
    uint32_t max_mode, mode;
    struct gop_info *info;
    uint64_t size_of_info, frame_buffer_base, frame_buffer_size;
};
struct gop_protocol;
typedef uint64_t (*gop_query_mode_fn)(struct gop_protocol *, uint32_t, uint64_t *, struct gop_info **);
typedef uint64_t (*gop_set_mode_fn)(struct gop_protocol *, uint32_t);
typedef uint64_t (*gop_blt_fn)(struct gop_protocol *, struct gop_pixel *, uint32_t,
                               uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
struct gop_protocol { gop_query_mode_fn query_mode; gop_set_mode_fn set_mode; gop_blt_fn blt; struct gop_mode *mode; };

static const struct efi_guid gop_guid = {
    0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}
};
static const struct efi_guid device_path_guid = {
    0x09576e91, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};
static const struct efi_guid simple_text_output_guid = {
    0x387477c1, 0x69c7, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};
static const struct efi_guid acpi2_guid = {
    0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}
};
static const struct efi_guid acpi_guid = {
    0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}
};
static const struct efi_guid fdt_guid = {
    0xb1b621d5, 0xf19c, 0x41a5, {0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0}
};
static void *console_out;

static void output(const char *text)
{
    efi_output_string write = *(efi_output_string *)((uint8_t *)console_out + 8);
    uint16_t wide[96];
    while (*text) {
        uint32_t n = 0;
        while (*text && n + 1 < sizeof(wide) / sizeof(wide[0])) wide[n++] = (uint8_t)*text++;
        wide[n] = 0;
        write(console_out, wide);
    }
}

static void hex(uint64_t value)
{
    char digits[17];
    int i;
    for (i = 15; i >= 0; --i) {
        digits[i] = "0123456789abcdef"[value & 15];
        value >>= 4;
    }
    digits[16] = 0;
    output(digits);
}

static void hex_byte(uint8_t value)
{
    const char digits[] = "0123456789abcdef";
    char text[3] = {digits[value >> 4], digits[value & 15], 0};
    output(text);
}

static int same_guid(const struct efi_guid *a, const struct efi_guid *b)
{
    uint32_t i;
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (i = 0; i < sizeof(*a); ++i) if (pa[i] != pb[i]) return 0;
    return 1;
}

static uint8_t sum8(const void *buffer, uint32_t bytes)
{
    const uint8_t *p = buffer;
    uint32_t i;
    uint8_t sum = 0;
    for (i = 0; i < bytes; ++i) sum = (uint8_t)(sum + p[i]);
    return sum;
}

static int valid_acpi_table(const void *table, const char *signature, uint32_t min_length)
{
    const struct acpi_header *h = table;
    uint32_t i;
    if (!table || h->length < min_length || h->length > 65536u) return 0;
    for (i = 0; i < 4; ++i) if (((const uint8_t *)&h->signature)[i] != (uint8_t)signature[i]) return 0;
    return sum8(table, h->length) == 0;
}

static int report_acpi(void *system)
{
    uint64_t count = *(uint64_t *)((uint8_t *)system + 104);
    struct efi_config_table *tables = *(struct efi_config_table **)((uint8_t *)system + 112);
    uint64_t i, limit = count < 128 ? count : 128;
    for (i = 0; tables && i < limit; ++i) {
        if (same_guid(&tables[i].guid, &acpi2_guid) || same_guid(&tables[i].guid, &acpi_guid)) {
            struct acpi_rsdp_v2 *r = tables[i].table;
            struct acpi_header *xsdt;
            uint32_t entries, n;
            int have_fadt = 0, have_madt = 0, have_gtdt = 0, have_mcfg = 0, have_ssdt = 0;
            if (!r) { output("ACPI_RSDP_NULL\r\n"); return 0; }
            if (r->signature[0] != 'R' || r->signature[1] != 'S' || r->signature[2] != 'D' ||
                r->signature[3] != ' ' || r->signature[4] != 'P' || r->signature[5] != 'T' ||
                r->signature[6] != 'R' || r->signature[7] != ' ') {
                output("ACPI_RSDP_SIGNATURE_INVALID\r\n");
                return 0;
            }
            if (r->revision < 2 || r->length < sizeof(*r) || r->length > 4096) {
                output("ACPI_RSDP_LENGTH_OR_REVISION_INVALID\r\n");
                return 0;
            }
            if (sum8(r, 20) || sum8(r, r->length)) {
                output("ACPI_RSDP_CHECKSUM_INVALID legacy="); hex(sum8(r, 20));
                output(" extended="); hex(sum8(r, r->length)); output("\r\n");
                return 0;
            }
            xsdt = (struct acpi_header *)(uint64_t)r->xsdt_address;
            if (!valid_acpi_table(xsdt, "XSDT", sizeof(*xsdt) + 8u) || (xsdt->length - sizeof(*xsdt)) % 8u) {
                output("ACPI_XSDT_INVALID\r\n");
                return 0;
            }
            entries = (xsdt->length - sizeof(*xsdt)) / 8u;
            if (entries > 64) { output("ACPI_XSDT_TOO_MANY_TABLES\r\n"); return 0; }
            for (n = 0; n < entries; ++n) {
                const uint8_t *entry = (const uint8_t *)xsdt + sizeof(*xsdt) + n * 8u;
                struct acpi_header *h = (struct acpi_header *)(uint64_t)*(const uint64_t *)entry;
                if (!h || h->length < sizeof(*h) || h->length > 65536u || sum8(h, h->length)) continue;
                if (valid_acpi_table(h, "FACP", 276u)) have_fadt = 1;
                else if (valid_acpi_table(h, "APIC", 44u)) have_madt = 1;
                else if (valid_acpi_table(h, "GTDT", 104u)) have_gtdt = 1;
                else if (valid_acpi_table(h, "MCFG", 60u)) have_mcfg = 1;
                else if (valid_acpi_table(h, "SSDT", 36u)) have_ssdt = 1;
            }
            if (!have_fadt || !have_madt || !have_gtdt || !have_mcfg || !have_ssdt) {
                output("ACPI_REQUIRED_TABLE_MISSING\r\n");
                return 0;
            }
            {
                struct acpi_header *fadt = 0;
                for (n = 0; n < entries; ++n) {
                    const uint8_t *entry = (const uint8_t *)xsdt + sizeof(*xsdt) + n * 8u;
                    struct acpi_header *h = (struct acpi_header *)(uint64_t)*(const uint64_t *)entry;
                    if (valid_acpi_table(h, "FACP", 276u)) { fadt = h; break; }
                }
                if (!fadt || !*(const uint64_t *)((const uint8_t *)fadt + 140) ||
                    !valid_acpi_table((const void *)(uint64_t)*(const uint64_t *)((const uint8_t *)fadt + 140), "DSDT", 36u) ||
                    !(*(const uint32_t *)((const uint8_t *)fadt + 112) & (1u << 20)) ||
                    !(*(const uint16_t *)((const uint8_t *)fadt + 129) & 1u)) {
                    output("ACPI_FADT_DSDT_OR_PSCI_INVALID\r\n");
                    return 0;
                }
            }
            output("ACPI_TABLES_VALID ");
            hex((uint64_t)tables[i].table);
            output("\r\n");
            return 1;
        }
    }
    output("NO_ACPI_CONFIGURATION_TABLE\r\n");
    return 0;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t align4(uint32_t n) { return (n + 3u) & ~3u; }

static int contains(const char *text, const char *needle)
{
    uint32_t i, j;
    if (!*needle) return 1;
    for (; *text; ++text) {
        for (i = 0, j = 0; needle[j] && text[i] == needle[j]; ++i, ++j) {}
        if (!needle[j]) return 1;
    }
    return 0;
}

static int relevant_node(const char *name)
{
    static const char *const keys[] = {
        "gic", "intc", "timer", "pcie", "pci", "memory", "cpu", "virtio", "uart", "serial", "interrupt", "psci"
    };
    uint32_t i;
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) if (contains(name, keys[i])) return 1;
    return 0;
}

static int relevant_property(const char *name)
{
    static const char *const keys[] = {
        "compatible", "reg", "interrupts", "interrupt-parent", "ranges", "#address-cells",
        "#size-cells", "#interrupt-cells", "device_type", "status", "clock-frequency",
        "phandle", "msi-parent", "bus-range", "dma-ranges", "interrupt-map", "interrupt-map-mask", "method"
    };
    uint32_t i;
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) if (contains(name, keys[i])) return 1;
    return 0;
}

static int printable_property(const char *name)
{
    return contains(name, "compatible") || contains(name, "device_type") || contains(name, "status");
}

static void dump_fdt_summary(void *system)
{
    uint64_t count = *(uint64_t *)((uint8_t *)system + 104);
    struct efi_config_table *tables = *(struct efi_config_table **)((uint8_t *)system + 112);
    uint64_t i, limit = count < 128 ? count : 128;
    const uint8_t *fdt = 0;
    uint32_t total, struct_off, strings_off, struct_size, strings_size, pos, end, depth = 0;
    uint8_t active[64] = {0};
    for (i = 0; tables && i < limit; ++i) if (same_guid(&tables[i].guid, &fdt_guid)) {
        fdt = tables[i].table;
        break;
    }
    if (!fdt) {
        output("NO_FDT_CONFIGURATION_TABLE\r\n");
        return;
    }
    total = be32(fdt + 4);
    struct_off = be32(fdt + 8);
    strings_off = be32(fdt + 12);
    strings_size = be32(fdt + 32);
    struct_size = be32(fdt + 36);
    if (be32(fdt) != 0xd00dfeedu || total < 40 || total > 1024 * 1024 ||
        struct_off > total || struct_size > total - struct_off ||
        strings_off > total || strings_size > total - strings_off) {
        output("FDT_HEADER_INVALID\r\n");
        return;
    }
    output("FDT_TABLE ");
    hex((uint64_t)fdt);
    output(" size ");
    hex(total);
    output("\r\n");
    pos = struct_off;
    end = struct_off + struct_size;
    while (pos + 4 <= end) {
        uint32_t token = be32(fdt + pos);
        pos += 4;
        if (token == 1) {
            uint32_t start = pos, n;
            char name[96];
            while (pos < end && fdt[pos]) ++pos;
            n = pos - start;
            if (pos >= end || depth + 1 >= sizeof(active)) break;
            if (n >= sizeof(name)) n = sizeof(name) - 1;
            for (i = 0; i < n; ++i) name[i] = (char)fdt[start + i];
            name[n] = 0;
            ++pos;
            pos = align4(pos);
            active[depth + 1] = (uint8_t)(active[depth] || relevant_node(name));
            ++depth;
            output("FDT_NODE ");
            output(name);
            output("\r\n");
        } else if (token == 2) {
            if (!depth) break;
            --depth;
        } else if (token == 3) {
            uint32_t length, name_off, value, j, name_len;
            char prop[64];
            if (pos + 8 > end) break;
            length = be32(fdt + pos);
            name_off = be32(fdt + pos + 4);
            pos += 8;
            value = pos;
            if (length > end - pos || name_off >= strings_size) break;
            pos += align4(length);
            if (pos > end) break;
            if (!active[depth]) continue;
            name_len = 0;
            while (name_off + name_len < strings_size && fdt[strings_off + name_off + name_len] && name_len + 1 < sizeof(prop)) {
                prop[name_len] = (char)fdt[strings_off + name_off + name_len];
                ++name_len;
            }
            prop[name_len] = 0;
            if (!relevant_property(prop)) continue;
            output("FDT_PROP ");
            output(prop);
            if (printable_property(prop)) {
                output(" ");
                for (j = 0; j < length && fdt[value + j]; ++j) {
                    char c[2] = {(char)fdt[value + j], 0};
                    output(c);
                }
            } else {
                uint32_t bytes = length < 96 ? length : 96;
                output(" len=");
                hex(length);
                output(" data=");
                for (j = 0; j < bytes; ++j) hex_byte(fdt[value + j]);
                if (bytes < length) output("...");
            }
            output("\r\n");
        } else if (token == 4) {
            continue;
        } else if (token == 9) {
            break;
        } else {
            output("FDT_TOKEN_INVALID\r\n");
            break;
        }
    }
}

/* Which layout does the PCI window at the FDT's reg really decode? Vendor/device
 * words only, bus 0 fn 0; 0xffffffff means nothing answered. Base is the FDT pci reg. */
static void pci_layout_probe(void)
{
    volatile uint32_t *base = (volatile uint32_t *)0x2e000000ull;
    uint32_t d;
    output("PCI_PROBE\r\n");
    for (d = 0; d < 8; ++d) {
        output("dev "); hex_byte((uint8_t)d);
        output(" CAM "); hex(base[(d << 11) / 4]);
        output(" ECAM "); hex(base[(d << 15) / 4]);
        output("\r\n");
    }
}

uint64_t GopCheckEntry(void *image, void *system)
{
    void *bs = *(void **)((uint8_t *)system + 0x60);
    efi_locate_protocol locate = *(efi_locate_protocol *)((uint8_t *)bs + 24 + 37 * 8);
    efi_handle_protocol handle_protocol = *(efi_handle_protocol *)((uint8_t *)bs + 24 + 16 * 8);
    struct gop_protocol *gop = 0;
    uint64_t status;
    uint32_t i;
    static const struct gop_pixel colors[8] = {
        {0, 0, 0, 0}, {0, 0, 255, 0}, {0, 255, 0, 0}, {255, 0, 0, 0},
        {255, 255, 255, 0}, {0, 255, 255, 0}, {255, 0, 255, 0}, {255, 255, 0, 0}
    };
    (void)image;
    console_out = *(void **)((uint8_t *)system + 0x40);
    output("HYPERWIN GOP CHECK\r\n");
    pci_layout_probe();
    output("LocateProtocol ");
    dump_fdt_summary(system);
    if (!report_acpi(system)) return 0x8000000000000002ull;
    status = locate(&gop_guid, 0, (void **)&gop);
    hex(status);
    output("\r\n");
    if (status || !gop || !gop->mode || !gop->mode->info || !gop->blt) {
        output("GOP protocol unusable\r\n");
        return status ? status : 0x8000000000000002ull;
    }
    {
        void *con_out_handle = *(void **)((uint8_t *)system + 0x38);
        void *con_out_gop = 0;
        void *con_out_text = 0;
        struct gop_memory_path *path = 0;
        status = handle_protocol(con_out_handle, &gop_guid, &con_out_gop);
        if (status || con_out_gop != gop) {
            output("GOP not bound to system console handle\r\n");
            return status ? status : 0x8000000000000002ull;
        }
        status = handle_protocol(con_out_handle, &device_path_guid, (void **)&path);
        if (status || !path || path->type != 1 || path->subtype != 3 || path->length != 24 ||
            path->memory_type != 0 || path->start != gop->mode->frame_buffer_base ||
            path->end != gop->mode->frame_buffer_base + gop->mode->frame_buffer_size - 1 ||
            path->end_type != 0x7f || path->end_subtype != 0xff || path->end_length != 4) {
            output("GOP memory-mapped device path invalid\r\n");
            return status ? status : 0x8000000000000002ull;
        }
        status = handle_protocol(con_out_handle, &simple_text_output_guid, &con_out_text);
        if (status || con_out_text != console_out) {
            output("GOP console handle has no SimpleTextOut\r\n");
            return status ? status : 0x8000000000000002ull;
        }
        output("GOP memory-mapped device path and text console validated\r\n");
    }
    {
        uint64_t info_bytes = 0;
        struct gop_info *queried = 0;
        efi_free_pool free_pool = *(efi_free_pool *)((uint8_t *)bs + 24 + 6 * 8);
        status = gop->query_mode(gop, 0, &info_bytes, &queried);
        if (status || !queried || info_bytes < sizeof(*queried) ||
            queried->width != HW_WIDTH || queried->height != HW_HEIGHT ||
            queried->pixel_format != 1 || queried->pixels_per_scanline != HW_WIDTH) {
            output("QueryMode validation failed\r\n");
            if (queried) free_pool(queried);
            return status ? status : 0x8000000000000002ull;
        }
        free_pool(queried);
        output("QueryMode validated\r\n");
    }
    output("mode ");
    hex(gop->mode->info->width);
    output("x");
    hex(gop->mode->info->height);
    output(" format ");
    hex(gop->mode->info->pixel_format);
    output(" fb ");
    hex(gop->mode->frame_buffer_base);
    output("\r\n");
    status = gop->set_mode(gop, 0);
    if (status) {
        output("SetMode(0) failed\r\n");
        return status;
    }
    {
        const uint8_t *fb = (const uint8_t *)gop->mode->frame_buffer_base;
        uint64_t bytes = (uint64_t)HW_WIDTH * HW_HEIGHT * 4u;
        uint64_t j;
        for (j = 0; j < bytes; ++j) if (fb[j]) break;
        if (j != bytes) {
            output("SetMode black clear failed\r\n");
            return 0x8000000000000002ull;
        }
        output("SetMode black clear OK\r\n");
    }
    for (i = 0; i < 8; ++i) {
        uint64_t x0 = ((uint64_t)gop->mode->info->width * i) / 8;
        uint64_t x1 = ((uint64_t)gop->mode->info->width * (i + 1)) / 8;
        status = gop->blt(gop, (struct gop_pixel *)&colors[i], 0, 0, 0, x0, 0,
                          x1 - x0, gop->mode->info->height, 0);
        if (status) break;
    }
    if (status) {
        output("GOP Blt failed\r\n");
        return status;
    }
    {
        struct gop_pixel sampled = {0, 0, 0, 0};
        uint64_t red_x = (uint64_t)HW_WIDTH / 8 + 1;
        uint64_t blue_x = (uint64_t)HW_WIDTH * 3 / 8 + 1;
        status = gop->blt(gop, &sampled, 1, red_x, 8, 0, 0, 1, 1, 0);
        if (status || sampled.blue != 0 || sampled.green != 0 || sampled.red != 255 || sampled.reserved != 0) {
            output("GOP red readback failed\r\n");
            return status ? status : 0x8000000000000002ull;
        }
        sampled.blue = sampled.green = sampled.red = sampled.reserved = 0;
        status = gop->blt(gop, &sampled, 1, blue_x, 8, 0, 0, 1, 1, 0);
        if (status || sampled.blue != 255 || sampled.green != 0 || sampled.red != 0 || sampled.reserved != 0) {
            output("GOP blue readback failed\r\n");
            return status ? status : 0x8000000000000002ull;
        }
    }
    output("GOP red/blue readback OK (Reserved=0)\r\nGOP Blt eight bars OK\r\n");
    return status;
}
