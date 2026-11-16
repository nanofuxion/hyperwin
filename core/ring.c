#include "ring.h"

#if defined(HYPERWIN_FREESTANDING)
static void hw_copy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
}
static void hw_zero(void *dst, size_t n)
{
    uint8_t *d = dst;
    while (n--) *d++ = 0;
}
#else
#include <string.h>
#define hw_copy memcpy
#define hw_zero(p, n) memset((p), 0, (n))
#endif

void hw_window_init(void *window, size_t bytes)
{
    struct hw_header *h = window;
    if (!window || bytes < HW_WINDOW_SIZE) return;
    hw_zero(window, 0x1000);
    hw_copy(h->magic, HW_MAGIC, 8);
    h->version = HW_VERSION;
    h->header_bytes = 0x1000;
    h->window_size = HW_WINDOW_SIZE;
    h->width = HW_WIDTH;
    h->height = HW_HEIGHT;
    h->stride = HW_WIDTH * 4;
    h->format = HW_FORMAT_BGRA;
    h->enable = 1;
    h->audio_rate = 48000;
    h->audio_channels = 2;
    __atomic_store_n(&h->seq, 0, __ATOMIC_RELAXED);
}

void hw_publish_frame(void *window, const uint8_t *bgra, uint32_t width, uint32_t height, uint32_t stride)
{
    struct hw_header *h = window;
    uint32_t back, y, row;
    uint8_t *dst;
    if (!h || h->magic[0] != 'H') return;
    /* GOP fixes one ring page as the linear framebuffer until VM teardown. */
    if (__atomic_load_n(&h->guest_flags, __ATOMIC_ACQUIRE) & HW_GUEST_GOP) return;
    if (width != h->width || height != h->height) return;
    back = __atomic_load_n(&h->front, __ATOMIC_RELAXED) ^ 1u;
    dst = (uint8_t *)window + hw_frame_off(h, back);
    row = h->stride;
    for (y = 0; y < height; y++) hw_copy(dst + (size_t)y * row, bgra + (size_t)y * stride, row);
    __atomic_store_n(&h->front, back, __ATOMIC_RELEASE);
    __atomic_add_fetch(&h->seq, 1, __ATOMIC_RELEASE);
}

void hw_publish_audio(void *window, const int16_t *stereo, uint32_t frames)
{
    struct hw_header *h = window;
    uint8_t *ring;
    uint64_t head, tail;
    uint32_t bytes, cap;
    if (!h || !stereo) return;
    cap = HW_AUDIO_BYTES;
    bytes = frames * h->audio_channels * (uint32_t)sizeof(int16_t);
    ring = (uint8_t *)window + HW_AUDIO;
    head = __atomic_load_n(&h->audio_head, __ATOMIC_RELAXED);
    tail = __atomic_load_n(&h->audio_tail, __ATOMIC_ACQUIRE);
    if (head - tail + bytes > cap) return;
    {
        uint32_t at = (uint32_t)(head % cap);
        uint32_t first = bytes < cap - at ? bytes : cap - at;
        hw_copy(ring + at, stereo, first);
        if (bytes > first) hw_copy(ring, (const uint8_t *)stereo + first, bytes - first);
    }
    __atomic_store_n(&h->audio_head, head + bytes, __ATOMIC_RELEASE);
}

void hw_set_pad(void *window, uint32_t connected, uint32_t packet, uint16_t buttons,
                uint8_t left_trigger, uint8_t right_trigger,
                int16_t lx, int16_t ly, int16_t rx, int16_t ry)
{
    struct hw_header *h = window;
    h->buttons = buttons;
    h->left_trigger = left_trigger;
    h->right_trigger = right_trigger;
    h->left_x = lx;
    h->left_y = ly;
    h->right_x = rx;
    h->right_y = ry;
    h->pad_packet = packet;
    __atomic_store_n(&h->pad_connected, connected, __ATOMIC_RELEASE);
}

int hw_copy_front(void *window, uint8_t *dst, uint32_t dst_bytes)
{
    struct hw_header *h = window;
    uint32_t seq, front, bytes;
    seq = __atomic_load_n(&h->seq, __ATOMIC_ACQUIRE);
    if (!seq) return 0;
    front = __atomic_load_n(&h->front, __ATOMIC_ACQUIRE);
    bytes = hw_frame_bytes(h);
    if (bytes > dst_bytes) return -1;
    hw_copy(dst, (uint8_t *)window + hw_frame_off(h, front), bytes);
    if (__atomic_load_n(&h->seq, __ATOMIC_ACQUIRE) != seq) return 0;
    return (int)seq;
}

int hw_copy_gop_front(void *window, uint8_t *dst, uint32_t dst_bytes)
{
    struct hw_header *h = window;
    uint32_t front, bytes;
    uint64_t offset;
    if (!h || !dst || h->magic[0] != 'H' || h->magic[1] != 'Y' ||
        h->magic[2] != 'P' || h->magic[3] != 'R' || h->magic[4] != 'W' ||
        h->magic[5] != 'I' || h->magic[6] != 'N' || h->magic[7] != '1' ||
        h->version != HW_VERSION || h->width != HW_WIDTH || h->height != HW_HEIGHT ||
        h->stride != HW_WIDTH * 4u || h->format != HW_FORMAT_BGRA ||
        h->window_size != HW_WINDOW_SIZE) return -1;
    front = __atomic_load_n(&h->front, __ATOMIC_ACQUIRE);
    if (front > 1) return -1;
    bytes = hw_frame_bytes(h);
    offset = hw_frame_off(h, front);
    if (bytes > dst_bytes || offset > h->window_size || bytes > h->window_size - offset) return -1;
    hw_copy(dst, (uint8_t *)window + offset, bytes);
    if (__atomic_load_n(&h->front, __ATOMIC_ACQUIRE) != front) return 0;
    return -2;
}

int hw_tpm_poll(void *window, struct hw_tpm *tpm)
{
    struct hw_header *h = window;
    uint32_t req, len, n;
    req = __atomic_load_n(&h->tpm_req_seq, __ATOMIC_ACQUIRE);
    if (req == __atomic_load_n(&h->tpm_resp_seq, __ATOMIC_RELAXED)) return 0;
    len = h->tpm_req_len;
    if (len > sizeof(h->tpm_req)) len = sizeof(h->tpm_req);
    n = hw_tpm_exec(tpm, h->tpm_req, len, h->tpm_resp, sizeof(h->tpm_resp));
    h->tpm_resp_len = n;
    __atomic_store_n(&h->tpm_resp_seq, req, __ATOMIC_RELEASE);
    return 1;
}
