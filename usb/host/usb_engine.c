#include "usb_engine.h"
#include <errno.h>
#include <string.h>

void usb_region_init(void *region)
{
    struct hw_usb_global *g = region;
    memset(g, 0, sizeof *g);
    memcpy(g->magic, HW_USB_MAGIC, 8);
    g->version = HW_USB_VERSION;
    g->slots = HW_USB_SLOTS;
    g->slot_stride = HW_USB_SLOT_STRIDE;
    __atomic_store_n(&g->host_state, 1, __ATOMIC_RELEASE);
}

int usb_table_offer(void *region, unsigned slot, uint32_t bus_id, uint16_t vid, uint16_t pid, uint32_t speed,
                    const uint8_t *desc, uint32_t desc_len)
{
    struct hw_usb_global *g = region;
    if (slot >= HW_USB_SLOTS || desc_len > HW_USB_DESC_MAX) return -EINVAL;
    struct hw_usb_dev_entry *d = &g->dev[slot];
    if (d->state != HW_USB_FREE) return -EBUSY;
    uint32_t gen = d->generation + 1;
    struct hw_usb_slot s = hw_usb_slot_at(region, slot);
    /* A new device starts with clean rings. */
    s.ctrl->sub_head = s.ctrl->sub_tail = s.ctrl->cmp_head = s.ctrl->cmp_tail = 0;
    s.ctrl->guest_sleep = s.ctrl->host_sleep = 0;
    s.ctrl->slot_generation = gen;
    d->bus_id = bus_id; d->vendor = vid; d->product = pid; d->speed = speed;
    d->desc_len = desc_len; d->guest_port = 0;
    memcpy(d->desc, desc, desc_len);
    d->generation = gen;
    __atomic_store_n(&d->state, HW_USB_OFFERED, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g->table_gen, 1, __ATOMIC_RELEASE);
    return 0;
}

void usb_table_retire(void *region, unsigned slot)
{
    struct hw_usb_global *g = region;
    if (slot >= HW_USB_SLOTS) return;
    __atomic_store_n(&g->dev[slot].state, HW_USB_DETACHING, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g->table_gen, 1, __ATOMIC_RELEASE);
}

void usb_engine_init(struct usb_engine *e, void *region, unsigned slot, const struct usb_backend *be, void *dev)
{
    memset(e, 0, sizeof *e);
    e->slot = hw_usb_slot_at(region, slot);
    e->be = be;
    e->dev = dev;
}

static void report(struct usb_engine *e, uint8_t op, const struct usb_done *d)
{
    struct hw_usb_cmp c;
    memset(&c, 0, sizeof c);
    c.seqnum = d->seqnum; c.op = d->op ? d->op : op; c.status = d->status; c.actual_length = d->actual_length;
    c.start_frame = d->start_frame; c.nr_packets = d->nr_packets; c.error_count = d->error_count;
    while (hw_usb_cmp_push(&e->slot, &c) != 0) { /* the guest reaps; it is never gone for long */ }
    e->completions++;
    e->slot.ctrl->completions = e->completions;
}

int usb_engine_poll(struct usb_engine *e, int max)
{
    int did = 0;
    struct hw_usb_sub s;
    while (did < max && hw_usb_submit_pop(&e->slot, &s) == 0) {
        did++;
        e->submits++;
        e->slot.ctrl->submits = e->submits;
        if (s.op == HW_USB_OP_UNLINK) {
            e->be->cancel(e->dev, s.unlink_seq);
            struct usb_done d = {s.seqnum, HW_USB_OP_UNLINK, 0, 0, 0, 0, 0};   /* RET_UNLINK; the cancelled request completes by itself */
            report(e, HW_USB_OP_UNLINK, &d);
            continue;
        }
        if (s.length && (uint64_t)s.arena_off + s.length > HW_USB_ARENA_SIZE) {
            struct usb_done d = {s.seqnum, s.op, -EINVAL, 0, 0, 0, 0};
            report(e, s.op, &d);
            continue;
        }
        struct usb_done d;
        memset(&d, 0, sizeof d);
        d.seqnum = s.seqnum;
        d.op = s.op;
        int r = e->be->submit(e->dev, &s, e->slot.arena, &d);
        if (r == 0) report(e, s.op, &d);
        else if (r > 0) e->inflight++;
        else { d.status = r; report(e, s.op, &d); }
        if (s.dir == HW_USB_DIR_OUT) e->slot.ctrl->bytes_out += s.length;
    }
    struct usb_done d;
    while (e->inflight && e->be->reap(e->dev, &d) > 0) {
        did++;
        e->inflight--;
        report(e, HW_USB_OP_BULK, &d);
        e->slot.ctrl->bytes_in += d.actual_length;
    }
    return did;
}

void usb_engine_fail_all(struct usb_engine *e)
{
    struct hw_usb_sub s;
    struct usb_done d;
    e->be->cancel_all(e->dev);
    while (e->inflight && e->be->reap(e->dev, &d) > 0) { e->inflight--; d.status = -ENODEV; report(e, HW_USB_OP_BULK, &d); }
    while (hw_usb_submit_pop(&e->slot, &s) == 0) {
        memset(&d, 0, sizeof d);
        d.seqnum = s.seqnum; d.status = -ENODEV;
        report(e, s.op, &d);
    }
}
