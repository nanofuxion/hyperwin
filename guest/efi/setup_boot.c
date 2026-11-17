/* First boot file on the setup disk. AVF has no QEMU bootindex, so the
 * installer would start on its own and never see the shared display. */
#include "hw_abi.h"

typedef uint64_t (*efi_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static efi_fn bs_fn(void *bs, int index)
{
    return *(efi_fn *)((uint8_t *)bs + 24 + index * 8);
}

static efi_fn proto_fn(void *protocol, int index)
{
    return *(efi_fn *)((uint8_t *)protocol + 8 + index * 8);
}

static void paint(uint32_t bgra)
{
    struct hw_header *h = (struct hw_header *)HW_WINDOW_PA;
    uint32_t *px = (uint32_t *)(HW_WINDOW_PA + hw_frame_off(h, 0)), i;
    if (hw_mode_ok(h)) for (i = 0; i < h->width * h->height; i++) px[i] = bgra;
    __atomic_or_fetch(&h->guest_flags, HW_GUEST_RING_RESERVED | HW_GUEST_GOP, __ATOMIC_RELEASE);
}

static uint64_t read_file(void *bs, void *volume, const uint16_t *path, void **data, uint64_t *size)
{
    static const uint8_t info_guid[16] = {
        0x92, 0x6e, 0x57, 0x09, 0x3f, 0x6d, 0xd2, 0x11,
        0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b
    };
    uint8_t info[256];
    void *file = 0;
    uint64_t status, info_size = sizeof info, bytes;
    status = proto_fn(volume, 0)((uint64_t)volume, (uint64_t)&file, (uint64_t)path, 1, 0, 0);
    if (status || !file) return status ? status : 0x8000000000000002ull;
    status = proto_fn(file, 7)((uint64_t)file, (uint64_t)info_guid, (uint64_t)&info_size, (uint64_t)info, 0, 0);
    if (status) {
        proto_fn(file, 1)((uint64_t)file, 0, 0, 0, 0, 0);
        return status;
    }
    bytes = *(uint64_t *)(info + 8);
    status = bs_fn(bs, 5)(2, bytes, (uint64_t)data, 0, 0, 0);
    if (status) {
        proto_fn(file, 1)((uint64_t)file, 0, 0, 0, 0, 0);
        return status;
    }
    *size = bytes;
    status = proto_fn(file, 3)((uint64_t)file, (uint64_t)size, (uint64_t)*data, 0, 0, 0);
    proto_fn(file, 1)((uint64_t)file, 0, 0, 0, 0, 0);
    return status;
}

static uint64_t start_buffer(void *bs, void *parent, void *data, uint64_t size)
{
    void *loaded = 0;
    uint64_t status = bs_fn(bs, 22)(0, (uint64_t)parent, 0, (uint64_t)data, size, (uint64_t)&loaded);
    if (status || !loaded) return status ? status : 0x8000000000000002ull;
    return bs_fn(bs, 23)((uint64_t)loaded, 0, 0, 0, 0, 0);
}

static uint64_t start_path(void *bs, void *parent, void *volume, const uint16_t *path)
{
    void *data = 0;
    uint64_t size = 0, status;
    status = read_file(bs, volume, path, &data, &size);
    if (status) return status;
    return start_buffer(bs, parent, data, size);
}

/* Start a file by device path, the way a boot manager does. The Windows loader finds its
 * boot device through LoadedImage->DeviceHandle, which a buffer load leaves empty. */
static uint64_t start_on_device(void *bs, void *parent, void *handle, const uint16_t *path)
{
    static const uint8_t dp_guid[16] = {
        0x91, 0x6e, 0x57, 0x09, 0x3f, 0x6d, 0xd2, 0x11,
        0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b
    };
    uint8_t *dp = 0, *full = 0, *p;
    void *loaded = 0;
    uint64_t status, prefix = 0, chars = 0, node, i;
    status = bs_fn(bs, 16)((uint64_t)handle, (uint64_t)dp_guid, (uint64_t)&dp, 0, 0, 0);
    if (status || !dp) return status ? status : 0x8000000000000002ull;
    while (!(dp[prefix] == 0x7f && dp[prefix + 1] == 0xff)) {
        uint64_t len = dp[prefix + 2] | ((uint64_t)dp[prefix + 3] << 8);
        if (len < 4 || prefix > 4096) return 0x8000000000000002ull;
        prefix += len;
    }
    while (path[chars]) chars++;
    chars++;                                   /* keep the terminator */
    node = 4 + chars * 2;
    status = bs_fn(bs, 5)(4, prefix + node + 4, (uint64_t)&full, 0, 0, 0);
    if (status || !full) return status ? status : 0x8000000000000009ull;
    for (i = 0; i < prefix; i++) full[i] = dp[i];
    p = full + prefix;
    p[0] = 4; p[1] = 4; p[2] = (uint8_t)node; p[3] = (uint8_t)(node >> 8);   /* media, file path */
    for (i = 0; i < chars; i++) { p[4 + i * 2] = (uint8_t)path[i]; p[5 + i * 2] = (uint8_t)(path[i] >> 8); }
    p += node;
    p[0] = 0x7f; p[1] = 0xff; p[2] = 4; p[3] = 0;
    status = bs_fn(bs, 22)(0, (uint64_t)parent, (uint64_t)full, 0, 0, (uint64_t)&loaded);
    if (status || !loaded) return status ? status : 0x8000000000000002ull;
    return bs_fn(bs, 23)((uint64_t)loaded, 0, 0, 0, 0, 0);
}

extern int64_t HyperwinInstallPciSsdt(void *system);
extern int64_t HyperwinInstallCpuSsdt(void *system);

uint64_t SetupBootEntry(void *image, void *system)
{
    static const uint8_t li_guid[16] = {
        0xa1, 0x31, 0x1b, 0x5b, 0x62, 0x95, 0xd2, 0x11,
        0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b
    };
    static const uint8_t fs_guid[16] = {
        0x22, 0x5b, 0x4e, 0x96, 0x59, 0x64, 0xd2, 0x11,
        0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b
    };
    static const uint16_t gop_path[] = {
        '\\','E','F','I','\\','B','O','O','T','\\','G','O','P','.','E','F','I',0
    };
    static const uint16_t compat_path[] = {
        '\\','E','F','I','\\','B','O','O','T','\\','C','O','M','P','A','T','.','E','F','I',0
    };
    static const uint16_t noprompt_path[] = {
        '\\','E','F','I','\\','B','O','O','T','\\','C','D','B','O','O','T','N','P','.','E','F','I',0
    };
    static const uint16_t bootmgfw_path[] = {
        '\\','E','F','I','\\','M','i','c','r','o','s','o','f','t','\\','B','o','o','t','\\',
        'b','o','o','t','m','g','f','w','.','e','f','i',0
    };
    static const uint16_t win_path[] = {
        '\\','E','F','I','\\','B','O','O','T','\\','B','O','O','T','A','A','6','4','.','E','F','I',0
    };
    struct hw_header *h = (struct hw_header *)HW_WINDOW_PA;
    void *bs = *(void **)((uint8_t *)system + 0x60);
    void *loaded = 0, *device = 0, *fs = 0, *ours = 0;
    void **handles = 0;
    uint64_t address = HW_WINDOW_PA, count = 0, i, status;
    int has_window = (h->magic[0] == 'H' && h->magic[1] == 'Y' && h->version == HW_VERSION);
    if (has_window) {
        for (i = 0; i < HW_WINDOW_SIZE; i += 2u * 1024u * 1024u) {
            address = HW_WINDOW_PA + i;
            bs_fn(bs, 2)(2, 0, (2u * 1024u * 1024u) / 4096, (uint64_t)&address, 0, 0);
        }
        paint(0);                 /* black, and byte-for-byte the frame the host starts with */
    }
    status = bs_fn(bs, 16)((uint64_t)image, (uint64_t)li_guid, (uint64_t)&loaded, 0, 0, 0);
    if (status || !loaded) return status;
    device = *(void **)((uint8_t *)loaded + 0x18);
    status = bs_fn(bs, 16)((uint64_t)device, (uint64_t)fs_guid, (uint64_t)&fs, 0, 0, 0);
    if (status || !fs) return status;
    status = proto_fn(fs, 0)((uint64_t)fs, (uint64_t)&ours, 0, 0, 0, 0);
    if (status || !ours) return status;
    start_path(bs, image, ours, gop_path);
    status = start_path(bs, image, ours, compat_path);
    if (status) return status;
    {
        volatile uint32_t *diag = (volatile uint32_t *)(HW_WINDOW_PA + HW_DIAG);
        diag[1] = (uint32_t)HyperwinInstallPciSsdt(system);
        diag[127] = (uint32_t)HyperwinInstallCpuSsdt(system);   /* processors described, or < 0 */
    }
    /* A prepared machine disk carries Windows on this same disk. */
    {
        void *probe = 0;
        if (!proto_fn(ours, 0)((uint64_t)ours, (uint64_t)&probe, (uint64_t)bootmgfw_path, 1, 0, 0) && probe) {
            proto_fn(probe, 1)((uint64_t)probe, 0, 0, 0, 0, 0);
            if (!start_on_device(bs, image, device, bootmgfw_path)) return 0;
        }
    }
    status = bs_fn(bs, 36)(2, (uint64_t)fs_guid, 0, (uint64_t)&count, (uint64_t)&handles, 0);
    if (status || !handles) return status;
    for (i = 0; i < count; i++) {
        if (handles[i] == device) continue;
        status = start_on_device(bs, image, handles[i], noprompt_path);
        if (!status) return 0;
        status = start_on_device(bs, image, handles[i], win_path);
        if (!status) return 0;
    }
    if (has_window) paint(0xffe02020u);
    return 0x800000000000000eull;
}
