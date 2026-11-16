/* Shared window between the Android (or QEMU) host and a Windows ARM64 guest.
 * AVF maps this file into guest physical memory. The desktop stays on Windows'
 * own software compositor; frames and audio cross here. Vulkan commands do not:
 * those go through the virtio-gpu device AVF's gfxstream backend already is.
 */
#ifndef HYPERWIN_HW_ABI_H
#define HYPERWIN_HW_ABI_H

#include <stdint.h>
#include <stddef.h>

#define HW_MAGIC "HYPRWIN1"
#define HW_VERSION 1u
#define HW_WINDOW_SIZE (128u * 1024u * 1024u)
/* AVF guest RAM starts at 0x80000000; the ring sits inside UEFI conventional RAM. */
#ifndef HW_WINDOW_PA
#define HW_WINDOW_PA 0x137000000ull
#endif
#define HW_WIDTH 1280u
#define HW_HEIGHT 720u
#define HW_FORMAT_BGRA 1u

#define HW_FRAME0 0x10000u
#define HW_AUDIO  0x800000u
#define HW_AUDIO_BYTES (1024u * 1024u)

/* Guest listens. The app opens it with VirtualMachine.connectVsock. */
#define HW_VSOCK_PORT 5601
#define HW_VSOCK_PORT_FILES 5602
#define HW_VSOCK_CID_HOST 2

/* File mailbox. The guest asks; the host reads its own files and writes the answer.
 * A disk image passed to AVF must live in the app's private directory. Host paths
 * such as /data/local/tmp are not disks. */
#define HW_FILES 0x900000u
#define HW_FILE_REQ 4096u
#define HW_FILE_RESP (32u * 1024u)
#define HW_FILE_STAT 1u
#define HW_FILE_READ 2u

#define HW_GUEST_FOUND 1u
#define HW_GUEST_FRAME 2u
#define HW_GUEST_TPM   4u
#define HW_GUEST_GPU   8u
#define HW_GUEST_AUDIO 16u
#define HW_GUEST_PAD   32u
#define HW_GUEST_GOP   64u
#define HW_GUEST_RING_RESERVED 128u

#define HW_VSOCK_OP_PAD 1u

struct hw_header {
    char magic[8];
    uint32_t version;
    uint32_t header_bytes;
    uint64_t window_size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t front;
    uint32_t seq;
    uint32_t enable;
    uint32_t guest_flags;
    uint64_t audio_head;
    uint64_t audio_tail;
    uint32_t audio_rate;
    uint32_t audio_channels;
    uint16_t pci_vendor;
    uint16_t pci_device;
    uint32_t pad_connected;
    uint32_t pad_packet;
    uint16_t buttons;
    uint8_t left_trigger;
    uint8_t right_trigger;
    int16_t left_x, left_y, right_x, right_y;
    uint32_t tpm_req_seq;
    uint32_t tpm_req_len;
    uint32_t tpm_resp_seq;
    uint32_t tpm_resp_len;
    uint32_t tpm_status;
    uint32_t tpm_rc;
    uint8_t tpm_req[1024];
    uint8_t tpm_resp[1024];
};

_Static_assert(sizeof(struct hw_header) < 0x1000, "header must fit in the first page");
_Static_assert(offsetof(struct hw_header, seq) == 44, "seq offset");
_Static_assert(offsetof(struct hw_header, tpm_req) == 128, "tpm request offset");

struct hw_file_mbox {
    uint32_t req_seq;
    uint32_t req_len;
    uint32_t resp_seq;
    uint32_t resp_len;
    int32_t status;
    uint32_t reserved;
    uint8_t req[HW_FILE_REQ];
    uint8_t resp[HW_FILE_RESP];
};

_Static_assert(HW_FILES + sizeof(struct hw_file_mbox) < HW_WINDOW_SIZE, "file mailbox fits");

/* One pad packet on the vsock, little endian, fixed size. */
struct hw_pad_msg {
    uint32_t magic; /* 'HWIN' */
    uint32_t op;
    uint32_t player;
    uint32_t connected;
    uint32_t packet;
    uint16_t buttons;
    uint8_t left_trigger;
    uint8_t right_trigger;
    int16_t left_x, left_y, right_x, right_y;
};

#define HW_PAD_MAGIC 0x4E495748u

static inline uint32_t hw_frame_bytes(const struct hw_header *h)
{
    return h->stride * h->height;
}

static inline uint32_t hw_frame_off(const struct hw_header *h, uint32_t index)
{
    return HW_FRAME0 + index * hw_frame_bytes(h);
}

#endif
