/*
 * sgpu: virtio-gpu commands carried over a byte stream (vsock) instead of virtqueues.
 *
 * The guest has no virtio-gpu device we can use, so it sends the same commands over a vsock
 * connection and the host app answers them with gfxstream's stream_renderer_* API. The guest
 * listens on SGPU_PORT and the host connects, like the display and input services.
 *
 * Every message in both directions is a little-endian u32 byte count (the rest of the message,
 * not counting itself) followed by a struct that starts with virtio_gpu_ctrl_hdr. Requests are the
 * virtio-gpu control commands with the layouts from the virtio spec. Each request gets exactly one
 * reply, in order. Fence completions arrive as SGPU_EVT_FENCE messages, possibly between replies.
 *
 * Without shared memory a few commands carry data inline:
 *   RESOURCE_ATTACH_BACKING  the entries' addresses are ignored. The host allocates
 *                            sum(length) bytes of its own backing for the resource.
 *   TRANSFER_TO_HOST_3D      followed by u32 len and len bytes. The host copies them into the
 *                            backing at the transfer offset, then runs the transfer.
 *   TRANSFER_FROM_HOST_3D    followed by u32 len. The host runs the transfer and replies
 *                            SGPU_RESP_OK_DATA with len bytes of backing from the offset.
 *                            A pipe with nothing to read answers SGPU_RESP_AGAIN after about
 *                            1 ms instead of blocking, so one guest thread waiting on the host
 *                            cannot stall the others. The guest asks again.
 *   RESOURCE_CREATE_BLOB     nr_entries must be 0. A GUEST blob gets host backing of size bytes.
 *   RESOURCE_MAP_BLOB        maps on the host only. Read and write it with SGPU_BLOB_READ/WRITE.
 *
 * A guest that keeps its own copy of mapped blobs (the Mesa gfxstream driver does) keeps it in
 * step with SGPU_CMD_SYNC: the request carries the guest's changed ranges, which the host writes
 * into its mappings. With SGPU_SYNC_PULL the reply carries the ranges the host changed since the
 * last sync. Each side compares against what the last sync left, in SGPU_SYNC_PAGE pages.
 *
 * SGPU_CMD_SURFACE names a range of a mapped blob as the pixels of a guest IOSurface (a Metal
 * drawable that the guest copied there on the GPU). The pixels stay on the host: neither side
 * syncs the whole pages of that range any more, and the host's GL compositor reads them when the
 * guest's window server draws that surface (GuestGL.kt, op HOST_SURFACE). width 0 ends it.
 *
 * SGPU_CMD_SCANOUT shows a frame without it ever leaving the phone's GPU. The guest renders into
 * a RESOURCE_CREATE_3D resource (a gfxstream ColorBuffer, which on the phone is an
 * AHardwareBuffer), waits until that rendering has finished, and names the resource. The phone
 * hands the buffer to its compositor on a layer above the CPU-drawn desktop. Each scanout buffer
 * has an index and a count of how often it was shown; the phone writes that count into
 * hwgpu_ctl.scanout_free[index] once the screen lets go of the buffer, and only then may the
 * guest draw into it again. Resource 0 hides the layer. A phone that cannot do this (its Vulkan
 * driver has no AHardwareBuffer export) leaves SGPU_HELLO_SCANOUT out of the hello reply and
 * refuses the command; software-drawn frames use SGPU_CMD_PRESENT instead.
 */
#ifndef SGPU_PROTO_H
#define SGPU_PROTO_H

#include <stdint.h>

#define SGPU_PORT    5526u
#define SGPU_VERSION 1u

/* virtio-gpu command and response types used here (virtio spec 5.7.6). */
#define VGPU_CMD_RESOURCE_UNREF           0x0102u
#define VGPU_CMD_RESOURCE_ATTACH_BACKING  0x0106u
#define VGPU_CMD_RESOURCE_DETACH_BACKING  0x0107u
#define VGPU_CMD_GET_CAPSET_INFO          0x0108u
#define VGPU_CMD_GET_CAPSET               0x0109u
#define VGPU_CMD_RESOURCE_CREATE_BLOB     0x010cu
#define VGPU_CMD_CTX_CREATE               0x0200u
#define VGPU_CMD_CTX_DESTROY              0x0201u
#define VGPU_CMD_CTX_ATTACH_RESOURCE      0x0202u
#define VGPU_CMD_CTX_DETACH_RESOURCE      0x0203u
#define VGPU_CMD_RESOURCE_CREATE_3D       0x0204u
#define VGPU_CMD_TRANSFER_TO_HOST_3D      0x0205u
#define VGPU_CMD_TRANSFER_FROM_HOST_3D    0x0206u
#define VGPU_CMD_SUBMIT_3D                0x0207u
#define VGPU_CMD_RESOURCE_MAP_BLOB        0x0208u
#define VGPU_CMD_RESOURCE_UNMAP_BLOB      0x0209u

#define VGPU_RESP_OK_NODATA               0x1100u
#define VGPU_RESP_OK_CAPSET_INFO          0x1102u
#define VGPU_RESP_OK_CAPSET               0x1103u
#define VGPU_RESP_OK_MAP_INFO             0x1106u
#define VGPU_RESP_ERR_UNSPEC              0x1200u
#define VGPU_RESP_ERR_OUT_OF_MEMORY       0x1201u
#define VGPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203u
#define VGPU_RESP_ERR_INVALID_CONTEXT_ID  0x1204u
#define VGPU_RESP_ERR_INVALID_PARAMETER   0x1205u

#define VGPU_FLAG_FENCE        (1u << 0)
#define VGPU_FLAG_INFO_RING_IDX (1u << 1)

#define VGPU_CAPSET_GFXSTREAM_VULKAN   3u
#define VGPU_CAPSET_GFXSTREAM_GLES     8u
#define VGPU_CAPSET_GFXSTREAM_COMPOSER 9u
#define VGPU_CONTEXT_INIT_CAPSET_ID_MASK 0xffu

/* sgpu additions, outside the ranges virtio-gpu uses. */
#define SGPU_CMD_HELLO       0x1000u  /* sgpu_hello -> sgpu_hello_resp */
#define SGPU_CMD_BLOB_WRITE  0x1001u  /* sgpu_blob_io + len bytes -> OK_NODATA */
#define SGPU_CMD_BLOB_READ   0x1002u  /* sgpu_blob_io -> SGPU_RESP_OK_DATA */
#define SGPU_CMD_SYNC        0x1003u  /* sgpu_sync_entry list -> SGPU_RESP_SYNC */
#define SGPU_CMD_SURFACE     0x1004u  /* sgpu_surface -> OK_NODATA */
#define SGPU_CMD_PRESENT     0x1005u  /* sgpu_present -> OK_NODATA (frame to shared window) */
#define SGPU_CMD_SCANOUT     0x1006u  /* sgpu_scanout -> OK_NODATA (GPU frame straight to the screen) */
#define SGPU_RESP_OK_HELLO   0x1180u
#define SGPU_HELLO_SCANOUT   (1u << 0) /* sgpu_hello_resp.hdr.flags: SGPU_CMD_SCANOUT works */
#define SGPU_RESP_OK_DATA    0x1181u  /* vgpu_hdr, u32 len, len bytes */
#define SGPU_RESP_SYNC       0x1182u  /* vgpu_hdr, sgpu_sync_entry list */
#define SGPU_RESP_AGAIN      0x1183u  /* TRANSFER_FROM_HOST on a pipe: nothing to read yet */
#define SGPU_RESP_OK_MAP_HEAP 0x1184u /* MAP_BLOB: sgpu_resp_map_heap, the blob lives in the shared heap */
#define SGPU_EVT_FENCE       0x1190u  /* vgpu_hdr with fence_id, ctx_id, ring_idx */
#define SGPU_SYNC_PULL       (1u << 8) /* SGPU_CMD_SYNC flag: reply with the host's changes */
#define SGPU_SYNC_MORE       (1u << 9) /* SGPU_RESP_SYNC flag: more host changes, pull again */
#define SGPU_FLAG_NO_REPLY   (1u << 12) /* request flag: answer nothing, not even an error (pipe writes) */
#define SGPU_SYNC_CHUNK      (16u << 20) /* most change bytes either side puts in one message */
#define SGPU_SYNC_PAGE       4096u

#pragma pack(push, 1)
struct vgpu_hdr {
	uint32_t type;
	uint32_t flags;
	uint64_t fence_id;
	uint32_t ctx_id;
	uint8_t ring_idx;
	uint8_t padding[3];
};

struct vgpu_resource_unref {          /* also CTX_ATTACH/DETACH_RESOURCE, DETACH_BACKING, UNMAP_BLOB */
	struct vgpu_hdr hdr;
	uint32_t resource_id;
	uint32_t padding;
};

struct vgpu_mem_entry {
	uint64_t addr;
	uint32_t length;
	uint32_t padding;
};

struct vgpu_resource_attach_backing {  /* followed by nr_entries vgpu_mem_entry */
	struct vgpu_hdr hdr;
	uint32_t resource_id;
	uint32_t nr_entries;
};

struct vgpu_get_capset_info {
	struct vgpu_hdr hdr;
	uint32_t capset_index;
	uint32_t padding;
};

struct vgpu_resp_capset_info {
	struct vgpu_hdr hdr;
	uint32_t capset_id;
	uint32_t capset_max_version;
	uint32_t capset_max_size;
	uint32_t padding;
};

struct vgpu_get_capset {
	struct vgpu_hdr hdr;
	uint32_t capset_id;
	uint32_t capset_version;
};

struct vgpu_resource_create_blob {
	struct vgpu_hdr hdr;
	uint32_t resource_id;
	uint32_t blob_mem;
	uint32_t blob_flags;
	uint32_t nr_entries;
	uint64_t blob_id;
	uint64_t size;
};

struct vgpu_ctx_create {
	struct vgpu_hdr hdr;
	uint32_t nlen;
	uint32_t context_init;
	char debug_name[64];
};

struct vgpu_resource_create_3d {
	struct vgpu_hdr hdr;
	uint32_t resource_id;
	uint32_t target;
	uint32_t format;
	uint32_t bind;
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t array_size;
	uint32_t last_level;
	uint32_t nr_samples;
	uint32_t flags;
	uint32_t padding;
};

struct vgpu_box {
	uint32_t x, y, z, w, h, d;
};

struct vgpu_transfer_3d {             /* followed by u32 len (and len bytes when to host) */
	struct vgpu_hdr hdr;
	struct vgpu_box box;
	uint64_t offset;
	uint32_t resource_id;
	uint32_t level;
	uint32_t stride;
	uint32_t layer_stride;
};

struct vgpu_submit_3d {               /* followed by size bytes of commands */
	struct vgpu_hdr hdr;
	uint32_t size;
	uint32_t num_in_fences;
};

struct vgpu_map_blob {
	struct vgpu_hdr hdr;
	uint32_t resource_id;
	uint32_t padding;
	uint64_t offset;
};

struct vgpu_resp_map_info {
	struct vgpu_hdr hdr;
	uint32_t map_info;
	uint32_t padding;
};

/* The blob's memory is the heap range [offset, offset + size): map it (IOCTL_HWGPU_MAP) instead of
 * keeping a copy in step. */
struct sgpu_resp_map_heap {
	struct vgpu_hdr hdr;
	uint32_t map_info;
	uint32_t padding;
	uint64_t offset;
	uint64_t size;
};

struct sgpu_hello {
	struct vgpu_hdr hdr;
	uint32_t version;
	uint32_t padding;
};

struct sgpu_hello_resp {
	struct vgpu_hdr hdr;
	uint32_t version;
	uint32_t num_capsets;
};

struct sgpu_blob_io {                  /* followed by len bytes for BLOB_WRITE */
	struct vgpu_hdr hdr;
	uint32_t resource_id;
	uint32_t len;
	uint64_t offset;
};

struct sgpu_resp_data {                /* followed by len bytes */
	struct vgpu_hdr hdr;
	uint32_t len;
};

struct sgpu_sync_entry {               /* followed by len bytes at offset in the blob */
	uint32_t resource_id;
	uint32_t len;
	uint64_t offset;
};

struct sgpu_surface {                  /* rows of `stride` bytes, top row first */
	struct vgpu_hdr hdr;
	uint32_t surface_id;               /* IOSurfaceGetID */
	uint32_t resource_id;              /* a mapped blob */
	uint64_t offset;
	uint32_t width;                    /* 0: the range is ordinary memory again */
	uint32_t height;
	uint32_t stride;
	uint32_t format;                   /* SGPU_SURFACE_BGRA8 */
};
#define SGPU_SURFACE_BGRA8 1u

struct sgpu_present {                  /* the host copies the resource into the frame buffer */
	struct vgpu_hdr hdr;
	uint32_t resource_id;              /* a mapped blob or backed resource with the pixels */
	uint32_t width;
	uint32_t height;
	uint32_t stride;                   /* bytes per row in the resource */
	uint32_t format;                   /* SGPU_PRESENT_BGRA8 */
};
#define SGPU_PRESENT_BGRA8 1u

struct sgpu_scanout {                  /* the phone's compositor shows the resource as it is */
	struct vgpu_hdr hdr;
	uint32_t resource_id;              /* a finished ColorBuffer resource, 0 to hide the layer */
	uint32_t index;                    /* the guest's buffer number, < HWGPU_SCANOUT_BUFFERS */
	uint32_t seq;                      /* how often this buffer has been shown, counting this time */
	uint32_t flags;                    /* SGPU_SCANOUT_VSYNC */
};
/* Show every frame, each for at least one refresh, in order (FIFO). Without it a newer frame
 * replaces one not yet on screen. */
#define SGPU_SCANOUT_VSYNC 1u
/* What a guest driver leaves in the first bytes of such an IOSurface for its window server. */
#define SGPU_SURFACE_MAGIC "SWEETMTL"
#pragma pack(pop)

#endif /* SGPU_PROTO_H */
