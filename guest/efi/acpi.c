#include "hw_abi.h"
#include "acpi_tables.h"

typedef uint64_t efi_status;
typedef efi_status (*efi_allocate_pages)(uint32_t, uint32_t, uint64_t, uint64_t *);
typedef efi_status (*efi_install_configuration_table)(const void *, void *);

struct efi_guid { uint32_t a; uint16_t b, c; uint8_t d[8]; };
struct efi_config_table { struct efi_guid guid; void *table; };

#pragma pack(push, 1)
struct acpi_header {
    uint32_t signature, length;
    uint8_t revision, checksum;
    char oem_id[6], oem_table_id[8];
    uint32_t oem_revision, creator_id, creator_revision;
};
struct acpi_gas {
    uint8_t space_id, bit_width, bit_offset, access_size;
    uint64_t address;
};
struct acpi_fadt {
    struct acpi_header header;
    uint32_t firmware_ctrl, dsdt;
    uint8_t reserved0, preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable, acpi_disable, s4bios_req, pstate_cnt;
    uint32_t pm1a_evt_blk, pm1b_evt_blk, pm1a_cnt_blk, pm1b_cnt_blk;
    uint32_t pm2_cnt_blk, pm_tmr_blk, gpe0_blk, gpe1_blk;
    uint8_t pm1_evt_len, pm1_cnt_len, pm2_cnt_len, pm_tmr_len;
    uint8_t gpe0_blk_len, gpe1_blk_len, gpe1_base, cst_cnt;
    uint16_t p_lvl2_lat, p_lvl3_lat, flush_size, flush_stride;
    uint8_t duty_offset, duty_width, day_alrm, mon_alrm, century;
    uint16_t ia_pc_boot_arch;
    uint8_t reserved1;
    uint32_t flags;
    struct acpi_gas reset_reg;
    uint8_t reset_value;
    uint16_t arm_boot_arch;
    uint8_t minor_version;
    uint64_t x_firmware_ctrl, x_dsdt;
    struct acpi_gas x_pm1a_evt_blk, x_pm1b_evt_blk;
    struct acpi_gas x_pm1a_cnt_blk, x_pm1b_cnt_blk;
    struct acpi_gas x_pm2_cnt_blk, x_pm_tmr_blk;
    struct acpi_gas x_gpe0_blk, x_gpe1_blk;
    struct acpi_gas sleep_control_reg, sleep_status_reg;
    uint64_t hypervisor_vendor_identity;
};
struct acpi_madt_header { struct acpi_header header; uint32_t local_intc_address, flags; };
struct acpi_gicc {
    uint8_t type, length;
    uint16_t reserved;
    uint32_t interface_number, uid, flags, parking_protocol_version;
    uint32_t performance_interrupt_gsiv;
    uint64_t parked_address, physical_base, gicv, gich;
    uint32_t maintenance_interrupt;
    uint64_t gicr_base, mpidr;
    uint8_t efficiency_class, reserved2;
    uint16_t spe_overflow_interrupt;
};
struct acpi_gicd {
    uint8_t type, length;
    uint16_t reserved1;
    uint32_t gic_id;
    uint64_t base;
    uint32_t system_vector_base;
    uint8_t version, reserved2[3];
};
struct acpi_gicr {
    uint8_t type, length;
    uint16_t reserved;
    uint64_t base;
    uint32_t size;
};
struct acpi_gic_its {
    uint8_t type, length;
    uint16_t reserved;
    uint32_t id;
    uint64_t base;
    uint32_t reserved2;
};
struct acpi_gtdt {
    struct acpi_header header;
    uint64_t counter_control_base;
    uint32_t reserved;
    uint32_t secure_gsiv, secure_flags;
    uint32_t nonsecure_gsiv, nonsecure_flags;
    uint32_t virtual_gsiv, virtual_flags;
    uint32_t nonsecure_el2_gsiv, nonsecure_el2_flags;
    uint64_t counter_read_base;
    uint32_t platform_timer_count, platform_timer_offset;
    uint32_t virtual_el2_gsiv, virtual_el2_flags;
};
struct acpi_mcfg {
    struct acpi_header header;
    uint64_t reserved;
    uint64_t base;
    uint16_t segment;
    uint8_t start_bus, end_bus;
    uint32_t reserved2;
};
struct acpi_xsdt { struct acpi_header header; uint64_t entries[5]; };
struct acpi_rsdp {
    uint64_t signature;
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address, length;
    uint64_t xsdt_address;
    uint8_t extended_checksum, reserved[3];
};
#pragma pack(pop)

_Static_assert(sizeof(struct acpi_header) == 36, "ACPI header layout");
_Static_assert(sizeof(struct acpi_fadt) == 276, "FADT 6.4 layout");
_Static_assert(offsetof(struct acpi_fadt, flags) == 112, "FADT flags offset");
_Static_assert(offsetof(struct acpi_fadt, arm_boot_arch) == 129, "FADT ARM boot flags offset");
_Static_assert(offsetof(struct acpi_fadt, x_dsdt) == 140, "FADT X_DSDT offset");
_Static_assert(sizeof(struct acpi_gicc) == 80, "GICC 6.4 layout");
_Static_assert(sizeof(struct acpi_gicd) == 24, "GICD 6.4 layout");
_Static_assert(sizeof(struct acpi_gicr) == 16, "GICR 6.4 layout");
_Static_assert(sizeof(struct acpi_gic_its) == 20, "GIC ITS 6.4 layout");
_Static_assert(sizeof(struct acpi_gtdt) == 104, "GTDT 6.4 layout");
_Static_assert(sizeof(struct acpi_rsdp) == 36, "RSDP 2.0 layout");

static const struct efi_guid acpi2_guid = {
    0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}
};
static const struct efi_guid acpi1_guid = {
    0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}
};
static const struct efi_guid fdt_guid = {
    0xb1b621d5, 0xf19c, 0x41a5, {0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0}
};
static void *acpi_console;

#define EFI_SUCCESS 0ull
#define EFI_INVALID_PARAMETER 0x8000000000000002ull
#define EFI_OUT_OF_RESOURCES 0x8000000000000009ull
#define EFI_ALREADY_STARTED 0x8000000000000014ull
#define EFI_ALLOCATE_ANY_PAGES 0u
#define EFI_ACPI_RECLAIM_MEMORY 9u
#define EFI_RESERVED_MEMORY 0u
#define FDT_MAGIC 0xd00dfeedu
#define FDT_BEGIN_NODE 1u
#define FDT_END_NODE 2u
#define FDT_PROP 3u
#define FDT_NOP 4u
#define FDT_END 9u

struct fdt_node {
    char name[64], compatible[128], psci_method[16];
    const uint8_t *reg, *interrupts, *ranges, *bus_range;
    uint32_t compatible_len, reg_len, interrupts_len, ranges_len, bus_range_len;
    uint32_t addr_cells, size_cells, parent_addr_cells, parent_size_cells;
};
struct platform_info {
    uint64_t gicd, gicr, its, ecam, ecam_size, mmio_base, mmio_size, prefetch_base, prefetch_size, mpidr;
    uint32_t gicr_size, its_size, bus_start, bus_end;
    uint32_t timer_gsiv[4], timer_flags[4], maintenance_gsiv;
    uint32_t cpu_count;
    uint8_t psci_hvc;
    uint8_t found_gic, found_its, found_timer, found_pci, found_cpu, found_psci;
    uint8_t pci_is_ecam;
};
static struct fdt_node fdt_stack[32];

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void diagnostic(const char *text)
{
    efi_status (*write)(void *, const uint16_t *) = *(efi_status (**)(void *, const uint16_t *))((uint8_t *)acpi_console + 8);
    uint16_t wide[96];
    while (*text) {
        uint32_t n = 0;
        while (*text && n + 1 < sizeof(wide) / sizeof(wide[0])) wide[n++] = (uint8_t)*text++;
        wide[n] = 0;
        write(acpi_console, wide);
    }
}

static void diagnostic_hex(uint64_t value)
{
    char digits[17];
    int i;
    for (i = 15; i >= 0; --i) { digits[i] = "0123456789abcdef"[value & 15u]; value >>= 4; }
    digits[16] = 0;
    diagnostic(digits);
}

static uint32_t align4(uint32_t n) { return (n + 3u) & ~3u; }

static int has_text(const char *text, const char *needle)
{
    uint32_t i, j;
    for (; *text; ++text) {
        for (i = 0, j = 0; needle[j] && text[i] == needle[j]; ++i, ++j) {}
        if (!needle[j]) return 1;
    }
    return 0;
}

static int has_compatible(const struct fdt_node *node, const char *wanted)
{
    uint32_t start = 0;
    while (start < node->compatible_len) {
        uint32_t end = start, i = 0;
        while (end < node->compatible_len && node->compatible[end]) ++end;
        while (wanted[i] && start + i < end && node->compatible[start + i] == wanted[i]) ++i;
        if (!wanted[i] && start + i == end) return 1;
        start = end + 1;
    }
    return 0;
}

static int property_name(const uint8_t *strings, uint32_t length, uint32_t offset, const char *name)
{
    uint32_t i = 0;
    if (offset >= length) return 0;
    while (name[i] && offset + i < length && strings[offset + i] == (uint8_t)name[i]) ++i;
    return name[i] == 0 && offset + i < length && strings[offset + i] == 0;
}

static int read_cells(const uint8_t *data, uint32_t count, uint64_t *value)
{
    uint32_t i;
    uint64_t n = 0;
    if (count > 2) return 0;
    for (i = 0; i < count; ++i) n = (n << 32) | be32(data + i * 4u);
    *value = n;
    return 1;
}

static int reg_tuple(const struct fdt_node *node, uint32_t tuple, uint64_t *base, uint64_t *size)
{
    uint32_t cells = node->parent_addr_cells + node->parent_size_cells;
    uint64_t off = (uint64_t)tuple * cells * 4u;
    if (!node->reg || !node->parent_addr_cells || node->parent_addr_cells > 2 || node->parent_size_cells > 2 ||
        off + (uint64_t)cells * 4u > node->reg_len) return 0;
    if (!read_cells(node->reg + off, node->parent_addr_cells, base) ||
        !read_cells(node->reg + off + node->parent_addr_cells * 4u, node->parent_size_cells, size)) return 0;
    return 1;
}

static int decode_interrupt(const uint8_t *data, uint32_t cells, uint32_t *gsiv, uint32_t *flags)
{
    uint32_t type, irq, f, sense;
    if (cells < 3) return 0;
    type = be32(data);
    irq = be32(data + 4);
    f = be32(data + 8);
    if (type == 0 && irq < 988) *gsiv = irq + 32;
    else if (type == 1 && irq < 16) *gsiv = irq + 16;
    else return 0;
    /* DT GIC sense values: 1 rising edge, 2 falling edge, 4 high level,
     * 8 low level. GTDT bit 0 is edge/level; bit 1 is active-low. */
    sense = f & 0x0fu;
    switch (sense) {
    case 0: *flags = 0; break; /* Firmware default; treated as level/high. */
    case 1: *flags = 1; break;
    case 2: *flags = 3; break;
    case 4: *flags = 0; break;
    case 8: *flags = 2; break;
    default: return 0;
    }
    return 1;
}

static int decode_ranges(const struct fdt_node *node, struct platform_info *p)
{
    uint32_t child_cells = node->addr_cells, parent_cells = node->parent_addr_cells;
    uint32_t size_cells = node->size_cells, tuple_cells = child_cells + parent_cells + size_cells;
    uint32_t count, i;
    if (!node->ranges || !child_cells || child_cells != 3 || parent_cells < 1 || parent_cells > 2 || size_cells < 1 || size_cells > 2 ||
        node->ranges_len % (tuple_cells * 4u)) return 0;
    count = node->ranges_len / (tuple_cells * 4u);
    for (i = 0; i < count; ++i) {
        const uint8_t *q = node->ranges + i * tuple_cells * 4u;
        uint32_t flags = be32(q);
        uint64_t parent, size;
        if (!read_cells(q + child_cells * 4u, parent_cells, &parent) ||
            !read_cells(q + (child_cells + parent_cells) * 4u, size_cells, &size) || !size) return 0;
        if (flags == 0x02000000u) { p->mmio_base = parent; p->mmio_size = size; }
        else if (flags == 0x43000000u) { p->prefetch_base = parent; p->prefetch_size = size; }
        else return 0;
    }
    return p->mmio_size && p->prefetch_size;
}

static int finish_node(const struct fdt_node *node, struct platform_info *p)
{
    uint64_t base, size;
    if (has_compatible(node, "arm,gic-v3")) {
        if (!reg_tuple(node, 0, &base, &size) || size < 0x10000u) return 0;
        p->gicd = base;
        if (!reg_tuple(node, 1, &base, &size) || size > 0xffffffffu) return 0;
        p->gicr = base;
        p->gicr_size = (uint32_t)size;
        if (node->interrupts && node->interrupts_len >= 12)
            (void)decode_interrupt(node->interrupts, 3, &p->maintenance_gsiv, &p->timer_flags[0]);
        p->found_gic = 1;
    }
    if (has_compatible(node, "arm,gic-v3-its")) {
        if (!reg_tuple(node, 0, &p->its, &size) || !size || size > 0xffffffffu) return 0;
        p->its_size = (uint32_t)size;
        p->found_its = 1;
    }
    if (has_compatible(node, "arm,armv8-timer")) {
        uint32_t i, cells = 3;
        if (!node->interrupts || node->interrupts_len != 4u * cells * 4u) {
            diagnostic("ACPI timer interrupt length "); diagnostic_hex(node->interrupts_len); diagnostic("\r\n");
            return 0;
        }
        for (i = 0; i < 4; ++i) {
            if (!decode_interrupt(node->interrupts + i * cells * 4u, cells, &p->timer_gsiv[i], &p->timer_flags[i])) {
                diagnostic("ACPI timer interrupt invalid type/id ");
                diagnostic_hex(be32(node->interrupts + i * cells * 4u)); diagnostic("/");
                diagnostic_hex(be32(node->interrupts + i * cells * 4u + 4)); diagnostic("\r\n");
                return 0;
            }
        }
        p->found_timer = 1;
    }
    if (has_compatible(node, "pci-host-cam-generic") ||
        has_compatible(node, "pci-host-ecam-generic")) {
        if (!reg_tuple(node, 0, &p->ecam, &size) ||
            !node->bus_range || node->bus_range_len != 8 ||
            !decode_ranges(node, p)) return 0;
        p->bus_start = be32(node->bus_range);
        p->bus_end = be32(node->bus_range + 4);
        if (p->bus_start > p->bus_end || p->bus_start != 0 || p->bus_end != 0) return 0;
        p->ecam_size = size;
        p->pci_is_ecam = has_compatible(node, "pci-host-ecam-generic");
        if (p->pci_is_ecam) {
            if (size < ((uint64_t)p->bus_end - p->bus_start + 1u) * 0x100000u) return 0;
        } else if (size != 0x01000000u) {
            return 0;
        }
        p->found_pci = 1;
    }
    if (has_compatible(node, "arm,psci-1.0") || has_compatible(node, "arm,psci-0.2") || has_compatible(node, "arm,psci")) {
        if (!node->psci_method[0]) return 0;
        if (has_text(node->psci_method, "hvc")) p->psci_hvc = 1;
        else if (!has_text(node->psci_method, "smc")) return 0;
        p->found_psci = 1;
    }
    if (has_compatible(node, "arm,armv8")) {
        if (!reg_tuple(node, 0, &p->mpidr, &size)) return 0;
        ++p->cpu_count;
        p->found_cpu = 1;
    }
    return 1;
}

static void *find_fdt(void *system)
{
    uint64_t count = *(uint64_t *)((uint8_t *)system + 104);
    struct efi_config_table *tables = *(struct efi_config_table **)((uint8_t *)system + 112);
    uint64_t i, limit = count < 128 ? count : 128;
    const uint8_t *a = (const uint8_t *)&fdt_guid;
    for (i = 0; tables && i < limit; ++i) {
        uint32_t j;
        const uint8_t *b = (const uint8_t *)&tables[i].guid;
        for (j = 0; j < sizeof(fdt_guid); ++j) if (a[j] != b[j]) break;
        if (j == sizeof(fdt_guid)) return tables[i].table;
    }
    return 0;
}

static int parse_fdt(void *system, struct platform_info *p)
{
    const uint8_t *fdt = find_fdt(system), *strings;
    struct fdt_node *stack = fdt_stack;
    uint32_t total, struct_off, strings_off, struct_size, strings_size, pos, end, depth = 0;
    if (!fdt) return 0;
    total = be32(fdt + 4);
    struct_off = be32(fdt + 8);
    strings_off = be32(fdt + 12);
    strings_size = be32(fdt + 32);
    struct_size = be32(fdt + 36);
    if (be32(fdt) != FDT_MAGIC || total < 40 || total > 1024u * 1024u ||
        struct_off > total || struct_size > total - struct_off || strings_off > total || strings_size > total - strings_off) return 0;
    strings = fdt + strings_off;
    pos = struct_off;
    end = struct_off + struct_size;
    while (pos + 4 <= end) {
        uint32_t token = be32(fdt + pos);
        pos += 4;
        if (token == FDT_BEGIN_NODE) {
            uint32_t start = pos, n;
            struct fdt_node *node;
            while (pos < end && fdt[pos]) ++pos;
            if (pos >= end || depth >= sizeof(fdt_stack) / sizeof(fdt_stack[0])) return 0;
            n = pos - start;
            node = &stack[depth];
            for (uint32_t i = 0; i < sizeof(*node); ++i) ((uint8_t *)node)[i] = 0;
            if (n >= sizeof(node->name)) n = sizeof(node->name) - 1;
            for (uint32_t i = 0; i < n; ++i) node->name[i] = (char)fdt[start + i];
            node->parent_addr_cells = depth ? stack[depth - 1].addr_cells : 2;
            node->parent_size_cells = depth ? stack[depth - 1].size_cells : 2;
            node->addr_cells = node->parent_addr_cells;
            node->size_cells = node->parent_size_cells;
            ++pos;
            pos = align4(pos);
            if (pos > end) return 0;
            ++depth;
        } else if (token == FDT_END_NODE) {
            if (!depth) return 0;
            if (!finish_node(&stack[depth - 1], p)) {
                diagnostic("ACPI FDT node rejected: ");
                diagnostic(stack[depth - 1].name);
                diagnostic("\r\n");
                return 0;
            }
            --depth;
        } else if (token == FDT_PROP) {
            uint32_t length, name_off, value;
            struct fdt_node *node;
            if (!depth || pos + 8 > end) return 0;
            length = be32(fdt + pos);
            name_off = be32(fdt + pos + 4);
            pos += 8;
            value = pos;
            if (length > end - pos || name_off >= strings_size) return 0;
            pos += align4(length);
            if (pos > end) return 0;
            node = &stack[depth - 1];
            if (property_name(strings, strings_size, name_off, "#address-cells") && length == 4) node->addr_cells = be32(fdt + value);
            else if (property_name(strings, strings_size, name_off, "#size-cells") && length == 4) node->size_cells = be32(fdt + value);
            else if (property_name(strings, strings_size, name_off, "compatible")) {
                uint32_t n = length < sizeof(node->compatible) - 1 ? length : sizeof(node->compatible) - 1;
                for (uint32_t i = 0; i < n; ++i) node->compatible[i] = (char)fdt[value + i];
                node->compatible[n] = 0;
                node->compatible_len = n;
            } else if (property_name(strings, strings_size, name_off, "reg")) { node->reg = fdt + value; node->reg_len = length; }
            else if (property_name(strings, strings_size, name_off, "interrupts")) { node->interrupts = fdt + value; node->interrupts_len = length; }
            else if (property_name(strings, strings_size, name_off, "ranges")) { node->ranges = fdt + value; node->ranges_len = length; }
            else if (property_name(strings, strings_size, name_off, "bus-range")) { node->bus_range = fdt + value; node->bus_range_len = length; }
            else if (property_name(strings, strings_size, name_off, "method")) {
                uint32_t n = length < sizeof(node->psci_method) - 1 ? length : sizeof(node->psci_method) - 1;
                for (uint32_t i = 0; i < n && fdt[value + i]; ++i) node->psci_method[i] = (char)fdt[value + i];
            }
        } else if (token == FDT_NOP) {
            continue;
        } else if (token == FDT_END) {
            break;
        } else return 0;
    }
    if (depth || !p->found_gic || !p->found_its || !p->found_timer || !p->found_pci || !p->found_psci || !p->found_cpu || p->cpu_count != 1 || p->mpidr != 0) {
        diagnostic("ACPI FDT nodes found GIC/ITS/TIMER/PCI/PSCI/CPU ");
        diagnostic(p->found_gic ? "1" : "0"); diagnostic(p->found_its ? "1" : "0");
        diagnostic(p->found_timer ? "1" : "0"); diagnostic(p->found_pci ? "1" : "0");
        diagnostic(p->found_psci ? "1" : "0"); diagnostic(p->found_cpu ? "1" : "0");
        diagnostic(" CPU-count "); diagnostic_hex(p->cpu_count); diagnostic(" MPIDR "); diagnostic_hex(p->mpidr); diagnostic("\r\n");
        return 0;
    }
    return 1;
}

static void zero_bytes(void *dst, uint64_t size)
{
    uint8_t *p = dst;
    while (size--) *p++ = 0;
}

static void copy_bytes(void *dst, const void *src, uint64_t size)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (size--) *d++ = *s++;
}

static void set_header(struct acpi_header *header, uint32_t signature, uint32_t length, uint8_t revision, const char *table_id)
{
    uint32_t i;
    header->signature = signature;
    header->length = length;
    header->revision = revision;
    header->checksum = 0;
    copy_bytes(header->oem_id, "HYPERW", 6);
    for (i = 0; i < 8; ++i) header->oem_table_id[i] = table_id[i] ? table_id[i] : ' ';
    header->oem_revision = 1;
    header->creator_id = 0x4e495748u;
    header->creator_revision = 1;
}

static void checksum(void *table, uint32_t length, uint32_t checksum_offset)
{
    uint8_t *bytes = table;
    uint32_t i, sum = 0;
    bytes[checksum_offset] = 0;
    for (i = 0; i < length; ++i) sum += bytes[i];
    bytes[checksum_offset] = (uint8_t)(0u - sum);
}

static void *allocate_typed(void *bs, uint64_t bytes, uint32_t type)
{
    efi_allocate_pages allocate = *(efi_allocate_pages *)((uint8_t *)bs + 24 + 2 * 8);
    uint64_t pages = (bytes + 4095u) >> 12, address = 0;
    if (allocate(EFI_ALLOCATE_ANY_PAGES, type, pages, &address) != EFI_SUCCESS) return 0;
    zero_bytes((void *)address, pages << 12);
    return (void *)address;
}

static void *allocate_acpi(void *bs, uint64_t bytes)
{
    return allocate_typed(bs, bytes, EFI_ACPI_RECLAIM_MEMORY);
}

/* crosvm's aarch64 PCI window is CAM (bus<<16 | devfn<<8 | reg). Windows only speaks
 * ECAM (bus<<20 | dev<<15 | fn<<12 | reg). Build an ECAM-shaped copy of bus 0 in reserved
 * RAM and point MCFG at it. It is a read-only snapshot: Windows' config writes land in
 * RAM and never reach the device, so BARs must stay where firmware left them. */
#define SHADOW_BYTES 0x100000u
static void flush_data(void *base, uint64_t bytes)
{
    uint8_t *p = base;
    uint64_t i;
    for (i = 0; i < bytes; i += 64) __asm__ volatile("dc civac, %0" :: "r"(p + i) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t build_shadow_ecam(void *bs, uint64_t cam)
{
    uint8_t *shadow = allocate_typed(bs, SHADOW_BYTES, EFI_RESERVED_MEMORY);
    uint32_t dev, fn, i, found = 0;
    if (!shadow) return 0;
    for (i = 0; i < SHADOW_BYTES; ++i) shadow[i] = 0xff;
    for (dev = 0; dev < 32; ++dev) {
        uint32_t functions = 1;
        for (fn = 0; fn < functions; ++fn) {
            volatile uint32_t *src = (volatile uint32_t *)(cam + ((uint64_t)dev << 11) + ((uint64_t)fn << 8));
            uint32_t *dst = (uint32_t *)(shadow + ((uint32_t)dev << 15) + ((uint32_t)fn << 12));
            if ((src[0] & 0xffffu) == 0xffffu) continue;
            for (i = 0; i < 64; ++i) dst[i] = src[i];
            for (i = 64; i < 1024; ++i) dst[i] = 0;
            if (!fn && (((src[3] >> 16) & 0x80u) != 0)) functions = 8;
            ++found;
            diagnostic("ACPI shadow dev "); diagnostic_hex(dev); diagnostic(" fn "); diagnostic_hex(fn);
            diagnostic(" id "); diagnostic_hex(dst[0]); diagnostic("\r\n");
        }
    }
    flush_data(shadow, SHADOW_BYTES);
    return found ? (uint64_t)shadow : 0;
}

static int already_has_acpi(void *system)
{
    uint64_t count = *(uint64_t *)((uint8_t *)system + 104), i, limit = count < 128 ? count : 128;
    struct efi_config_table *tables = *(struct efi_config_table **)((uint8_t *)system + 112);
    const uint8_t *a = (const uint8_t *)&acpi2_guid, *b = (const uint8_t *)&acpi1_guid;
    for (i = 0; tables && i < limit; ++i) {
        uint32_t j;
        const uint8_t *g = (const uint8_t *)&tables[i].guid;
        for (j = 0; j < sizeof(acpi2_guid) && a[j] == g[j]; ++j) {}
        if (j == sizeof(acpi2_guid)) return 1;
        for (j = 0; j < sizeof(acpi1_guid) && b[j] == g[j]; ++j) {}
        if (j == sizeof(acpi1_guid)) return 1;
    }
    return 0;
}

efi_status HyperwinInstallAcpi(void *system)
{
    struct platform_info p = {0};
    void *bs, *aml_dsdt, *aml_ssdt, *dsdt, *ssdt, *fadt, *madt, *gtdt, *mcfg, *xsdt, *rsdp;
    struct acpi_fadt *f;
    struct acpi_madt_header *m;
    struct acpi_gicc *gicc;
    struct acpi_gicd *gicd;
    struct acpi_gicr *gicr;
    struct acpi_gic_its *its;
    struct acpi_gtdt *t;
    struct acpi_mcfg *c;
    struct acpi_xsdt *x;
    struct acpi_rsdp *r;
    uint8_t *cursor;
    uint64_t mcfg_base;
    uint32_t madt_bytes = sizeof(struct acpi_madt_header) + sizeof(struct acpi_gicc) + sizeof(struct acpi_gicd) + sizeof(struct acpi_gicr) + sizeof(struct acpi_gic_its);
    efi_install_configuration_table install;
    efi_status status;

    if (!system) return EFI_INVALID_PARAMETER;
    acpi_console = *(void **)((uint8_t *)system + 0x40);
    if (already_has_acpi(system)) return EFI_ALREADY_STARTED;
    if (!parse_fdt(system, &p)) { diagnostic("ACPI FDT parse rejected\r\n"); return EFI_INVALID_PARAMETER; }
    diagnostic("ACPI FDT input GICD "); diagnostic_hex(p.gicd);
    diagnostic(" GICR "); diagnostic_hex(p.gicr);
    diagnostic(" ITS "); diagnostic_hex(p.its);
    diagnostic(" ECAM "); diagnostic_hex(p.ecam);
    diagnostic(" MMIO "); diagnostic_hex(p.mmio_base);
    diagnostic("+"); diagnostic_hex(p.mmio_size);
    diagnostic(" PREF "); diagnostic_hex(p.prefetch_base);
    diagnostic("+"); diagnostic_hex(p.prefetch_size);
    diagnostic(" PCI-config "); diagnostic(p.pci_is_ecam ? "ECAM" : "CAM");
    diagnostic(" TIMER "); diagnostic_hex(p.timer_gsiv[0]); diagnostic("/");
    diagnostic_hex(p.timer_gsiv[1]); diagnostic("/"); diagnostic_hex(p.timer_gsiv[2]);
    diagnostic("/"); diagnostic_hex(p.timer_gsiv[3]); diagnostic("\r\n");
    /* Keep the static PCI AML tied to this captured FDT. Refuse mismatches. */
    if (p.gicd != 0x3fff0000ull || p.gicr != 0x3ffd0000ull || p.gicr_size != 0x20000u ||
        p.its != 0x40000000ull || p.its_size != 0x20000u ||
        p.ecam != 0x2e000000ull || p.bus_start != 0 || p.bus_end != 0 ||
        p.mmio_base != 0x2c000000ull || p.mmio_size != 0x02000000ull ||
        p.prefetch_base != 0x188000000ull || p.prefetch_size != 0xfe78000000ull ||
        p.timer_gsiv[0] != 29 || p.timer_gsiv[1] != 30 || p.timer_gsiv[2] != 27 || p.timer_gsiv[3] != 26) {
        diagnostic("ACPI FDT layout differs from SSDT\r\n");
        return EFI_INVALID_PARAMETER;
    }

    bs = *(void **)((uint8_t *)system + 0x60);
    mcfg_base = p.ecam;
    if (!p.pci_is_ecam) {
        mcfg_base = build_shadow_ecam(bs, p.ecam);
        if (!mcfg_base) { diagnostic("ACPI shadow ECAM build failed\r\n"); return EFI_OUT_OF_RESOURCES; }
        diagnostic("ACPI shadow ECAM at "); diagnostic_hex(mcfg_base); diagnostic("\r\n");
    }
    install = *(efi_install_configuration_table *)((uint8_t *)bs + 24 + 21 * 8);
    aml_dsdt = (void *)HyperwinDsdt;
    aml_ssdt = (void *)HyperwinPciSsdt;
    if (HYPERWINDSDT_SIZE < sizeof(struct acpi_header) || HYPERWINPCISSDT_SIZE < sizeof(struct acpi_header)) return EFI_INVALID_PARAMETER;
    dsdt = allocate_acpi(bs, HYPERWINDSDT_SIZE);
    ssdt = allocate_acpi(bs, HYPERWINPCISSDT_SIZE);
    fadt = allocate_acpi(bs, sizeof(struct acpi_fadt));
    madt = allocate_acpi(bs, madt_bytes);
    gtdt = allocate_acpi(bs, sizeof(struct acpi_gtdt));
    mcfg = allocate_acpi(bs, sizeof(struct acpi_mcfg));
    xsdt = allocate_acpi(bs, sizeof(struct acpi_xsdt));
    rsdp = allocate_acpi(bs, sizeof(struct acpi_rsdp));
    if (!dsdt || !ssdt || !fadt || !madt || !gtdt || !mcfg || !xsdt || !rsdp) {
        diagnostic("ACPI reclaim allocation failed\r\n");
        return EFI_OUT_OF_RESOURCES;
    }
    copy_bytes(dsdt, aml_dsdt, HYPERWINDSDT_SIZE);
    copy_bytes(ssdt, aml_ssdt, HYPERWINPCISSDT_SIZE);

    f = fadt;
    set_header(&f->header, 0x50434146u, sizeof(*f), 6, "HWFADT  ");
    f->preferred_pm_profile = 2;
    f->flags = 1u << 20; /* Hardware-reduced ACPI. */
    f->arm_boot_arch = 1u | (p.psci_hvc ? 2u : 0u);
    f->minor_version = 4;
    f->x_dsdt = (uint64_t)dsdt;
    checksum(f, sizeof(*f), offsetof(struct acpi_header, checksum));

    m = madt;
    set_header(&m->header, 0x43495041u, madt_bytes, 5, "HWMADT  ");
    m->flags = 0;
    cursor = (uint8_t *)m + sizeof(*m);
    gicc = (struct acpi_gicc *)cursor;
    gicc->type = 0x0b; gicc->length = sizeof(*gicc); gicc->interface_number = 0; gicc->uid = 0;
    gicc->flags = 1; gicc->maintenance_interrupt = p.maintenance_gsiv;
    gicc->gicr_base = p.gicr; gicc->mpidr = p.mpidr;
    cursor += sizeof(*gicc);
    gicd = (struct acpi_gicd *)cursor;
    gicd->type = 0x0c; gicd->length = sizeof(*gicd); gicd->base = p.gicd; gicd->version = 3;
    cursor += sizeof(*gicd);
    gicr = (struct acpi_gicr *)cursor;
    gicr->type = 0x0e; gicr->length = sizeof(*gicr); gicr->base = p.gicr; gicr->size = p.gicr_size;
    cursor += sizeof(*gicr);
    its = (struct acpi_gic_its *)cursor;
    its->type = 0x0f; its->length = sizeof(*its); its->id = 0; its->base = p.its;
    checksum(m, madt_bytes, offsetof(struct acpi_header, checksum));

    t = gtdt;
    set_header(&t->header, 0x54445447u, sizeof(*t), 3, "HWGTDT  ");
    /* No memory-mapped counter frames: the spec value for "not present" is all ones, not 0. */
    t->counter_control_base = ~0ull;
    t->counter_read_base = ~0ull;
    t->secure_gsiv = p.timer_gsiv[0]; t->secure_flags = p.timer_flags[0];
    t->nonsecure_gsiv = p.timer_gsiv[1]; t->nonsecure_flags = p.timer_flags[1];
    t->virtual_gsiv = p.timer_gsiv[2]; t->virtual_flags = p.timer_flags[2];
    t->nonsecure_el2_gsiv = p.timer_gsiv[3]; t->nonsecure_el2_flags = p.timer_flags[3];
    checksum(t, sizeof(*t), offsetof(struct acpi_header, checksum));

    c = mcfg;
    set_header(&c->header, 0x4746434du, sizeof(*c), 1, "HWMCFG  ");
    c->base = mcfg_base; c->segment = 0; c->start_bus = (uint8_t)p.bus_start; c->end_bus = (uint8_t)p.bus_end;
    checksum(c, sizeof(*c), offsetof(struct acpi_header, checksum));

    x = xsdt;
    set_header(&x->header, 0x54445358u, sizeof(*x), 1, "HWXSDT  ");
    x->entries[0] = (uint64_t)fadt; x->entries[1] = (uint64_t)madt;
    x->entries[2] = (uint64_t)gtdt; x->entries[3] = (uint64_t)mcfg; x->entries[4] = (uint64_t)ssdt;
    checksum(x, sizeof(*x), offsetof(struct acpi_header, checksum));

    r = rsdp;
    r->signature = 0x2052545020445352ull; /* "RSD PTR " */
    copy_bytes(r->oem_id, "HYPERW", 6);
    r->revision = 2; r->length = sizeof(*r); r->xsdt_address = (uint64_t)xsdt;
    checksum(r, 20, offsetof(struct acpi_rsdp, checksum));
    checksum(r, sizeof(*r), offsetof(struct acpi_rsdp, extended_checksum));

    status = install(&acpi2_guid, rsdp);
    if (status) { diagnostic("ACPI 2.0 config install failed\r\n"); return status; }
    status = install(&acpi1_guid, rsdp);
    diagnostic(status ? "ACPI 1.0 config install failed\r\n" : "ACPI config tables published\r\n");
    return status;
}
