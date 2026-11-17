#include "hw_abi.h"

typedef uint64_t (*efi_install_protocol)(void **handle, const void *guid, uint32_t interface_type, void *interface);
typedef uint64_t (*efi_allocate_pool)(uint32_t type, uint64_t size, void **out);
typedef uint64_t (*efi_output_string)(void *protocol, const uint16_t *string);

struct efi_guid { uint32_t a; uint16_t b, c; uint8_t d[8]; };
#pragma pack(push, 1)
struct gop_memory_path {
    uint8_t type, subtype;
    uint16_t length;
    uint32_t memory_type;
    uint64_t start, end;
    uint8_t end_type, end_subtype;
    uint16_t end_length;
};
#pragma pack(pop)
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

extern uint64_t HyperwinInstallAcpi(void *system);
extern void HyperwinTraceInstall(void *system);
extern void HyperwinTraceNote(const char *what, uint64_t a, uint64_t b, uint64_t c);

#define EFI_INVALID_PARAMETER 0x8000000000000002ull
#define EFI_UNSUPPORTED       0x8000000000000003ull
#define EFI_OUT_OF_RESOURCES  0x8000000000000009ull

static const struct efi_guid gop_guid = {
    0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}
};
static const struct efi_guid device_path_guid = {
    0x09576e91, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};
static const struct efi_guid simple_text_output_guid = {
    0x387477c1, 0x69c7, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};
/* The shared window is BGRA in memory: EFI's BlueGreenRedReserved enum is 1. */
static struct gop_info mode_info = {0, HW_WIDTH, HW_HEIGHT, 1, 0, 0, 0, 0, HW_WIDTH};
static struct gop_mode mode = {1, 0, &mode_info, sizeof(mode_info), 0, HW_WIDTH * HW_HEIGHT * 4u};
/* The host picks the frame size (the phone's own, or smaller) and writes it into the header. */
#define FB_WIDTH ((uint64_t)mode_info.width)
#define FB_HEIGHT ((uint64_t)mode_info.height)
static struct gop_protocol protocol;
static struct gop_memory_path memory_path = {
    1, 3, 24, 0, 0, 0, 0x7f, 0xff, 4
};
static void *boot_services;
static void *console_out;

_Static_assert(sizeof(struct gop_info) == 36, "GOP mode info ABI");
_Static_assert(sizeof(struct gop_mode) == 40, "GOP mode ABI");
_Static_assert(sizeof(struct gop_memory_path) == 28, "GOP memory device path ABI");

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

/* Windows shows the SMBIOS system and processor text ("Gunyah Virtual Machine", "Gunyah vCPU" and
 * a clock guessed from the timer). The host knows the real phone, so put its names there through
 * the firmware's own SMBIOS protocol, which rebuilds the published table on every change. */
struct smbios_protocol {
    void *add;
    uint64_t (*update_string)(struct smbios_protocol *, uint16_t *handle, uint64_t *number, const char *text);
    void *remove;
    uint64_t (*get_next)(struct smbios_protocol *, uint16_t *handle, uint8_t *type, uint8_t **record, void **producer);
};
typedef uint64_t (*efi_locate_protocol)(const void *guid, void *registration, void **out);
static const struct efi_guid smbios_guid = {
    0x03583ff6, 0xcb36, 0x4940, {0x94, 0x7e, 0xb9, 0xb3, 0x9f, 0x4a, 0xfa, 0xf7}
};

static void set_string(struct smbios_protocol *s, uint16_t handle, uint8_t number, const char *text)
{
    uint64_t n = number;
    if (number && text[0]) s->update_string(s, &handle, &n, text);
}

static void copy_text(char *dst, const char *src, uint32_t size)
{
    uint32_t i;
    for (i = 0; i + 1 < size && src[i] >= 0x20 && src[i] < 0x7f; i++) dst[i] = src[i];
    dst[i] = 0;
}

static void apply_identity(void *bs, const struct hw_header *h)
{
    efi_locate_protocol locate = *(efi_locate_protocol *)((uint8_t *)bs + 24 + 37 * 8);
    struct smbios_protocol *s = 0;
    char maker[32], model[48], cpu_maker[32], cpu_name[64];
    uint32_t mhz = h->cpu_mhz;
    uint8_t type;
    copy_text(maker, h->system_maker, sizeof maker);
    copy_text(model, h->system_model, sizeof model);
    copy_text(cpu_maker, h->cpu_maker, sizeof cpu_maker);
    copy_text(cpu_name, h->cpu_name, sizeof cpu_name);
    if (locate(&smbios_guid, 0, (void **)&s) || !s) return;
    for (type = 1; type <= 4; type++) {
        uint16_t handle = 0xfffe;                  /* "first record" */
        uint8_t *r, a, b, c = 0;
        if (s->get_next(s, &handle, &type, &r, 0) || !r) continue;
        if (type == 4) {
            if (mhz && mhz < 0xffff) {                 /* max and current speed, in MHz */
                r[0x14] = r[0x16] = (uint8_t)mhz;
                r[0x15] = r[0x17] = (uint8_t)(mhz >> 8);
            }
            a = r[0x07];                               /* string numbers; r moves on an update */
            b = r[0x10];
            set_string(s, handle, a, cpu_maker);
            set_string(s, handle, b, cpu_name);
        } else {
            a = r[0x04];                               /* manufacturer */
            b = type == 3 ? r[0x06] : r[0x05];         /* product (the chassis keeps it as version) */
            if (type == 1) c = r[0x1a];                /* family */
            set_string(s, handle, a, maker);
            set_string(s, handle, b, model);
            set_string(s, handle, c, model);
        }
    }
}

static uint64_t query_mode(struct gop_protocol *self, uint32_t requested, uint64_t *size, struct gop_info **info)
{
    efi_allocate_pool allocate = *(efi_allocate_pool *)((uint8_t *)boot_services + 24 + 5 * 8);
    struct gop_info *copy;
    (void)self;
    HyperwinTraceNote("GOP.QueryMode", requested, 0, 0);
    if (requested || !size || !info) return EFI_INVALID_PARAMETER;
    if (allocate(4, sizeof(*copy), (void **)&copy)) return EFI_OUT_OF_RESOURCES;
    *copy = mode_info;
    *size = sizeof(*copy);
    *info = copy;
    return 0;
}

static uint64_t set_mode(struct gop_protocol *self, uint32_t requested)
{
    uint8_t *frame;
    uint64_t i, bytes;
    HyperwinTraceNote("GOP.SetMode", requested, 0, 0);
    if (requested != 0) return EFI_UNSUPPORTED;
    if (!self || !self->mode || !self->mode->info || !self->mode->frame_buffer_base)
        return EFI_INVALID_PARAMETER;
    bytes = self->mode->frame_buffer_size;
    if (bytes < FB_WIDTH * FB_HEIGHT * 4u) return EFI_INVALID_PARAMETER;
    frame = (uint8_t *)self->mode->frame_buffer_base;
    for (i = 0; i < FB_WIDTH * FB_HEIGHT; ++i) {
        frame[i * 4u] = 0;
        frame[i * 4u + 1u] = 0;
        frame[i * 4u + 2u] = 0;
        frame[i * 4u + 3u] = 0;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return 0;
}

static int rect_in_frame(uint64_t x, uint64_t y, uint64_t width, uint64_t height)
{
    return x <= FB_WIDTH && y <= FB_HEIGHT && width <= FB_WIDTH - x && height <= FB_HEIGHT - y;
}

static uint64_t blt(struct gop_protocol *self, struct gop_pixel *buffer, uint32_t operation,
                    uint64_t source_x, uint64_t source_y, uint64_t dest_x, uint64_t dest_y,
                    uint64_t width, uint64_t height, uint64_t delta)
{
    struct gop_pixel *fb = (struct gop_pixel *)self->mode->frame_buffer_base;
    uint64_t x, y, pitch = delta ? delta / sizeof(*buffer) : width;
    HyperwinTraceNote("GOP.Blt", operation, width, height);
    if (operation > 3 || (operation != 3 && !buffer)) return EFI_INVALID_PARAMETER;
    if ((operation == 0 || operation == 2) && !rect_in_frame(dest_x, dest_y, width, height)) return EFI_INVALID_PARAMETER;
    if ((operation == 1 || operation == 3) && !rect_in_frame(source_x, source_y, width, height)) return EFI_INVALID_PARAMETER;
    if ((operation == 2 || operation == 3) && !rect_in_frame(dest_x, dest_y, width, height)) return EFI_INVALID_PARAMETER;
    if (operation == 0) {
        struct gop_pixel color = *buffer;
        for (y = 0; y < height; y++) for (x = 0; x < width; x++) fb[(dest_y + y) * FB_WIDTH + dest_x + x] = color;
    } else if (operation == 1) {
        for (y = 0; y < height; y++) for (x = 0; x < width; x++) buffer[(dest_y + y) * pitch + dest_x + x] = fb[(source_y + y) * FB_WIDTH + source_x + x];
    } else if (operation == 2) {
        for (y = 0; y < height; y++) for (x = 0; x < width; x++) fb[(dest_y + y) * FB_WIDTH + dest_x + x] = buffer[(source_y + y) * pitch + source_x + x];
    } else {
        int64_t row = dest_y > source_y ? (int64_t)height - 1 : 0;
        int64_t step = dest_y > source_y ? -1 : 1;
        for (; row >= 0 && row < (int64_t)height; row += step) {
            uint64_t col;
            if (dest_y == source_y && dest_x > source_x) {
                for (col = width; col > 0; col--) fb[(dest_y + (uint64_t)row) * FB_WIDTH + dest_x + col - 1] = fb[(source_y + (uint64_t)row) * FB_WIDTH + source_x + col - 1];
            } else {
                for (col = 0; col < width; col++) fb[(dest_y + (uint64_t)row) * FB_WIDTH + dest_x + col] = fb[(source_y + (uint64_t)row) * FB_WIDTH + source_x + col];
            }
        }
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return 0;
}

uint64_t GopDriverEntry(void *image, void *system)
{
    struct hw_header *h = (struct hw_header *)HW_WINDOW_PA;
    void *bs = *(void **)((uint8_t *)system + 0x60);
    efi_install_protocol install = *(efi_install_protocol *)((uint8_t *)bs + 24 + 13 * 8);
    void *display_handle = 0;
    uint64_t status, acpi_status;
    (void)image;
    boot_services = bs;
    console_out = *(void **)((uint8_t *)system + 0x40);
    output("HYPERWIN GOP DRIVER\r\n");
    if (h->magic[0] != 'H' || h->magic[1] != 'Y' || h->magic[2] != 'P' ||
        h->magic[3] != 'R' || h->magic[4] != 'W' || h->magic[5] != 'I' ||
        h->magic[6] != 'N' || h->magic[7] != '1' || h->version != HW_VERSION ||
        h->header_bytes < sizeof(*h) || h->window_size != HW_WINDOW_SIZE ||
        !hw_mode_ok(h) || __atomic_load_n(&h->front, __ATOMIC_ACQUIRE) > 1 ||
        !( __atomic_load_n(&h->guest_flags, __ATOMIC_ACQUIRE) & HW_GUEST_RING_RESERVED)) {
        output("shared ring invalid or unreserved\r\n");
        return EFI_INVALID_PARAMETER;
    }
    mode_info.width = mode_info.pixels_per_scanline = h->width;
    mode_info.height = h->height;
    apply_identity(bs, h);
    acpi_status = HyperwinInstallAcpi(system);
    output(acpi_status == 0 ? "ACPI tables installed\r\n" : "ACPI table install failed\r\n");
    protocol.query_mode = query_mode;
    protocol.set_mode = set_mode;
    protocol.blt = blt;
    protocol.mode = &mode;
    mode.info = &mode_info;
    mode.size_of_info = sizeof(mode_info);
    mode.frame_buffer_base = (uint64_t)h + hw_frame_off(h, __atomic_load_n(&h->front, __ATOMIC_ACQUIRE));
    mode.frame_buffer_size = (uint64_t)h->stride * h->height;
    memory_path.start = mode.frame_buffer_base;
    memory_path.end = mode.frame_buffer_base + mode.frame_buffer_size - 1;
    /* Publish a discoverable display handle with its own memory-mapped path.
     * The text-output interface forwards to the existing AVF serial console. */
    status = install(&display_handle, &device_path_guid, 0, &memory_path);
    if (status) {
        output("GOP display device path install failed\r\n");
        return status;
    }
    status = install(&display_handle, &gop_guid, 0, &protocol);
    if (status) {
        output("GOP protocol install failed\r\n");
        return status;
    }
    status = install(&display_handle, &simple_text_output_guid, 0, console_out);
    if (status) {
        output("GOP text console association failed\r\n");
        return status;
    }
    *(void **)((uint8_t *)system + 0x38) = display_handle;
    output("GOP installed on memory-mapped console handle\r\n");
    __atomic_or_fetch(&h->guest_flags, HW_GUEST_GOP, __ATOMIC_RELEASE);
    HyperwinTraceInstall(system);
    return 0;
}
