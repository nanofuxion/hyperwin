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
#define HWW_ESC_CHANNEL  7u   /* chan.index -> chan: the index-th open channel, for submits per program */
#define HWW_ESC_GDI_COPY 8u   /* gdi: copy a mapped GDI redirection bitmap's pages to caller buffer */

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
    uint64_t submits_to_phone;       /* ring writes by programs, closed channels and open ones */
    uint64_t submits_to_guest;       /* ring writes by the phone */
    uint64_t marker_submits;         /* submissions with a marker through DxgkDdiSubmitCommand */
};

/* One open channel's own numbers: who has it and how many submits crossed it. */
struct hww_chan {
    uint32_t index;                  /* in: 0, 1, ... over the open channels */
    uint32_t slot;                   /* out */
    uint32_t pid;                    /* out: the process that has it */
    uint32_t reserved;
    uint64_t submits_to_phone;
    uint64_t submits_to_guest;
};

struct hww_gdi_copy {
    uint32_t halloc;        /* in: D3DKMT_HANDLE of the GDI allocation */
    uint32_t flags;         /* in: bit 0 = skip copy if hash matches prev_hash */
    uint64_t dst_user;      /* in: caller's user buffer */
    uint32_t dst_size;      /* in: buffer size in bytes */
    uint32_t copied;        /* out: bytes copied (0 if unchanged) */
    uint32_t width, height; /* out: allocation dimensions */
    uint32_t map_pages;     /* out: mapped aperture pages (0 if unmapped) */
    uint32_t nmap;          /* out: global MapApertureSegment count */
    uint32_t px0, px1;      /* out: first two 32-bit pixels */
    uint64_t hash;          /* in: prev_hash (when flags & 1); out: current 64-bit hash */
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
        struct hww_chan chan;
        struct hww_gdi_copy gdi;
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
_Static_assert(sizeof(struct hww_counters) == 96, "hww_counters");
_Static_assert(sizeof(struct hww_chan) == 32, "hww_chan");
_Static_assert(sizeof(struct hww_escape) == 16 + 96, "hww_escape");
_Static_assert(sizeof(struct hww_marker) == 24, "hww_marker");

/* What an allocation is, as its private driver data: written by the kernel driver for Windows' own
 * surfaces (the desktop's primary and friends) and by the user-mode driver for a program's. Windows
 * hands it back to whoever opens the allocation, in any process, so `id` names the surface everywhere. */
#define HWW_ALLOC_MAGIC   0x4c415748u   /* "HWAL" */
#define HWW_ALLOC_PRIMARY 1u   /* a screen's primary surface */
#define HWW_ALLOC_SHADOW  2u
#define HWW_ALLOC_STAGING 3u
#define HWW_ALLOC_UMD     4u   /* made by the user-mode driver */
struct hww_alloc {
    uint32_t magic;
    uint32_t kind;
    uint32_t width, height;
    uint32_t format;      /* D3DDDIFORMAT for Windows' own, DXGI_FORMAT for the user-mode driver's */
    uint32_t source;      /* screen (VidPN source) of a primary */
    uint64_t id;          /* kernel-made ids have the top bit set */
};
_Static_assert(sizeof(struct hww_alloc) == 32, "hww_alloc");

#endif
