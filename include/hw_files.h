/* Phone storage in Windows, through the shared window (include/hw_abi.h).
 *
 * The phone shares its storage volumes: internal storage, SD cards and USB drives. Windows shows
 * each one as a drive (guest/files/hwfiles.c, a WinFsp file system). Requests and file data go
 * through slots in the window, so nothing crosses a socket and nothing is copied twice.
 *
 * Who does what:
 *   phone app  hwf_init() once per boot, publishes the volumes, then serves the slots
 *              (app/src/main/cpp/files.c).
 *   guest      hwfiles.exe gets this region mapped by hwgpu.sys (IOCTL_HWGPU_FILES), claims a free
 *              slot per request, fills it, and waits for the answer.
 *
 * A slot goes FREE -> CLAIMED (guest fills it) -> REQUEST -> BUSY (phone works) -> DONE -> FREE.
 * One slot carries one request: a page of fields and paths, then up to HWF_DATA bytes of data.
 * Bigger reads and writes use several slots at once, which the phone serves in parallel.
 *
 * All numbers are little endian. Errors are Linux errno values; 0 is success.
 */
#ifndef HYPERWIN_HW_FILES_H
#define HYPERWIN_HW_FILES_H

#include <stdint.h>

/* Above the GPU rings (they end at 0x7809000) and below the debug marks at HW_FRAME_END. */
#define HWF_OFF        0x7810000u
#define HWF_MAGIC      0x53454c4946575948ull /* "HYWFILES" little endian */
#define HWF_VERSION    2u  /* 2: 24 slots of 256 KiB */
#define HWF_SLOTS      24u
#define HWF_META       0x1000u
#define HWF_DATA       (256u << 10)
#define HWF_STRIDE     (HWF_META + HWF_DATA)
#define HWF_BYTES      (0x1000u + HWF_SLOTS * HWF_STRIDE)
#define HWF_VOLUMES    8u
#define HWF_PATH       1900u  /* bytes of UTF-8, with the terminating zero */

_Static_assert(HWF_OFF + HWF_BYTES <= 0x7f00000u, "file slots end below the debug marks");

/* slot state */
#define HWF_FREE     0u
#define HWF_CLAIMED  1u
#define HWF_REQUEST  2u
#define HWF_BUSY     3u
#define HWF_DONE     4u

/* ops. Paths are from the volume's root, with '\' between names, like "\Games\x.pak". */
#define HWF_VOLINFO  1u   /* -> total, free in a, b */
#define HWF_STAT     2u   /* path -> info */
#define HWF_OPEN     3u   /* path -> handle, info */
#define HWF_CREATE   4u   /* path, flags HWF_DIR -> handle, info */
#define HWF_CLOSE    5u   /* handle */
#define HWF_READ     6u   /* handle, offset, length -> data, length */
#define HWF_WRITE    7u   /* handle, offset, length, data -> length, info */
#define HWF_RESIZE   8u   /* handle, offset = new size -> info */
#define HWF_FSTAT    9u   /* handle -> info */
#define HWF_SYNC     10u  /* handle */
#define HWF_LIST     11u  /* path, offset = first entry -> entries in data, length, more */
#define HWF_REMOVE   12u  /* path, flags HWF_DIR */
#define HWF_RENAME   13u  /* path, path2, flags HWF_REPLACE */
#define HWF_TIMES    14u  /* handle, a = access, b = write time (ns since 1970, 0 keeps it) -> info */

/* request flags */
#define HWF_DIR        1u
#define HWF_REPLACE    2u
#define HWF_OPEN_WRITE 4u  /* the Windows handle was opened for writing */

/* info.kind */
#define HWF_KIND_FILE 0u
#define HWF_KIND_DIR  1u

/* volume flags */
#define HWF_VOL_PRIMARY   1u
#define HWF_VOL_REMOVABLE 2u
#define HWF_VOL_READONLY  4u

struct hwf_info {
    uint32_t kind;
    uint32_t mode;      /* the phone's permission bits */
    uint64_t size;
    uint64_t blocks;    /* bytes the file takes on disk */
    uint64_t ino;
    int64_t atime, mtime, ctime;  /* ns since 1970 */
};

/* One entry of a HWF_LIST answer, then its UTF-8 name; the next starts 8-aligned. */
struct hwf_entry {
    struct hwf_info info;
    uint16_t name_len;
    uint16_t pad[3];
};

struct hwf_volume {
    uint32_t id;        /* 0 = unused, otherwise stable while the volume stays */
    uint32_t flags;
    char label[32];     /* UTF-8, what Windows shows */
};

/* The region's first page. */
struct hwf_hdr {
    uint64_t magic;
    uint32_t version;
    uint32_t slots;
    uint32_t data_bytes;
    uint32_t host_tick;   /* the phone bumps it while it serves */
    uint32_t volume_gen;  /* bumped after the phone changes the volume list */
    uint32_t reserved;
    struct hwf_volume volume[HWF_VOLUMES];
};

/* First page of each slot. */
struct hwf_slot {
    uint32_t state;
    uint32_t op;
    uint32_t volume;
    uint32_t flags;
    uint64_t handle;
    uint64_t offset;
    uint64_t length;
    int64_t a, b;
    /* answer */
    int32_t err;
    uint32_t more;
    uint64_t out_handle;
    uint64_t out_length;
    int64_t out_a, out_b;
    struct hwf_info info;
    char path[HWF_PATH];
    char path2[HWF_PATH];
};

_Static_assert(sizeof(struct hwf_hdr) <= 0x1000u, "header fits its page");
_Static_assert(sizeof(struct hwf_slot) <= HWF_META, "slot fields fit their page");

static inline struct hwf_hdr *hwf_hdr(void *region) { return (struct hwf_hdr *)region; }
static inline struct hwf_slot *hwf_slot(void *region, uint32_t i)
{
    return (struct hwf_slot *)((uint8_t *)region + 0x1000u + i * HWF_STRIDE);
}
static inline uint8_t *hwf_data(void *region, uint32_t i) { return (uint8_t *)hwf_slot(region, i) + HWF_META; }

static inline int hwf_valid(void *region)
{
    struct hwf_hdr *h = hwf_hdr(region);
    return __atomic_load_n(&h->magic, __ATOMIC_ACQUIRE) == HWF_MAGIC && h->version == HWF_VERSION &&
           h->slots == HWF_SLOTS && h->data_bytes == HWF_DATA;
}

#endif
