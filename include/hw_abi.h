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
/* GPU memory Windows programs map directly (gpu/README.md): a second region shared with the phone,
 * right below the window. The firmware keeps it from Windows like the window. The phone's GPU
 * server hands it out; hwgpu.sys maps parts of it into the programs they belong to. */
#define HW_HEAP_SIZE (512ull * 1024ull * 1024ull)
#define HW_HEAP_PA (HW_WINDOW_PA - HW_HEAP_SIZE)
/* Default frame size. The host may set any size from HW_MIN_* to HW_MAX_* in the header. */
#define HW_WIDTH 1280u
#define HW_HEIGHT 720u
#define HW_MIN_WIDTH 640u
#define HW_MIN_HEIGHT 480u
#define HW_MAX_WIDTH 4096u
#define HW_MAX_HEIGHT 2304u
#define HW_FORMAT_BGRA 1u

#define HW_FRAME0 0x10000u
/* Two frames that do not fit between HW_FRAME0 and HW_AUDIO start here instead. */
#define HW_FRAME_BIG 0x1000000u
/* Guest debug marks (guest/efi/trace.c) sit at the top of the window; frames end below. */
#define HW_FRAME_END 0x7f00000u
#define HW_AUDIO  0x800000u
#define HW_AUDIO_BYTES (1024u * 1024u)

/* Guest listens. The app opens it with VirtualMachine.connectVsock. */
#define HW_VSOCK_PORT 5601
#define HW_VSOCK_PORT_FILES 5602
/* Guest listens; the app connects and sends framed HID input reports (length byte, report). */
#define HW_VSOCK_PORT_INPUT 5650
/* Guest agent (guest/agent/hwagent.c): the app connects to this port. Frames are
 * u32 payload length, u8 type, payload. */
#define HW_VSOCK_PORT_AGENT 5651
/* Touch screen and Precision Touchpad contacts for hwhid.sys (guest/drivers/hwhid/hwhid.c). */
#define HW_VSOCK_PORT_HID 5652
#define HW_AGENT_VERSION 5
#define HW_AGENT_PACKET 1       /* both ways: one IPv4 packet */
#define HW_AGENT_EXEC 2         /* app -> guest: u32 id, u8 mode, UTF-8 command line */
#define HW_AGENT_EXEC_OUT 3     /* guest -> app: u32 id, output bytes */
#define HW_AGENT_EXEC_DONE 4    /* guest -> app: u32 id, u32 exit code */
#define HW_AGENT_FILE_OPEN 5    /* app -> guest: UTF-8 path to create */
#define HW_AGENT_FILE_DATA 6    /* app -> guest: bytes */
#define HW_AGENT_FILE_CLOSE 7   /* app -> guest */
#define HW_AGENT_FILE_RESULT 8  /* guest -> app: u32 1 = written */
#define HW_AGENT_HELLO 9        /* guest -> app: u8 version, u8 network adapter present */
#define HW_AGENT_NET_CONFIG 10  /* app -> guest: ASCII "ip mask gateway dns mtu" */
#define HW_AGENT_PORTS 11       /* guest -> app: u16 n, n TCP listening ports, u16 m, m bound UDP ports */
/* app -> guest: u8 n, then n contacts now on the screen: u8 id, u16 x, u16 y (0..65535 of the
 * desktop). A contact missing from the next frame has lifted. */
#define HW_AGENT_TOUCH 12
#define HW_AGENT_TOUCH_MAX 10
/* app -> guest: u32 clock of the phone's processor in MHz, for Windows to show. Windows measures
 * the clock with the cycle counter, which this VM lacks, and would report the timer's 25 MHz. */
#define HW_AGENT_CPU_MHZ 13
/* guest -> app: u8 1 when the foreground program hides the cursor and holds it in (clipped, or a
 * fullscreen window): a game wants mouse-look, and the app captures the phone's mouse. */
#define HW_AGENT_CURSOR 14
#define HW_AGENT_EXEC_SYSTEM 0  /* run as SYSTEM, output captured */
#define HW_AGENT_EXEC_DESKTOP 1 /* start on the signed-in user's desktop */
#define HW_AGENT_EXEC_HIDDEN 2  /* the same, with no console window (helpers such as "hwagent.exe keyboard") */
#define HW_VSOCK_CID_HOST 2

/* File mailbox. The guest asks; the host reads its own files and writes the answer.
 * A disk image passed to AVF must live in the app's private directory. Host paths
 * such as /data/local/tmp are not disks. */
#define HW_FILES 0x900000u
#define HW_FILE_REQ 4096u
#define HW_FILE_RESP (32u * 1024u)
#define HW_FILE_STAT 1u
#define HW_FILE_READ 2u

/* Guest diagnostics, read by the host debug tap. [0] magic, [1] loader PCI SSDT result
 * (irqs << 8 | routes, or a negative stage), [2] hwpci stage, [3] hwpci children found. */
#define HW_DIAG 0xA00000u

/* Processor performance registers for the processor SSDT (guest/efi/cpu_acpi.c). Windows only
 * takes them through a PCC mailbox, which this page is: signature u32, command u16, status u16,
 * then the registers. The doorbell is the status word itself, so ringing it completes the
 * command at once and nobody has to answer. In the register space: the reference counter (u64,
 * the host advances it) at +0, its copy the delivered counter at +8, then per CPU n desired
 * performance at +16 + 16n and performance-limited flags at +24 + 16n. */
#define HW_CPPC 0xB00000u
#define HW_CPPC_REGS (HW_CPPC + 8u)
#define HW_CPPC_MAX_CPUS 64u

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
    /* What Windows shows as the computer and processor (SMBIOS). The host fills these in before
     * boot; an empty string keeps the firmware's own text. */
    char system_maker[32];
    char system_model[48];
    char cpu_maker[32];
    char cpu_name[64];
    uint32_t cpu_mhz;
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
    uint32_t bytes = hw_frame_bytes(h);
    return (2u * bytes > HW_AUDIO - HW_FRAME0 ? HW_FRAME_BIG : HW_FRAME0) + index * bytes;
}

/* True when the header describes a frame pair that fits the window. */
static inline int hw_mode_ok(const struct hw_header *h)
{
    return h->width >= HW_MIN_WIDTH && h->width <= HW_MAX_WIDTH &&
           h->height >= HW_MIN_HEIGHT && h->height <= HW_MAX_HEIGHT &&
           h->stride == h->width * 4u && h->format == HW_FORMAT_BGRA &&
           (uint64_t)hw_frame_off(h, 1) + hw_frame_bytes(h) <= HW_FRAME_END;
}

#endif
