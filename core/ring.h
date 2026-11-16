#ifndef HYPERWIN_RING_H
#define HYPERWIN_RING_H

#include "hw_abi.h"
#include "tpm2.h"

void hw_window_init(void *window, size_t bytes);
void hw_publish_frame(void *window, const uint8_t *bgra, uint32_t width, uint32_t height, uint32_t stride);
void hw_publish_audio(void *window, const int16_t *stereo, uint32_t frames);
void hw_set_pad(void *window, uint32_t connected, uint32_t packet, uint16_t buttons,
                uint8_t left_trigger, uint8_t right_trigger,
                int16_t lx, int16_t ly, int16_t rx, int16_t ry);
int hw_copy_front(void *window, uint8_t *dst, uint32_t dst_bytes);
int hw_copy_gop_front(void *window, uint8_t *dst, uint32_t dst_bytes);
/* One TPM request, if the guest has posted one. Returns 1 when a command was answered. */
int hw_tpm_poll(void *window, struct hw_tpm *tpm);

#endif
