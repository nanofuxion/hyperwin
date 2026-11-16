#include "hw_abi.h"
#include "ring.h"

#define UART_DR ((volatile uint32_t *)0x09000000u)
#define UART_FR ((volatile uint32_t *)0x09000018u)

static void uart_str(const char *s)
{
    while (*s) {
        while (*UART_FR & (1u << 5)) {}
        *UART_DR = (uint8_t)*s++;
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

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

/* crosvm and QEMU both describe the shared window as a pmem-region in the device tree. */
static struct hw_header *find_window(void *fdt)
{
    const uint8_t *b = fdt;
    const uint8_t *p, *end, *strings;
    uint32_t totalsize, off_struct, off_strings;
    int depth = 0, pmem_depth = -1;
    uint64_t base = 0, node_base = 0;
    int node_reg = 0;
    if (!fdt || be32(b) != 0xd00dfeedu) return 0;
    totalsize = be32(b + 4);
    off_struct = be32(b + 8);
    off_strings = be32(b + 12);
    if (totalsize < 48 || off_struct > totalsize || off_strings > totalsize) return 0;
    p = b + off_struct;
    end = b + totalsize;
    strings = b + off_strings;
    while (p + 4 <= end) {
        uint32_t tok = be32(p);
        p += 4;
        if (tok == 1) {
            depth++;
            node_reg = 0;
            while (p < end && *p) p++;
            if (p < end) p++;
            p = (const uint8_t *)(((uintptr_t)p + 3u) & ~(uintptr_t)3u);
        } else if (tok == 2) {
            if (depth == pmem_depth && node_reg) return (struct hw_header *)node_base;
            if (depth == pmem_depth) pmem_depth = -1;
            depth--;
        } else if (tok == 3 && p + 8 <= end) {
            uint32_t len = be32(p);
            uint32_t nameoff = be32(p + 4);
            const char *name = (const char *)strings + nameoff;
            const uint8_t *val = p + 8;
            p = val + ((len + 3u) & ~3u);
            if (p > end) break;
            if (eq(name, "compatible")) {
                const char *s = (const char *)val;
                const char *e = s + len;
                while (s < e && *s) {
                    if (eq(s, "pmem-region")) pmem_depth = depth;
                    while (s < e && *s) s++;
                    if (s < e) s++;
                }
            } else if (eq(name, "reg") && len >= 16) {
                node_base = ((uint64_t)be32(val) << 32) | be32(val + 4);
                node_reg = 1;
                (void)base;
            }
        } else if (tok == 4) {
            continue;
        } else {
            break;
        }
    }
    return 0;
}

static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int tpm_call(struct hw_header *h, const uint8_t *cmd, uint32_t len)
{
    uint32_t seq, i;
    for (i = 0; i < len; i++) h->tpm_req[i] = cmd[i];
    h->tpm_req_len = len;
    seq = __atomic_add_fetch(&h->tpm_req_seq, 1, __ATOMIC_RELEASE);
    for (i = 0; i < 5000000u; i++) {
        if (__atomic_load_n(&h->tpm_resp_seq, __ATOMIC_ACQUIRE) == seq) {
            h->tpm_rc = get_be32(h->tpm_resp + 6);
            return h->tpm_rc == 0;
        }
    }
    return 0;
}

void hyperwin_main(void *fdt)
{
    struct hw_header *h;
    uint8_t *frame;
    uint32_t x, y;
    uint8_t cmd[16];
    int16_t tone[96];
    uart_str("\r\nHYPERWIN PROBE\r\n");
    /* File offset 16 MiB of the QEMU RAM backend. AVF uses the pmem node instead. */
    {
        struct hw_header *fixed = (struct hw_header *)0x41000000ull;
        if (fixed->magic[0] == 'H' && fixed->magic[1] == 'Y') h = fixed;
        else h = find_window(fdt);
    }
    if (!h || h->magic[0] != 'H') {
        uart_str("window missing\r\n");
        return;
    }
    uart_str("window ");
    uart_hex((uint64_t)h);
    uart_str("\r\n");
    __atomic_or_fetch(&h->guest_flags, HW_GUEST_FOUND, __ATOMIC_RELEASE);
    frame = (uint8_t *)h + HW_FRAME0;
    for (y = 0; y < h->height; y++) {
        for (x = 0; x < h->width; x++) {
            uint8_t *px = frame + ((size_t)y * h->stride) + x * 4;
            px[0] = (uint8_t)x;
            px[1] = (uint8_t)y;
            px[2] = 0x40;
            px[3] = 0xff;
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
        put_be16(cmd, 0x8001); put_be32(cmd + 2, 12); put_be32(cmd + 6, 0x00000144); put_be16(cmd + 10, 0);
        if (tpm_call(h, cmd, 12)) {
            put_be16(cmd, 0x8001); put_be32(cmd + 2, 12); put_be32(cmd + 6, 0x0000017B); put_be16(cmd + 10, 16);
            if (tpm_call(h, cmd, 12))
                __atomic_or_fetch(&h->guest_flags, HW_GUEST_TPM, __ATOMIC_RELEASE);
        }
    }
    uart_str("flags ");
    uart_hex(__atomic_load_n(&h->guest_flags, __ATOMIC_ACQUIRE));
    uart_str("\r\nPROBE DONE\r\n");
}
