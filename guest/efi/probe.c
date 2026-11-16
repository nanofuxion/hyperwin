#include "hw_abi.h"
#include "ring.h"

struct efi_desc {
    uint32_t type;
    uint32_t pad;
    uint64_t physical;
    uint64_t virtual_;
    uint64_t pages;
    uint64_t attribute;
};

typedef uint64_t (*efi_get_memory_map)(uint64_t *size, void *map, uint64_t *key, uint64_t *desc_size, uint32_t *ver);
typedef uint64_t (*efi_allocate_pages)(uint32_t type, uint32_t memory_type, uint64_t pages, uint64_t *address);
typedef uint64_t (*efi_allocate_pool)(uint32_t type, uint64_t size, void **out);
typedef uint64_t (*efi_output_string)(void *protocol, const uint16_t *string);
static void *efi_console_out;

static void uart_str(const char *s)
{
    efi_output_string output = *(efi_output_string *)((uint8_t *)efi_console_out + 8);
    uint16_t wide[128];
    while (*s) {
        uint32_t n = 0;
        while (*s && n + 1 < sizeof(wide) / sizeof(wide[0]))
            wide[n++] = (uint8_t)*s++;
        wide[n] = 0;
        output(efi_console_out, wide);
    }
}

static void uart_hex(uint64_t v)
{
    char buf[17];
    int i;
    for (i = 15; i >= 0; i--) {
        buf[i] = "0123456789abcdef"[v & 0xf];
        v >>= 4;
    }
    buf[16] = 0;
    uart_str(buf);
}

static void be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static struct hw_header *find_window(void *bs)
{
    efi_get_memory_map getmap = *(efi_get_memory_map *)((uint8_t *)bs + 24 + 4 * 8);
    efi_allocate_pool alloc = *(efi_allocate_pool *)((uint8_t *)bs + 24 + 5 * 8);
    uint64_t bytes = 0, key = 0, desc_size = 0;
    uint32_t ver = 0;
    uint8_t *map = 0;
    uint64_t off;
    struct hw_header *fixed = (struct hw_header *)HW_WINDOW_PA;
    if (fixed->magic[0] == 'H' && fixed->magic[1] == 'Y' && fixed->magic[2] == 'P' &&
        fixed->magic[3] == 'R' && fixed->magic[4] == 'W' && fixed->magic[5] == 'I' &&
        fixed->magic[6] == 'N' && fixed->magic[7] == '1') return fixed;
    getmap(&bytes, 0, &key, &desc_size, &ver);
    bytes += 4096;
    if (alloc(4, bytes, (void **)&map)) return 0;
    if (getmap(&bytes, map, &key, &desc_size, &ver)) return 0;
    if (desc_size < sizeof(struct efi_desc)) return 0;
    for (off = 0; off + sizeof(struct efi_desc) <= bytes; off += desc_size) {
        struct efi_desc *d = (struct efi_desc *)(map + off);
        uint64_t span;
        uint8_t *p;
        if ((off / desc_size) < 24) {
            uart_str("mem ");
            uart_hex(d->type);
            uart_str(" ");
            uart_hex(d->physical);
            uart_str(" ");
            uart_hex(d->pages);
            uart_str("\r\n");
        }
        if (d->type != 7 && d->type != 14 && d->type != 9 && d->type != 10 && d->type != 0) continue;
        span = d->pages << 12;
        if (span < 8) continue;
        p = (uint8_t *)d->physical;
        if (p[0] == 'H' && p[1] == 'Y' && p[2] == 'P' && p[3] == 'R' &&
            p[4] == 'W' && p[5] == 'I' && p[6] == 'N' && p[7] == '1')
            return (struct hw_header *)p;
    }
    return 0;
}

static uint64_t reserve_window_pages(void *bs)
{
    efi_allocate_pages allocate = *(efi_allocate_pages *)((uint8_t *)bs + 24 + 2 * 8);
    uint64_t address = HW_WINDOW_PA;
    /* AllocateAddress=2, EfiReservedMemoryType=0; keep Windows from reusing the ring. */
    uint64_t status = allocate(2, 0, HW_WINDOW_SIZE / 4096, &address);
    uart_str("AllocatePages status ");
    uart_hex(status);
    uart_str(" address ");
    uart_hex(address);
    uart_str("\r\n");
    return status == 0 && address == HW_WINDOW_PA;
}

static void report_window_memory_map(void *bs)
{
    efi_get_memory_map getmap = *(efi_get_memory_map *)((uint8_t *)bs + 24 + 4 * 8);
    efi_allocate_pool alloc = *(efi_allocate_pool *)((uint8_t *)bs + 24 + 5 * 8);
    uint64_t bytes = 0, key = 0, desc_size = 0, off;
    uint32_t ver = 0;
    uint8_t *map = 0;
    uint64_t window_end = HW_WINDOW_PA + HW_WINDOW_SIZE;
    int found = 0;
    getmap(&bytes, 0, &key, &desc_size, &ver);
    bytes += 4096;
    if (alloc(4, bytes, (void **)&map)) return;
    if (getmap(&bytes, map, &key, &desc_size, &ver) || desc_size < sizeof(struct efi_desc)) return;
    for (off = 0; off + sizeof(struct efi_desc) <= bytes; off += desc_size) {
        struct efi_desc *d = (struct efi_desc *)(map + off);
        uint64_t end = d->physical + (d->pages << 12);
        if (end <= HW_WINDOW_PA || d->physical >= window_end) continue;
        found = 1;
        uart_str("window map type ");
        uart_hex(d->type);
        uart_str(" base ");
        uart_hex(d->physical);
        uart_str(" pages ");
        uart_hex(d->pages);
        uart_str("\r\n");
    }
    if (!found) uart_str("window absent from EFI memory map\r\n");
}

static int tpm_call(struct hw_header *h, const uint8_t *cmd, uint32_t len)
{
    uint32_t seq, i, spin;
    for (i = 0; i < len; i++) h->tpm_req[i] = cmd[i];
    h->tpm_req_len = len;
    seq = __atomic_add_fetch(&h->tpm_req_seq, 1, __ATOMIC_RELEASE);
    for (i = 0; i < 5000; i++) {
        if (__atomic_load_n(&h->tpm_resp_seq, __ATOMIC_ACQUIRE) == seq) {
            h->tpm_rc = rd32(h->tpm_resp + 6);
            return h->tpm_rc == 0;
        }
        for (spin = 0; spin < 100000; spin++) {}
    }
    return 0;
}

static void note_gpu(struct hw_header *h, void *bs)
{
    efi_get_memory_map getmap = *(efi_get_memory_map *)((uint8_t *)bs + 24 + 4 * 8);
    efi_allocate_pool alloc = *(efi_allocate_pool *)((uint8_t *)bs + 24 + 5 * 8);
    uint64_t bytes = 0, key = 0, desc_size = 0, off;
    uint32_t ver = 0, dev;
    uint8_t *map = 0;
    const uint64_t ecam = 0x3f000000ull;
    getmap(&bytes, 0, &key, &desc_size, &ver);
    bytes += 4096;
    if (alloc(4, bytes, (void **)&map)) return;
    if (getmap(&bytes, map, &key, &desc_size, &ver)) return;
    for (off = 0; off + 40 <= bytes; off += desc_size) {
        struct efi_desc *d = (struct efi_desc *)(map + off);
        uint64_t start = d->physical;
        uint64_t end = start + (d->pages << 12);
        if (d->type != 11 || ecam < start || ecam >= end) continue;
        for (dev = 0; dev < 32; dev++) {
            volatile uint16_t *cfg = (volatile uint16_t *)(ecam + ((uint64_t)dev << 15));
            uint16_t vendor = cfg[0];
            uint16_t device = cfg[1];
            if (vendor == 0x1af4 && (device == 0x1050 || device == 0x1040 + 16)) {
                h->pci_vendor = vendor;
                h->pci_device = device;
                __atomic_or_fetch(&h->guest_flags, HW_GUEST_GPU, __ATOMIC_RELEASE);
                uart_str("virtio-gpu ");
                uart_hex(device);
                uart_str("\r\n");
                return;
            }
        }
    }
}

uint64_t EfiMain(void *image, void *system)
{
    void *bs = *(void **)((uint8_t *)system + 0x60);
    struct hw_header *h;
    uint8_t *frame;
    uint32_t x, y;
    uint8_t cmd[16];
    int16_t tone[96];
    (void)image;
    efi_console_out = *(void **)((uint8_t *)system + 0x40);
    uart_str("\r\nHYPERWIN PROBE\r\n");
    h = find_window(bs);
    if (!h) {
        uart_str("window missing\r\n");
        return 0;
    }
    uart_str("window ");
    uart_hex((uint64_t)h);
    uart_str("\r\n");
    __atomic_or_fetch(&h->guest_flags, HW_GUEST_FOUND, __ATOMIC_RELEASE);
    if (reserve_window_pages(bs)) {
        __atomic_or_fetch(&h->guest_flags, HW_GUEST_RING_RESERVED, __ATOMIC_RELEASE);
        uart_str("ring pages reserved\r\n");
    }
    else {
        uart_str("ring page reservation failed\r\n");
        report_window_memory_map(bs);
    }
    frame = (uint8_t *)h + HW_FRAME0;
    for (y = 0; y < h->height; y++) {
        for (x = 0; x < h->width; x++) {
            uint8_t *p = frame + ((size_t)y * h->stride) + x * 4;
            p[0] = (uint8_t)x;
            p[1] = (uint8_t)y;
            p[2] = 0x40;
            p[3] = 0xff;
        }
    }
    hw_publish_frame(h, frame, h->width, h->height, h->stride);
    __atomic_or_fetch(&h->guest_flags, HW_GUEST_FRAME, __ATOMIC_RELEASE);
    for (x = 0; x < 48; x++) {
        int16_t s = (x & 8) ? 8000 : -8000;
        tone[x * 2] = s;
        tone[x * 2 + 1] = s;
    }
    hw_publish_audio(h, tone, 48);
    __atomic_or_fetch(&h->guest_flags, HW_GUEST_AUDIO, __ATOMIC_RELEASE);
    if (__atomic_load_n(&h->pad_connected, __ATOMIC_ACQUIRE))
        __atomic_or_fetch(&h->guest_flags, HW_GUEST_PAD, __ATOMIC_RELEASE);
    if (__atomic_load_n(&h->tpm_status, __ATOMIC_ACQUIRE)) {
        be16(cmd, 0x8001); be32(cmd + 2, 12); be32(cmd + 6, 0x00000144); be16(cmd + 10, 0);
        if (tpm_call(h, cmd, 12)) {
            be16(cmd, 0x8001); be32(cmd + 2, 12); be32(cmd + 6, 0x0000017B); be16(cmd + 10, 16);
            if (tpm_call(h, cmd, 12))
                __atomic_or_fetch(&h->guest_flags, HW_GUEST_TPM, __ATOMIC_RELEASE);
        }
    }
    note_gpu(h, bs);
    uart_str("flags ");
    uart_hex(h->guest_flags);
    uart_str("\r\nPROBE DONE\r\n");
    return 0;
}
