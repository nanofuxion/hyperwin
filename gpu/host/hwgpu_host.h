// Phone side of the HyperWin GPU rings: answers sgpu requests (sgpu_proto.h) arriving in the
// shared window (hwgpu_ring.h) with gfxstream's renderer on the phone's Vulkan driver.
#ifndef HWGPU_HOST_H
#define HWGPU_HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Lays out the rings in the window, starts gfxstream once per process and a thread that serves
// every channel. renderer_flags are STREAM_RENDERER_FLAGS_* bits. Returns 0 or a negative errno.
// Call after hw_window_init and before the guest boots.
int hwgpu_host_start(void* window, size_t window_bytes, uint32_t renderer_flags, uint32_t width,
                     uint32_t height);
// Stops the thread and frees what every guest process made. The renderer stays up.
void hwgpu_host_stop(void);

#ifdef __cplusplus
}
#endif

#endif
