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
#define HWD3D_VERSION  1u

/* hwd3d_msg.flags */
#define HWD3D_NO_REPLY 0x0001u   /* the phone answers nothing, not even an error */
#define HWD3D_REPLY    0x8000u   /* set on every answer */

/* hwd3d_msg.op */
#define HWD3D_OP_HELLO 0x0001u   /* hwd3d_hello -> hwd3d_hello_reply */
#define HWD3D_OP_NOP   0x0002u   /* nothing; counted (transport tests) */
#define HWD3D_OP_PING  0x0003u   /* -> hwd3d_ping_reply: how many messages arrived */

#define HWD3D_MAX_MSG  (1u << 20)

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

struct hwd3d_reply {
    struct hwd3d_msg hdr;
    int32_t status;    /* an HRESULT */
    uint32_t reserved;
};

struct hwd3d_hello_reply {
    struct hwd3d_reply r;
    uint32_t version;  /* HWD3D_VERSION the phone speaks */
    uint32_t caps;     /* none yet */
};

struct hwd3d_ping_reply {
    struct hwd3d_reply r;
    uint64_t received; /* messages the phone has read on this channel, this ping included */
};
#pragma pack(pop)

#ifdef __cplusplus
#define HWD3D_ASSERT static_assert
#else
#define HWD3D_ASSERT _Static_assert
#endif
HWD3D_ASSERT(sizeof(struct hwd3d_msg) == 16, "hwd3d_msg");
HWD3D_ASSERT(sizeof(struct hwd3d_hello) == 32, "hwd3d_hello");
HWD3D_ASSERT(sizeof(struct hwd3d_reply) == 24, "hwd3d_reply");
HWD3D_ASSERT(sizeof(struct hwd3d_hello_reply) == 32, "hwd3d_hello_reply");
HWD3D_ASSERT(sizeof(struct hwd3d_ping_reply) == 32, "hwd3d_ping_reply");
/* The sgpu server reads 4 + 24 bytes before it knows which protocol a channel speaks. */
HWD3D_ASSERT(sizeof(struct hwd3d_hello) >= 28, "a hello covers the server's first read");

#endif
