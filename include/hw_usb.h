/* USB passthrough over shared RAM (usb/README.md): the ABI between the phone's app (host) and
 * hwusb.sys in Windows (guest). One 64 MiB region right below the GPU heap, with a global
 * page and 8 device slots. Descriptors carry USB/IP's fields; their data sits in a per-device arena
 * instead of after the header. No byte of this is a Windows or WDK type: plain C, shared by both sides.
 *
 * Who writes what: the guest owns the submit ring's head and the completion ring's tail, the host
 * owns the submit ring's tail and the completion ring's head. Entries are written first, then the index
 * moves with a release store; the reader takes it with an acquire load.
 */
#ifndef HYPERWIN_HW_USB_H
#define HYPERWIN_HW_USB_H

#include <stdint.h>
#include <string.h>
#include "hw_abi.h"

#define HW_USB_SIZE (64ull * 1024ull * 1024ull)
#define HW_USB_PA (HW_HEAP_PA - HW_USB_SIZE)
#define HW_USB_SLOTS 8u
#define HW_USB_MAGIC "HWUSBRNG"
#define HW_USB_VERSION 1u
#define HW_USB_VSOCK_PORT 5604u   /* wake-up kicks only, never data */

/* Global area: 64 KiB. Slots follow, each the same size. */
#define HW_USB_GLOBAL_SIZE 0x10000u
#define HW_USB_SLOT_STRIDE ((uint32_t)((HW_USB_SIZE - HW_USB_GLOBAL_SIZE) / HW_USB_SLOTS))
#define HW_USB_RING_ENTRIES 256u
#define HW_USB_ENTRY_SIZE 128u
#define HW_USB_CTRL_OFF 0u
#define HW_USB_CTRL_SIZE 0x1000u
#define HW_USB_SUB_OFF (HW_USB_CTRL_OFF + HW_USB_CTRL_SIZE)
#define HW_USB_SUB_SIZE (HW_USB_RING_ENTRIES * HW_USB_ENTRY_SIZE)
#define HW_USB_CMP_OFF (HW_USB_SUB_OFF + HW_USB_SUB_SIZE)
#define HW_USB_CMP_SIZE (HW_USB_RING_ENTRIES * HW_USB_ENTRY_SIZE)
#define HW_USB_ARENA_OFF (HW_USB_CMP_OFF + HW_USB_CMP_SIZE)
#define HW_USB_ARENA_SIZE (HW_USB_SLOT_STRIDE - HW_USB_ARENA_OFF)
#define HW_USB_PAGE 4096u

/* Speeds, as in USB/IP and usbfs. */
enum hw_usb_speed { HW_USB_LOW = 1, HW_USB_FULL = 2, HW_USB_HIGH = 3, HW_USB_SUPER = 5, HW_USB_SUPER_PLUS = 6 };
/* Slot states in the device table. The host moves free -> offered -> (guest) attached -> detaching -> free. */
enum hw_usb_state { HW_USB_FREE = 0, HW_USB_OFFERED = 1, HW_USB_ATTACHED = 2, HW_USB_DETACHING = 3 };
/* Descriptor types. CTRL..ISO are USB transfers. */
enum hw_usb_op {
    HW_USB_OP_CTRL = 1, HW_USB_OP_BULK = 2, HW_USB_OP_INT = 3, HW_USB_OP_ISO = 4,
    HW_USB_OP_UNLINK = 5,          /* cancel the request with seqnum = unlink_seq */
    HW_USB_OP_SET_INTERFACE = 6,   /* ep = interface, value = alternate setting */
    HW_USB_OP_SET_CONFIG = 7,      /* value = configuration */
    HW_USB_OP_CLEAR_HALT = 8,      /* ep = endpoint address */
    HW_USB_OP_RESET = 9,
};
#define HW_USB_DIR_OUT 0u
#define HW_USB_DIR_IN 1u

#define HW_USB_DESC_MAX 4096u

struct hw_usb_dev_entry {          /* 4160 bytes */
    uint32_t state;                /* enum hw_usb_state: the host offers, the guest attaches */
    uint32_t generation;           /* grows with every offer, so a quick replug is a new device */
    uint32_t bus_id;               /* the phone's number for it, for the settings screen */
    uint16_t vendor, product;
    uint32_t speed;                /* enum hw_usb_speed */
    uint32_t desc_len;             /* bytes of desc: device descriptor, then every configuration */
    uint32_t guest_port;           /* set by the guest when it plugs the device in */
    uint32_t reserved[9];
    uint8_t desc[HW_USB_DESC_MAX];
};

struct hw_usb_global {             /* start of the global area */
    char magic[8];
    uint32_t version;
    uint32_t slots;
    volatile uint32_t host_state;  /* 1 = host running */
    volatile uint32_t guest_state; /* 1 = hwusb.sys running */
    volatile uint32_t table_gen;   /* grows with every change of the table */
    uint32_t slot_stride;
    uint32_t reserved[8];
    struct hw_usb_dev_entry dev[HW_USB_SLOTS];
};

struct hw_usb_ctrl {               /* control page of one slot; each counter on its own cache line */
    volatile uint32_t sub_head;    uint32_t pad0[15];   /* guest */
    volatile uint32_t sub_tail;    uint32_t pad1[15];   /* host */
    volatile uint32_t cmp_head;    uint32_t pad2[15];   /* host */
    volatile uint32_t cmp_tail;    uint32_t pad3[15];   /* guest */
    volatile uint32_t guest_sleep; /* 1: the guest is idle and waits for a kick */
    volatile uint32_t host_sleep;
    volatile uint32_t slot_generation;   /* the device generation this slot's rings belong to */
    uint32_t pad4[13];
    volatile uint64_t submits, completions, bytes_out, bytes_in;
};

/* USB/IP's CMD_SUBMIT, CMD_UNLINK as one 128-byte entry. Data is in the arena. */
struct hw_usb_sub {
    uint32_t seqnum;
    uint8_t op;                    /* enum hw_usb_op */
    uint8_t dir;                   /* HW_USB_DIR_* */
    uint8_t ep;                    /* endpoint number (without direction), interface for SET_INTERFACE */
    uint8_t reserved0;
    uint32_t transfer_flags;       /* USB/IP: URB_SHORT_NOT_OK, URB_ISO_ASAP, URB_ZERO_PACKET ... */
    uint32_t length;               /* buffer length */
    uint32_t arena_off;            /* data: offset into the arena, 4 KiB aligned; 0 with length 0 */
    uint32_t start_frame;
    uint32_t nr_packets;           /* isochronous: packet descriptors at iso_off in the arena */
    uint32_t interval;
    uint8_t setup[8];
    uint32_t unlink_seq;           /* for UNLINK */
    uint32_t value;                /* SET_INTERFACE alternate, SET_CONFIG number */
    uint32_t iso_off;              /* arena offset of nr_packets struct hw_usb_iso */
    uint32_t stream_id;
    uint8_t reserved1[72];
};

struct hw_usb_iso {                /* USB/IP's iso packet descriptor */
    uint32_t offset, length, actual_length, status;
};

/* USB/IP's RET_SUBMIT and RET_UNLINK. */
struct hw_usb_cmp {
    uint32_t seqnum;
    uint8_t op;                    /* of the request this completes */
    uint8_t reserved0[3];
    int32_t status;                /* 0 or a negative errno: -EPIPE stall, -ENODEV gone, -ECONNRESET unlinked, -EOVERFLOW... */
    uint32_t actual_length;
    uint32_t start_frame;
    uint32_t nr_packets;
    uint32_t error_count;
    uint8_t reserved1[100];
};

#define HW_USB_SLOT_OFF(i) (HW_USB_GLOBAL_SIZE + (uint64_t)(i) * HW_USB_SLOT_STRIDE)

/* Layout checks: both sides compile this and must agree. */
_Static_assert(sizeof(struct hw_usb_dev_entry) == 4096u + 64u, "device table entry");
_Static_assert(sizeof(struct hw_usb_global) <= HW_USB_GLOBAL_SIZE, "global area");
_Static_assert(sizeof(struct hw_usb_ctrl) <= HW_USB_CTRL_SIZE, "control page");
_Static_assert(sizeof(struct hw_usb_sub) == HW_USB_ENTRY_SIZE, "submit entry");
_Static_assert(sizeof(struct hw_usb_cmp) == HW_USB_ENTRY_SIZE, "completion entry");
_Static_assert(sizeof(struct hw_usb_iso) == 16, "iso packet");
_Static_assert(HW_USB_ARENA_OFF % HW_USB_PAGE == 0, "arena is page aligned");
_Static_assert(HW_USB_SLOT_STRIDE % HW_USB_PAGE == 0, "slot is page aligned");
_Static_assert(HW_USB_GLOBAL_SIZE + HW_USB_SLOTS * (uint64_t)HW_USB_SLOT_STRIDE <= HW_USB_SIZE, "region size");
_Static_assert(HW_USB_ARENA_SIZE > 7u * 1024u * 1024u, "arena about 7.9 MiB");
_Static_assert(HW_USB_PA + HW_USB_SIZE == HW_HEAP_PA, "region sits right below the heap");

/* ---- rings (the same code runs on both sides) ---- */

struct hw_usb_slot {               /* pointers into a mapped region for one slot */
    struct hw_usb_ctrl *ctrl;
    struct hw_usb_sub *sub;
    struct hw_usb_cmp *cmp;
    uint8_t *arena;
};

static inline struct hw_usb_slot hw_usb_slot_at(void *region, unsigned i)
{
    uint8_t *b = (uint8_t *)region + HW_USB_SLOT_OFF(i);
    struct hw_usb_slot s = {(struct hw_usb_ctrl *)(b + HW_USB_CTRL_OFF), (struct hw_usb_sub *)(b + HW_USB_SUB_OFF),
                            (struct hw_usb_cmp *)(b + HW_USB_CMP_OFF), b + HW_USB_ARENA_OFF};
    return s;
}

#define HW_USB_LOAD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define HW_USB_STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)

/* Guest: put a request on the submit ring. 0, or -1 when the ring is full. */
static inline int hw_usb_submit_push(struct hw_usb_slot *s, const struct hw_usb_sub *e)
{
    uint32_t head = s->ctrl->sub_head, tail = HW_USB_LOAD(&s->ctrl->sub_tail);
    if (head - tail >= HW_USB_RING_ENTRIES) return -1;
    s->sub[head % HW_USB_RING_ENTRIES] = *e;
    HW_USB_STORE(&s->ctrl->sub_head, head + 1);
    return 0;
}
/* Host: take the next request. 0 and the entry, or -1 when empty. */
static inline int hw_usb_submit_pop(struct hw_usb_slot *s, struct hw_usb_sub *out)
{
    uint32_t tail = s->ctrl->sub_tail, head = HW_USB_LOAD(&s->ctrl->sub_head);
    if (head == tail) return -1;
    *out = s->sub[tail % HW_USB_RING_ENTRIES];
    HW_USB_STORE(&s->ctrl->sub_tail, tail + 1);
    return 0;
}
/* Host: report a finished request. -1 when the completion ring is full. */
static inline int hw_usb_cmp_push(struct hw_usb_slot *s, const struct hw_usb_cmp *e)
{
    uint32_t head = s->ctrl->cmp_head, tail = HW_USB_LOAD(&s->ctrl->cmp_tail);
    if (head - tail >= HW_USB_RING_ENTRIES) return -1;
    s->cmp[head % HW_USB_RING_ENTRIES] = *e;
    HW_USB_STORE(&s->ctrl->cmp_head, head + 1);
    return 0;
}
/* Guest: take the next completion. */
static inline int hw_usb_cmp_pop(struct hw_usb_slot *s, struct hw_usb_cmp *out)
{
    uint32_t tail = s->ctrl->cmp_tail, head = HW_USB_LOAD(&s->ctrl->cmp_head);
    if (head == tail) return -1;
    *out = s->cmp[tail % HW_USB_RING_ENTRIES];
    HW_USB_STORE(&s->ctrl->cmp_tail, tail + 1);
    return 0;
}

#endif
