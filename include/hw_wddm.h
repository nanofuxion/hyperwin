/* The private escape between HyperWin's user-mode drivers and its WDDM adapter (wddm/README.md).
 * A program opens the adapter through D3DKMT, sends one of these with D3DKMTEscape, and gets a GPU
 * channel mapped into itself. Our own definitions: nothing from the WDK is used here.
 *
 * The escape buffer is one struct hww_escape. The kernel driver fills the result in place and sets
 * status. Everything is a fixed width; sizes are checked on both sides. */
#ifndef HYPERWIN_HW_WDDM_H
#define HYPERWIN_HW_WDDM_H

#include <stdint.h>

#define HWW_MAGIC    0x57574848u   /* "HHWW" little endian */
#define HWW_VERSION  1u

/* The adapter's name as Windows shows it begins with this. */
#define HWW_ADAPTER_PREFIX "HyperWin GPU"

/* hww_escape.op */
#define HWW_ESC_INFO     1u   /* -> info */
#define HWW_ESC_OPEN     2u   /* -> open: a channel mapped into the caller */
#define HWW_ESC_CLOSE    3u   /* open.slot: frees the channel and its mappings */
#define HWW_ESC_MAP      4u   /* map: a range of the shared GPU heap the channel was granted */
#define HWW_ESC_UNMAP    5u   /* map.user */
#define HWW_ESC_COUNTERS 6u   /* -> counters */

#pragma pack(push, 8)
struct hww_info {
    uint32_t driver_version;   /* HWW_VERSION the kernel driver speaks */
    uint32_t channels;         /* channels in the ring */
    uint64_t epoch;            /* the renderer's; a new value means it restarted (a driver swap) */
    uint64_t heap_size;        /* the shared GPU heap */
    char gpu_name[64];
};

struct hww_open {
    uint64_t chan;             /* user address of the channel: control page, tx ring, rx ring */
    uint32_t slot;
    uint32_t generation;
};

struct hww_map {
    uint64_t offset, size;     /* in: from the start of the heap, page aligned */
    uint64_t user;             /* out: address in the caller */
    uint32_t slot;             /* in: the channel the range was granted to */
    uint32_t reserved;
};

/* Bytes that crossed between the phone and Windows' memory other than through a program's own
 * shared mappings, and counts of kernel work. All since the renderer started, from the ring's
 * header (the phone's) and the kernel driver (Windows'). */
struct hww_counters {
    uint64_t phone_copy_to_guest;    /* bytes the phone copied into Windows memory */
    uint64_t phone_copy_from_guest;  /* bytes the phone copied out of Windows memory */
    uint64_t kernel_copies;          /* bytes this driver copied for a program: 0 by design */
    uint64_t escapes;                /* D3DKMTEscape calls handled */
    uint64_t channels_opened;
    uint64_t mapped_bytes;           /* bytes of shared memory currently mapped into programs */
    uint64_t markers_pending;        /* fences Windows is waiting on */
    uint64_t markers_done;
    uint64_t resets;                 /* times the renderer was replaced under a running adapter */
};

struct hww_escape {
    uint32_t magic;            /* HWW_MAGIC */
    uint32_t size;             /* sizeof(struct hww_escape) */
    uint32_t op;
    int32_t status;            /* out: an NTSTATUS */
    union {
        struct hww_info info;
        struct hww_open open;
        struct hww_map map;
        struct hww_counters counters;
        uint32_t slot;         /* HWW_ESC_CLOSE */
    } u;
};

/* A marker the Direct3D driver part puts in a DMA buffer's private data when Windows needs to
 * know how far the phone has got: the kernel driver finishes the submission once the channel's
 * fence for ring reaches fence (hwgpu_ctl.fence[ring]). */
#define HWW_MARKER_MAGIC 0x4b52414du   /* "MARK" */
struct hww_marker {
    uint32_t magic;
    uint32_t slot;
    uint32_t generation;
    uint32_t ring;
    uint64_t fence;
};
#pragma pack(pop)

_Static_assert(sizeof(struct hww_info) == 88, "hww_info");
_Static_assert(sizeof(struct hww_map) == 32, "hww_map");
_Static_assert(sizeof(struct hww_counters) == 72, "hww_counters");
_Static_assert(sizeof(struct hww_escape) == 16 + 88, "hww_escape");
_Static_assert(sizeof(struct hww_marker) == 24, "hww_marker");

#endif
