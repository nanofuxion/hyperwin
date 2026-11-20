/* The phone side of USB passthrough (usb/README.md, phase 2): one worker thread per device, on the usbfs file
 * descriptor UsbDeviceConnection hands out. Requests come off the slot's submit ring (include/hw_usb.h); bulk and
 * interrupt data go straight into the arena through the URB (no extra copy), control transfers go through a small
 * buffer because usbfs wants the setup packet in front of the data. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <linux/usbdevice_fs.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <android/log.h>
#include "../../../../usb/host/usb_engine.h"

#define TAG "HyperWinUsb"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

#define MAX_REQ 256

struct req {
    struct usbdevfs_urb urb;       /* first, so the pointer usbfs gives back is the request */
    int used;
    uint32_t seqnum;
    uint8_t op, dir;
    uint8_t *ctrl;                 /* control: setup + data */
    uint32_t arena_off, length;
    struct usbdevfs_iso_packet_desc iso[1];   /* iso is added in phase 5 */
};

struct dev {
    int fd;
    unsigned slot;
    volatile int run;
    pthread_t th;
    struct req reqs[MAX_REQ];
    struct usb_engine eng;
    uint8_t desc[HW_USB_DESC_MAX];
    uint32_t desc_len, bus_id, speed;
    uint16_t vid, pid;
};

static uint8_t *g_region;
static struct dev *g_dev[HW_USB_SLOTS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int be_submit(void *p, const struct hw_usb_sub *s, uint8_t *arena, struct usb_done *done)
{
    struct dev *d = p;
    int r;
    switch (s->op) {
    case HW_USB_OP_SET_INTERFACE: {
        struct usbdevfs_setinterface si = {.interface = s->ep, .altsetting = s->value};
        return ioctl(d->fd, USBDEVFS_SETINTERFACE, &si) ? -errno : 0;
    }
    case HW_USB_OP_SET_CONFIG: { unsigned c = s->value; return ioctl(d->fd, USBDEVFS_SETCONFIGURATION, &c) ? -errno : 0; }
    case HW_USB_OP_CLEAR_HALT: { unsigned e = s->ep; return ioctl(d->fd, USBDEVFS_CLEAR_HALT, &e) ? -errno : 0; }
    case HW_USB_OP_RESET: return ioctl(d->fd, USBDEVFS_RESET, 0) ? -errno : 0;
    case HW_USB_OP_CTRL: case HW_USB_OP_BULK: case HW_USB_OP_INT: break;
    default: return -ENOTSUP;
    }
    struct req *q = NULL;
    for (int i = 0; i < MAX_REQ; i++) if (!d->reqs[i].used) { q = &d->reqs[i]; break; }
    if (!q) return -ENOMEM;
    memset(q, 0, sizeof *q);
    q->used = 1; q->seqnum = s->seqnum; q->op = s->op; q->dir = s->dir; q->arena_off = s->arena_off; q->length = s->length;
    q->urb.usercontext = q;
    q->urb.endpoint = (unsigned char)(s->ep | (s->dir == HW_USB_DIR_IN ? 0x80 : 0));
    if (s->op == HW_USB_OP_CTRL) {
        q->ctrl = malloc(8u + s->length);
        if (!q->ctrl) { q->used = 0; return -ENOMEM; }
        memcpy(q->ctrl, s->setup, 8);
        q->urb.type = USBDEVFS_URB_TYPE_CONTROL;
        q->urb.endpoint = 0;
        q->urb.buffer = q->ctrl;
        q->urb.buffer_length = 8 + s->length;
        /* the direction of a control transfer is bit 7 of bmRequestType, not ours */
        if (!(s->setup[0] & 0x80) && s->length) memcpy(q->ctrl + 8, arena + s->arena_off, s->length);
    } else {
        q->urb.type = s->op == HW_USB_OP_BULK ? USBDEVFS_URB_TYPE_BULK : USBDEVFS_URB_TYPE_INTERRUPT;
        q->urb.buffer = arena + s->arena_off;    /* the arena itself: no copy of ours */
        q->urb.buffer_length = (int)s->length;
        if (s->transfer_flags & 0x1) q->urb.flags |= USBDEVFS_URB_SHORT_NOT_OK;
        if (s->transfer_flags & 0x40) q->urb.flags |= USBDEVFS_URB_ZERO_PACKET;
        if (s->stream_id) q->urb.stream_id = s->stream_id;
    }
    r = ioctl(d->fd, USBDEVFS_SUBMITURB, &q->urb);
    if (r) { int e = -errno; free(q->ctrl); q->used = 0; return e; }
    (void)done;
    return 1;   /* reaped later */
}

static int be_reap(void *p, struct usb_done *done)
{
    struct dev *d = p;
    struct usbdevfs_urb *u = NULL;
    if (ioctl(d->fd, USBDEVFS_REAPURBNDELAY, &u)) return errno == ENODEV ? -ENODEV : 0;
    struct req *q = u->usercontext;
    memset(done, 0, sizeof *done);
    done->seqnum = q->seqnum;
    done->op = q->op;
    done->status = u->status ? -(u->status < 0 ? -u->status : u->status) : 0;
    done->actual_length = (uint32_t)u->actual_length;
    if (q->ctrl) {
        if (q->urb.actual_length > 0 && (q->ctrl[0] & 0x80) && q->urb.actual_length <= (int)q->length)
            memcpy(d->eng.slot.arena + q->arena_off, q->ctrl + 8, (size_t)q->urb.actual_length);
        free(q->ctrl);
    }
    q->used = 0;
    return 1;
}

static void be_cancel(void *p, uint32_t seq)
{
    struct dev *d = p;
    for (int i = 0; i < MAX_REQ; i++) if (d->reqs[i].used && d->reqs[i].seqnum == seq) ioctl(d->fd, USBDEVFS_DISCARDURB, &d->reqs[i].urb);
}

static void be_cancel_all(void *p)
{
    struct dev *d = p;
    for (int i = 0; i < MAX_REQ; i++) if (d->reqs[i].used) ioctl(d->fd, USBDEVFS_DISCARDURB, &d->reqs[i].urb);
}

static const struct usb_backend k_be = {be_submit, be_reap, be_cancel, be_cancel_all};

static void *worker(void *p)
{
    struct dev *d = p;
    int idle = 0;
    LOG("slot %u: %04x:%04x serving", d->slot, d->vid, d->pid);
    while (d->run) {
        if (usb_engine_poll(&d->eng, 32)) { idle = 0; continue; }
        /* A dead device shows as ENODEV from the reap. */
        struct pollfd pf = {d->fd, POLLOUT, 0};
        if (idle < 2000) { idle++; sched_yield(); continue; }     /* short spins while busy */
        int w = poll(&pf, 1, idle < 20000 ? 1 : 20);              /* then back off */
        if (w > 0 && (pf.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
        if (idle < 1000000) idle++;
        unsigned spd = 0;
        if (ioctl(d->fd, USBDEVFS_GET_SPEED, &spd) < 0 && errno == ENODEV) break;   /* unplugged */
    }
    LOG("slot %u: stopping", d->slot);
    usb_table_retire(g_region, d->slot);
    usb_engine_fail_all(&d->eng);
    return NULL;
}

JNIEXPORT void JNICALL
Java_dev_hyperwin_app_UsbBridge_nativeStart(JNIEnv *env, jobject self, jobject region)
{
    (void)self;
    g_region = (*env)->GetDirectBufferAddress(env, region);
    if (g_region) usb_region_init(g_region);   /* keeps the magic the app wrote, adds the table */
    LOG("region %p", (void *)g_region);
}

JNIEXPORT jint JNICALL
Java_dev_hyperwin_app_UsbBridge_nativeAttach(JNIEnv *env, jobject self, jint fd, jint busId, jint vid, jint pid,
                                              jint speed, jbyteArray desc)
{
    (void)self;
    if (!g_region) return -EINVAL;
    pthread_mutex_lock(&g_lock);
    int slot = -1;
    for (unsigned i = 0; i < HW_USB_SLOTS; i++) if (!g_dev[i]) { slot = (int)i; break; }
    if (slot < 0) { pthread_mutex_unlock(&g_lock); return -ENOSPC; }
    struct dev *d = calloc(1, sizeof *d);
    if (!d) { pthread_mutex_unlock(&g_lock); return -ENOMEM; }
    d->fd = fd; d->slot = (unsigned)slot; d->run = 1; d->vid = (uint16_t)vid; d->pid = (uint16_t)pid;
    d->bus_id = (uint32_t)busId; d->speed = (uint32_t)speed;
    int sp = ioctl(fd, USBDEVFS_GET_SPEED, 0);   /* usbfs knows: 1 low, 2 full, 3 high, 5 super, 6 super+ */
    if (sp > 0) d->speed = (uint32_t)sp;
    d->desc_len = (uint32_t)(*env)->GetArrayLength(env, desc);
    if (d->desc_len > HW_USB_DESC_MAX) d->desc_len = HW_USB_DESC_MAX;
    (*env)->GetByteArrayRegion(env, desc, 0, (jsize)d->desc_len, (jbyte *)d->desc);
    usb_engine_init(&d->eng, g_region, d->slot, &k_be, d);
    /* the table says free until the guest has given the previous generation back */
    ((struct hw_usb_global *)g_region)->dev[d->slot].state = HW_USB_FREE;
    int r = usb_table_offer(g_region, d->slot, d->bus_id, d->vid, d->pid, d->speed, d->desc, d->desc_len);
    if (r) { free(d); pthread_mutex_unlock(&g_lock); return r; }
    g_dev[slot] = d;
    pthread_create(&d->th, NULL, worker, d);
    pthread_mutex_unlock(&g_lock);
    return slot;
}

JNIEXPORT void JNICALL
Java_dev_hyperwin_app_UsbBridge_nativeDetach(JNIEnv *env, jobject self, jint slot)
{
    (void)env; (void)self;
    pthread_mutex_lock(&g_lock);
    struct dev *d = slot >= 0 && slot < (jint)HW_USB_SLOTS ? g_dev[slot] : NULL;
    if (d) {
        d->run = 0;
        pthread_join(d->th, NULL);
        g_dev[slot] = NULL;
        free(d);
    }
    pthread_mutex_unlock(&g_lock);
}

/* The guest has taken the device back: the slot is free again for a new offer. Called when it reads FREE. */
JNIEXPORT jint JNICALL
Java_dev_hyperwin_app_UsbBridge_nativeSlotState(JNIEnv *env, jobject self, jint slot)
{
    (void)env; (void)self;
    if (!g_region || slot < 0 || slot >= (jint)HW_USB_SLOTS) return -1;
    return (jint)__atomic_load_n(&((struct hw_usb_global *)g_region)->dev[slot].state, __ATOMIC_ACQUIRE);
}
