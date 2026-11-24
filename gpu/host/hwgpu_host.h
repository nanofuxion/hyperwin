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
/* size bytes of the shared GPU heap, granted to channel slot so its Windows process may map them:
 * the phone's address, and *offset in the heap. Null when the heap is missing or full. The grant
 * ends with the connection; the memory with hwgpu_host_heap_free. */
void* hwgpu_host_heap_grant(uint32_t slot, uint64_t size, uint64_t* offset);
void hwgpu_host_heap_free(void* p);
// How many scanout frames went to the layer so far.
uint64_t hwgpu_host_scanout_frames(void);
/* A frame from Windows' screen driver, an AHardwareBuffer, onto the scanout layer. */
void hwgpu_host_show_buffer(void* ahb, uint32_t width, uint32_t height);
/* A fullscreen game's frame (an AHardwareBuffer) from channel slot, shown directly on the scanout layer
 * above the desktop until hwgpu_host_hide_game_buffer(slot) or the channel closes. */
void hwgpu_host_show_game_buffer(uint32_t slot, uint32_t generation, void* ahb, uint32_t width, uint32_t height, int vsync);
void hwgpu_host_hide_game_buffer(uint32_t slot);
void hwgpu_host_show_window_buffer(uint32_t slot, void* ahb, uint32_t width, uint32_t height, int32_t x, int32_t y, uint32_t cw, uint32_t ch, const uint32_t* crop);
int hwgpu_host_window_hole(int32_t* ltrb);  /* the desktop rect a windowed program shows through, or 0 */
/* Updates the cursor image (RGBA8888, up to 64x64) shown over a fullscreen game. */
void hwgpu_host_set_cursor_shape(uint32_t slot, uint32_t width, uint32_t height, int32_t hot_x, int32_t hot_y,
                                 const void* rgba);

/* The desktop and games processes (hwgpu_host.cpp, g_role). */
typedef int (*hwgpu_handoff_cb)(uint32_t slot, uint32_t generation, const char* exe, const char* conf);
typedef void (*hwgpu_hole_cb)(int on, int32_t left, int32_t top, int32_t right, int32_t bottom);
void hwgpu_host_set_callbacks(hwgpu_handoff_cb handoff, hwgpu_hole_cb hole);
int hwgpu_host_role(void);  /* 0 the desktop process, 1 the games process */
/* Desktop process: gives a Direct3D program's channel to the games process. 1 when it took it. */
int hwgpu_host_handoff(uint32_t slot, uint32_t generation, const char* exe, const char* conf);
/* Games process: serves a channel handed over. 1 when it started. */
int hwgpu_host_adopt(uint32_t slot, uint32_t generation, const char* exe, const char* conf);
/* Desktop process: the games process ended; the channels it had are dropped. */
void hwgpu_host_games_died(void);
/* Desktop process: where a windowed game in the games process shows, in screen pixels. */
void hwgpu_host_set_hole(int on, int32_t left, int32_t top, int32_t right, int32_t bottom);
/* Desktop process: the app's layer for Windows' pointer, over everything (an ASurfaceControl*), or null. */
void hwgpu_host_set_pointer_layer(void* layer);
/* Games process: the app's layer under the desktop (an ASurfaceControl*, reference passes), or null. */
void hwgpu_host_set_under_layer(void* layer);

#ifdef __cplusplus
}
#endif

#endif
