/* The phone side of the USB ring (usb/README.md), without any USB in it: it moves requests from a slot's
 * submit ring to a backend and completions back. The backend is usbfs on the phone (usb_host.c, later) or a
 * loopback device in the Mac test. */
#ifndef HYPERWIN_USB_ENGINE_H
#define HYPERWIN_USB_ENGINE_H
#include <stdint.h>
#include "../../include/hw_usb.h"

struct usb_done {                 /* what a backend reports for one request */
    uint32_t seqnum;
    uint8_t op;                   /* of the request; 0 = the engine fills it from the one it started */
    int32_t status;
    uint32_t actual_length;
    uint32_t start_frame, nr_packets, error_count;
};

struct usb_backend {
    /* Start a request. 0: done now, `done` filled. 1: queued, reap will report it. <0: refused, a negative errno. */
    int (*submit)(void *dev, const struct hw_usb_sub *s, uint8_t *arena, struct usb_done *done);
    /* A finished request, if there is one: 1 and `done`, else 0. A dead device reports -ENODEV for everything. */
    int (*reap)(void *dev, struct usb_done *done);
    /* Try to cancel a queued request. */
    void (*cancel)(void *dev, uint32_t seqnum);
    /* The device is gone: queue a -ENODEV completion for every request still in flight. */
    void (*cancel_all)(void *dev);
};

struct usb_engine {
    struct hw_usb_slot slot;
    const struct usb_backend *be;
    void *dev;
    uint32_t inflight;
    uint64_t submits, completions;
};

void usb_engine_init(struct usb_engine *e, void *region, unsigned slot, const struct usb_backend *be, void *dev);
/* One pass: take up to `max` requests, reap what finished. Returns how many things happened (0: idle). */
int usb_engine_poll(struct usb_engine *e, int max);
/* The device went away: every request still on the submit ring and in flight completes with -ENODEV. */
void usb_engine_fail_all(struct usb_engine *e);

/* Device table (host side): publish a device and its descriptors, or retire it. */
int usb_table_offer(void *region, unsigned slot, uint32_t bus_id, uint16_t vid, uint16_t pid, uint32_t speed,
                    const uint8_t *desc, uint32_t desc_len);
void usb_table_retire(void *region, unsigned slot);
void usb_region_init(void *region);

#endif
