#ifndef HYPERWIN_RING_H
#define HYPERWIN_RING_H

#include "hw_abi.h"
#include "tpm2.h"

void hw_window_init(void *window, size_t bytes);
/* Frame size for the next boot; ignored when it does not fit the window. Call after hw_window_init. */
void hw_window_set_mode(void *window, uint32_t width, uint32_t height);
void hw_publish_frame(void *window, const uint8_t *bgra, uint32_t width, uint32_t height, uint32_t stride);
void hw_publish_audio(void *window, const int16_t *stereo, uint32_t frames);
int hw_copy_front(void *window, uint8_t *dst, uint32_t dst_bytes);
int hw_copy_gop_front(void *window, uint8_t *dst, uint32_t dst_bytes);
/* One TPM request, if the guest has posted one. Returns 1 when a command was answered. */
int hw_tpm_poll(void *window, struct hw_tpm *tpm);

#endif
