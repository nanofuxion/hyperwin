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
// heap: the shared GPU heap (HW_HEAP_PA in the guest), or null to keep mapped memory in step by copying.
int hwgpu_host_start(void* window, size_t window_bytes, void* heap, size_t heap_bytes, uint32_t renderer_flags,
                     uint32_t width, uint32_t height);
// Stops the thread and frees what every guest process made. The renderer stays up.
void hwgpu_host_stop(void);
// The compositor layer SGPU_CMD_SCANOUT frames go on (an ASurfaceControl*, whose reference passes
// to the host; null for none), and the size in pixels it covers. Frames are scaled to that size.
int hwgpu_host_set_layer(void* layer, int view_width, int view_height);
// Whether every scanout frame waits for the phone's screen, as if each program asked for FIFO.
// On by default. Programs that ask for FIFO themselves always wait.
void hwgpu_host_set_vsync(int on);
// The same layer now covers another size.
void hwgpu_host_set_layer_size(int view_width, int view_height);

/* HwD3D's surface (d3d/README.md): a layer above the scanout one, as an ASurfaceControl, and the
 * ANativeWindow its buffers come from (taking a reference), or both null. libhwd3d.so draws on it
 * through these: */
void hwgpu_host_set_d3d_layer(void* layer, void* window);
void* hwgpu_host_d3d_window_acquire(void);    /* a reference, or null when there is no surface */
void hwgpu_host_d3d_window_release(void* window);
void hwgpu_host_d3d_show(int on);
int hwgpu_host_vsync(void);                   /* "Limit to screen refresh" is on */
// How many scanout frames went to the layer so far.
uint64_t hwgpu_host_scanout_frames(void);

#ifdef __cplusplus
}
#endif

#endif
