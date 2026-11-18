/* GPU byte rings in the HyperWin shared window (include/hw_abi.h).
 *
 * The same SweetCider sgpu stream (sgpu_proto.h) that once went over vsock goes through RAM here.
 * Each Windows process that opens the GPU gets one channel: a control page, a guest-to-phone ring
 * and a phone-to-guest ring. Each ring has one producer and one consumer.
 *
 * Who does what:
 *   phone app  hwgpu_ring_init() once per boot, then serves every channel (gpu/host).
 *   guest      the kernel side claims a free channel for a process and frees it when the
 *              process's handle closes (gpu/guest/direct/hwgpu.c). The process itself only ever
 *              sees its own channel, mapped into it, never the window.
 *   phone app  sees CLOSING, drops what that process made, marks the channel FREE.
 *
 * The phone publishes finished fences in the control page instead of sending messages.
 *
 * Build with HWGPU_KERNEL in a Windows kernel driver: the blocking helpers are left out.
 */
#ifndef HYPERWIN_HWGPU_RING_H
#define HYPERWIN_HWGPU_RING_H

#include <stdint.h>
#include <stddef.h>

/* Above the largest frame pair (4096x2304 BGRA twice ends at 0x5800000) and below the guest
 * debug marks at HW_FRAME_END (0x7f00000). */
#define HWGPU_RING_OFF      0x6000000u
#define HWGPU_MAGIC         0x474e495255504748ull /* "HGPURING" little endian */
#define HWGPU_VERSION       2u  /* 2: heap grants */
#define HWGPU_CHANNELS      8u
#define HWGPU_CTL_BYTES     0x1000u
#define HWGPU_TX_BYTES      (2u << 20)  /* guest -> phone */
#define HWGPU_RX_BYTES      (1u << 20)  /* phone -> guest */
#define HWGPU_CHAN_STRIDE   (HWGPU_CTL_BYTES + HWGPU_TX_BYTES + HWGPU_RX_BYTES)
#define HWGPU_RING_BYTES    (0x1000u + HWGPU_CHANNELS * HWGPU_CHAN_STRIDE)
#define HWGPU_FENCE_RINGS   64u
#define HWGPU_GRANTS        128u  /* heap ranges one channel may map at once */
#define HWGPU_SCANOUT_BUFFERS 16u /* buffers one channel may show with SGPU_CMD_SCANOUT */

_Static_assert(HWGPU_RING_OFF + HWGPU_RING_BYTES <= 0x7f00000u, "GPU rings end below the debug marks");

/* guest_state */
#define HWGPU_FREE      0u
#define HWGPU_CONNECTED 2u
#define HWGPU_CLOSING   3u
/* host_state */
#define HWGPU_HOST_IDLE  0u
#define HWGPU_HOST_READY 1u

/* Context and resource ids belong to one channel: ctx = slot + 1, resources in
 * [(slot + 1) << 24, (slot + 2) << 24). The phone refuses ids from another channel. */
#define HWGPU_CTX(slot)      ((uint32_t)(slot) + 1u)
#define HWGPU_RES_BASE(slot) (((uint32_t)(slot) + 1u) << 24)

struct hwgpu_ring_hdr {
    uint64_t magic;
    uint32_t version;
    uint32_t channels;
    uint32_t tx_bytes;
    uint32_t rx_bytes;
    uint32_t host_tick;  /* the phone bumps it while its server runs */
    uint32_t reserved;
    /* phone: a value that differs for every renderer process. The WDDM adapter sees it change
     * when a driver swap restarts the renderer, and resets (wddm/README.md). */
    uint64_t epoch;
    char gpu_name[64];   /* phone: the GPU's name, for the adapter's name in Windows */
    /* phone: bytes it copied into / out of guest memory to keep a resource in step (the sgpu sync
     * path: memory that could not be shared). The counters of wddm/README.md. */
    uint64_t copy_to_guest, copy_from_guest;
};

/* First page of each channel. */
struct hwgpu_ctl {
    uint32_t guest_state;
    uint32_t host_state;
    uint32_t generation;
    uint32_t slot;
    uint64_t tx_head;  /* guest publishes */
    uint64_t tx_tail;  /* phone consumes */
    uint64_t rx_head;  /* phone publishes */
    uint64_t rx_tail;  /* guest consumes */
    uint64_t fence[HWGPU_FENCE_RINGS];  /* phone: last finished fence id per ring index */
    /* phone: parts of the heap (HW_HEAP_PA) this channel's process may map, size 0 for unused.
     * hwgpu.sys maps only a range that matches one exactly. */
    struct { uint64_t offset, size; } grant[HWGPU_GRANTS];
    /* phone: per scanout buffer, the seq of its last SGPU_CMD_SCANOUT that the screen has let go
     * of. The guest draws into a buffer again only once this has reached what it last sent. */
    uint32_t scanout_free[HWGPU_SCANOUT_BUFFERS];
    /* phone: HWGPU_SCANOUT_VSYNC when every scanout frame should wait for the phone's screen, as
     * if the program had asked for FIFO (the app's "Limit to screen refresh" setting). */
    uint32_t scanout_flags;
};
#define HWGPU_SCANOUT_VSYNC 1u

_Static_assert(sizeof(struct hwgpu_ctl) <= HWGPU_CTL_BYTES, "control fits its page");

static inline struct hwgpu_ring_hdr *hwgpu_hdr(void *window)
{
    return (struct hwgpu_ring_hdr *)((uint8_t *)window + HWGPU_RING_OFF);
}

/* Offset of a channel from the start of the window. */
static inline uint32_t hwgpu_chan_off(uint32_t slot)
{
    return HWGPU_RING_OFF + 0x1000u + slot * HWGPU_CHAN_STRIDE;
}

/* A channel as one block: control page, tx ring, rx ring. */
static inline struct hwgpu_ctl *hwgpu_chan_ctl(void *chan) { return (struct hwgpu_ctl *)chan; }
static inline uint8_t *hwgpu_tx(void *chan) { return (uint8_t *)chan + HWGPU_CTL_BYTES; }
static inline uint8_t *hwgpu_rx(void *chan) { return (uint8_t *)chan + HWGPU_CTL_BYTES + HWGPU_TX_BYTES; }

static inline int hwgpu_ring_valid(void *window)
{
    struct hwgpu_ring_hdr *h = hwgpu_hdr(window);
    return __atomic_load_n(&h->magic, __ATOMIC_ACQUIRE) == HWGPU_MAGIC && h->version == HWGPU_VERSION &&
           h->channels == HWGPU_CHANNELS && h->tx_bytes == HWGPU_TX_BYTES && h->rx_bytes == HWGPU_RX_BYTES;
}

/* Guest kernel: reserve a free channel for one process. Returns the slot or -1. */
static inline int hwgpu_claim(void *window, uint32_t *generation)
{
    if (!hwgpu_ring_valid(window)) return -1;
    for (uint32_t i = 0; i < HWGPU_CHANNELS; i++) {
        struct hwgpu_ctl *c = hwgpu_chan_ctl((uint8_t *)window + hwgpu_chan_off(i));
        uint32_t expected = HWGPU_FREE;
        /* Claim in a state the phone ignores, reset, then publish CONNECTED. */
        if (!__atomic_compare_exchange_n(&c->guest_state, &expected, HWGPU_CLOSING + 1u, 0,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;
        c->tx_head = c->tx_tail = c->rx_head = c->rx_tail = 0;
        c->slot = i;
        __atomic_store_n(&c->host_state, HWGPU_HOST_IDLE, __ATOMIC_RELAXED);
        *generation = __atomic_add_fetch(&c->generation, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&c->guest_state, HWGPU_CONNECTED, __ATOMIC_RELEASE);
        return (int)i;
    }
    return -1;
}

/* Guest kernel: the process let go. The phone cleans up and frees the channel. */
static inline void hwgpu_release(void *window, uint32_t slot)
{
    struct hwgpu_ctl *c = hwgpu_chan_ctl((uint8_t *)window + hwgpu_chan_off(slot));
    __atomic_store_n(&c->guest_state, HWGPU_CLOSING, __ATOMIC_RELEASE);
}

static inline size_t hwgpu_write_some(uint8_t *data, uint32_t capacity, uint64_t *head, uint64_t *tail,
                                      const void *src, size_t bytes)
{
    const uint64_t h = __atomic_load_n(head, __ATOMIC_RELAXED);
    const uint64_t used = h - __atomic_load_n(tail, __ATOMIC_ACQUIRE);
    if (used >= capacity) return 0;
    const size_t n = bytes < capacity - used ? bytes : (size_t)(capacity - used);
    const size_t at = (size_t)(h % capacity);
    const size_t first = n < capacity - at ? n : capacity - at;
    __builtin_memcpy(data + at, src, first);
    if (n > first) __builtin_memcpy(data, (const uint8_t *)src + first, n - first);
    __atomic_store_n(head, h + n, __ATOMIC_RELEASE);
    return n;
}

static inline size_t hwgpu_read_some(const uint8_t *data, uint32_t capacity, uint64_t *head, uint64_t *tail,
                                     void *dst, size_t bytes)
{
    const uint64_t t = __atomic_load_n(tail, __ATOMIC_RELAXED);
    const uint64_t avail = __atomic_load_n(head, __ATOMIC_ACQUIRE) - t;
    if (!avail) return 0;
    const size_t n = bytes < avail ? bytes : (size_t)avail;
    const size_t at = (size_t)(t % capacity);
    const size_t first = n < capacity - at ? n : capacity - at;
    __builtin_memcpy(dst, data + at, first);
    if (n > first) __builtin_memcpy((uint8_t *)dst + first, data, n - first);
    __atomic_store_n(tail, t + n, __ATOMIC_RELEASE);
    return n;
}

#ifndef HWGPU_KERNEL
#ifdef _WIN32
#include <windows.h>
/* Sleep() rounds up to the system timer, often 15.6 ms, so waits of a millisecond or more use a
 * high-resolution timer, one per thread. */
static inline void hwgpu_pause(unsigned us)
{
    static __thread HANDLE timer;
    if (us < 1000) {
        SwitchToThread();
        return;
    }
    if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                               TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)us * 10;
    if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
    else Sleep(us / 1000);
}
#else
#include <unistd.h>
static inline void hwgpu_pause(unsigned us) { usleep(us); }
#endif

/* No doorbell crosses the VM boundary, so a blocked side polls: short waits first, then at most
 * every 300 us. *misses resets on every transfer. */
static inline void hwgpu_backoff(uint32_t *misses)
{
    if (*misses < 200) ++*misses;
    hwgpu_pause(*misses < 20 ? 50 : *misses < 200 ? 100 : 300);
}

/* Both sides: the channel is still the connection this side opened. */
static inline int hwgpu_alive(struct hwgpu_ctl *c, uint32_t generation)
{
    return __atomic_load_n(&c->generation, __ATOMIC_ACQUIRE) == generation &&
           __atomic_load_n(&c->guest_state, __ATOMIC_ACQUIRE) == HWGPU_CONNECTED &&
           __atomic_load_n(&c->host_state, __ATOMIC_ACQUIRE) == HWGPU_HOST_READY;
}

/* Moves all bytes, waiting for room or data. 0 when the connection ends first.
 * to_phone picks the ring: the guest writes tx and reads rx, the phone the other way round. */
static inline int hwgpu_write_all(void *chan, uint32_t generation, int to_phone, const void *src, size_t bytes)
{
    struct hwgpu_ctl *c = hwgpu_chan_ctl(chan);
    uint8_t *data = to_phone ? hwgpu_tx(chan) : hwgpu_rx(chan);
    uint32_t cap = to_phone ? HWGPU_TX_BYTES : HWGPU_RX_BYTES;
    uint64_t *head = to_phone ? &c->tx_head : &c->rx_head, *tail = to_phone ? &c->tx_tail : &c->rx_tail;
    const uint8_t *p = (const uint8_t *)src;
    uint32_t misses = 0;
    while (bytes) {
        if (!hwgpu_alive(c, generation)) return 0;
        size_t n = hwgpu_write_some(data, cap, head, tail, p, bytes);
        if (!n) { hwgpu_backoff(&misses); continue; }
        misses = 0;
        p += n;
        bytes -= n;
    }
    return 1;
}

static inline int hwgpu_read_all(void *chan, uint32_t generation, int from_phone, void *dst, size_t bytes)
{
    struct hwgpu_ctl *c = hwgpu_chan_ctl(chan);
    const uint8_t *data = from_phone ? hwgpu_rx(chan) : hwgpu_tx(chan);
    uint32_t cap = from_phone ? HWGPU_RX_BYTES : HWGPU_TX_BYTES;
    uint64_t *head = from_phone ? &c->rx_head : &c->tx_head, *tail = from_phone ? &c->rx_tail : &c->tx_tail;
    uint8_t *p = (uint8_t *)dst;
    uint32_t misses = 0;
    while (bytes) {
        if (!hwgpu_alive(c, generation)) return 0;
        size_t n = hwgpu_read_some(data, cap, head, tail, p, bytes);
        if (!n) { hwgpu_backoff(&misses); continue; }
        misses = 0;
        p += n;
        bytes -= n;
    }
    return 1;
}
#endif /* HWGPU_KERNEL */

#endif
