/* HwD3D's wire format (d3d/README.md): Direct3D calls from thin DLLs in Windows to DXVK on the
 * phone (gpu/host/hwd3d), over a GPU channel (gpu/include/hwgpu_ring.h).
 *
 * A channel speaks HwD3D when its first message is HWD3D_OP_HELLO; otherwise it is an sgpu
 * (Vulkan) channel. Every message starts with struct hwd3d_msg, whose second word is HWD3D_MAGIC:
 * an sgpu message has a command type there, which is never this value.
 *
 * Rules: fixed-width fields, little endian, no pointers, sizes a multiple of 4. Bump HWD3D_VERSION
 * on any change; a guest DLL that gets another version back falls back to Microsoft's Direct3D.
 */
#ifndef HYPERWIN_HW_D3D_H
#define HYPERWIN_HW_D3D_H

#include <stdint.h>

#define HWD3D_MAGIC    0x44334448u   /* "HD3D" little endian */
#define HWD3D_VERSION  6u   /* 6: the staging area is asked for (HWD3D_OP_STAGING), not given at hello */

/* hwd3d_msg.flags */
#define HWD3D_NO_REPLY 0x0001u   /* the phone answers nothing, not even an error */
#define HWD3D_REPLY    0x8000u   /* set on every answer */

/* hwd3d_msg.op */
#define HWD3D_OP_HELLO 0x0001u   /* hwd3d_hello -> hwd3d_hello_reply */
#define HWD3D_OP_NOP   0x0002u   /* nothing; counted (transport tests) */
#define HWD3D_OP_PING  0x0003u   /* -> hwd3d_ping_reply: how many messages arrived */
#define HWD3D_OP_PROGRAM 0x0004u /* hwd3d_program: which program this channel draws for; no answer */
/* The rest are a struct hwd3d_call and its arguments; the answer, a struct hwd3d_reply and its
 * outputs. Arguments and outputs are packed as gpu/host/hwd3d/gen_thunks.py describes: each one
 * 4-byte aligned, an array as a u32 "present", a u32 count and the elements. Present 2 means the
 * elements are in the staging area instead: a u64 position follows the count (HWD3D_STAGED). */
#define HWD3D_OP_CALL           0x0010u   /* object.method(arguments) */
#define HWD3D_OP_RELEASE        0x0011u   /* the guest let go of object; no answer */
#define HWD3D_OP_CREATE_FACTORY 0x0012u   /* CreateDXGIFactory1: riid -> handle, interface */
#define HWD3D_OP_CREATE_DEVICE  0x0013u   /* D3D11CreateDevice -> device, level, context */
#define HWD3D_OP_STAGING        0x0014u   /* hwd3d_msg only -> hwd3d_hello_reply with the staging area, when the guest first needs one */
/* Surfaces shared between programs (DWM and the screen driver): an Android hardware buffer under each
 * program's texture. object = the device; the answer's status is an HRESULT. */
#define HWD3D_OP_CREATE_SHARED  0x0015u   /* D3D11_TEXTURE2D_DESC (44 bytes) -> texture handle, share id */
#define HWD3D_OP_OPEN_SHARED    0x0016u   /* share id -> texture handle, D3D11_TEXTURE2D_DESC */
#define HWD3D_OP_SCANOUT        0x0017u   /* share id: put that surface on the phone's screen; no answer */
#define HWD3D_OP_FINISH         0x0018u   /* -> status once this channel's GPU work is done (before a shared surface is shown) */
/* The screen driver's way to hand a frame over through the ordinary Direct3D 11 API: UpdateSubresource of a
 * shared surface with a 1x1 box at 0,0, row pitch HWD3D_SCANOUT_PITCH and depth pitch HWD3D_SCANOUT_SLICE.
 * The driver sends HWD3D_OP_SCANOUT instead of writing the pixel. (SetResourceMinLOD never reached the
 * driver for these surfaces.) */
#define HWD3D_SCANOUT_PITCH     0x5ca10001u
#define HWD3D_SCANOUT_SLICE     0x5ca10002u

/* hwd3d_msg arrays: inline, or in the staging area. */
#define HWD3D_INLINE 1u
#define HWD3D_STAGED 2u

/* The staging area: part of the shared GPU heap the phone grants each HwD3D channel at hello, which
 * Windows maps (IOCTL_HWGPU_MAP). Bulk data (uploads, written maps) goes there; the ring carries
 * only its position. It is used as a ring: positions only grow, the data of position p is at
 * HWD3D_STAGING_DATA + p % cap, with cap = size - HWD3D_STAGING_DATA - HWD3D_QUERY_PAGE, and one
 * array never wraps. The phone
 * publishes in done how far it has read, so the guest knows what it may write over. */
#define HWD3D_STAGING_DATA 64u
struct hwd3d_staging {
    uint64_t done;     /* phone: every staged byte before this position has been used */
    uint64_t reserved[7];
};

/* The query page: the last HWD3D_QUERY_PAGE bytes of the staging area, outside its ring. Each query
 * the guest ends gets a slot; the phone writes the result there when the GPU has it and then sets
 * seq to the End it answers, so GetData is a memory read. */
#define HWD3D_QUERY_PAGE  (64u << 10)
#define HWD3D_QUERY_DATA  120u
struct hwd3d_query_slot {
    uint32_t seq;      /* phone, written last: the End this result is for */
    uint32_t size;
    uint8_t data[HWD3D_QUERY_DATA];
};
#define HWD3D_QUERY_SLOTS (HWD3D_QUERY_PAGE / sizeof(struct hwd3d_query_slot))

/* A message can hold a texture's first contents. */
#define HWD3D_MAX_MSG  (256u << 20)

#pragma pack(push, 4)
struct hwd3d_msg {
    uint32_t size;     /* the whole message, this header included */
    uint32_t magic;    /* HWD3D_MAGIC */
    uint16_t op;
    uint16_t flags;
    uint32_t seq;      /* the guest numbers its requests; an answer carries the request's */
};

struct hwd3d_hello {
    struct hwd3d_msg hdr;
    uint32_t version;  /* HWD3D_VERSION the guest speaks */
    uint32_t pid;      /* for the phone's log */
    uint32_t arch;     /* 1 ARM64, 2 x64, 3 i686: which DLL sent it */
    uint32_t reserved;
};

/* After the hello, before any Create: the program's path (UTF-8) and the text of the dxvk.conf beside it,
 * if there is one, so DXVK picks its per-game settings by the program, not by the renderer's name. Each
 * is padded to 4 bytes. */
struct hwd3d_program {
    struct hwd3d_msg hdr;
    uint32_t path_bytes;
    uint32_t conf_bytes;
};

struct hwd3d_call {
    struct hwd3d_msg hdr;
    uint32_t object;   /* a handle the phone gave out; 0 for none */
    uint32_t method;   /* gen_thunks.py's number for the method, by the interface that declares it */
};

struct hwd3d_reply {
    struct hwd3d_msg hdr;
    int32_t status;    /* an HRESULT */
    uint32_t reserved;
};

struct hwd3d_hello_reply {
    struct hwd3d_reply r;
    uint32_t version;  /* HWD3D_VERSION the phone speaks */
    uint32_t caps;     /* none yet */
    uint64_t staging_offset, staging_size;  /* in the heap; size 0 for none */
};

struct hwd3d_ping_reply {
    struct hwd3d_reply r;
    uint64_t received; /* messages the phone has read on this channel, this ping included */
};
#pragma pack(pop)

/* Bytes in one block of a DXGI_FORMAT, and the block's size in texels; 0 for formats not done. */
static inline uint32_t hwd3d_fmt_block(uint32_t f, uint32_t *bw, uint32_t *bh)
{
    *bw = *bh = 1;
    if (f >= 1 && f <= 4) return 16;
    if (f >= 5 && f <= 8) return 12;
    if (f >= 9 && f <= 22) return 8;
    if (f >= 23 && f <= 47) return 4;
    if (f >= 48 && f <= 59) return 2;
    if (f >= 60 && f <= 66) return 1;
    if (f == 67) return 4;
    if (f == 68 || f == 69) { *bw = 2; return 4; }
    if ((f >= 70 && f <= 84) || (f >= 94 && f <= 99)) {
        *bw = *bh = 4;
        return (f <= 72 || (f >= 79 && f <= 81)) ? 8 : 16;
    }
    if (f == 85 || f == 86 || f == 115) return 2;
    if (f >= 87 && f <= 93) return 4;
    return 0;
}

/* A resource as both sides size it: D3D11_RESOURCE_DIMENSION, format, size, mips (0: all). */
struct hwd3d_res {
    uint32_t dim, format, width, height, depth, mips;
};

/* The bytes one subresource, or a box of it (left, top, front, right, bottom, back), takes
 * packed tightly: row bytes, rows of blocks, slices. 0 when it cannot be told. Buffers have
 * dim 1 and their byte width as width. */
static inline int hwd3d_region(const struct hwd3d_res *r, uint32_t sub, const uint32_t *box, uint32_t *row,
                               uint32_t *rows, uint32_t *slices)
{
    if (r->dim == 1) {
        *row = box ? box[3] - box[0] : r->width;
        *rows = *slices = 1;
        return !box || (box[3] > box[0] && box[3] <= r->width);
    }
    uint32_t m = r->mips;
    if (!m) {
        uint32_t big = r->width > r->height ? r->width : r->height;
        if (r->depth > big) big = r->depth;
        for (m = 1; big > 1; big >>= 1) m++;
    }
    uint32_t bw, bh, bytes = hwd3d_fmt_block(r->format, &bw, &bh), mip = sub % m;
    uint32_t w = r->width >> mip, h = r->height >> mip, d = r->dim == 4 ? r->depth >> mip : 1;
    if (!bytes) return 0;
    if (!w) w = 1;
    if (!h) h = 1;
    if (!d) d = 1;
    if (box) {
        if (box[3] <= box[0] || box[4] <= box[1] || box[5] <= box[2] || box[3] > w || box[4] > h || box[5] > d) return 0;
        w = box[3] - box[0];
        h = box[4] - box[1];
        d = box[5] - box[2];
    }
    *row = (w + bw - 1) / bw * bytes;
    *rows = (h + bh - 1) / bh;
    *slices = d;
    return 1;
}

#ifdef __cplusplus
#define HWD3D_ASSERT static_assert
#else
#define HWD3D_ASSERT _Static_assert
#endif
HWD3D_ASSERT(sizeof(struct hwd3d_msg) == 16, "hwd3d_msg");
HWD3D_ASSERT(sizeof(struct hwd3d_hello) == 32, "hwd3d_hello");
HWD3D_ASSERT(sizeof(struct hwd3d_reply) == 24, "hwd3d_reply");
HWD3D_ASSERT(sizeof(struct hwd3d_call) == 24, "hwd3d_call");
HWD3D_ASSERT(sizeof(struct hwd3d_program) == 24, "hwd3d_program");
HWD3D_ASSERT(sizeof(struct hwd3d_hello_reply) == 48, "hwd3d_hello_reply");
HWD3D_ASSERT(sizeof(struct hwd3d_staging) == HWD3D_STAGING_DATA, "hwd3d_staging");
HWD3D_ASSERT(sizeof(struct hwd3d_query_slot) == 128, "hwd3d_query_slot");
HWD3D_ASSERT(sizeof(struct hwd3d_ping_reply) == 32, "hwd3d_ping_reply");
/* The sgpu server reads 4 + 24 bytes before it knows which protocol a channel speaks. */
HWD3D_ASSERT(sizeof(struct hwd3d_hello) >= 28, "a hello covers the server's first read");

#endif
