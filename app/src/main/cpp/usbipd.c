/* A USB/IP server in the app (usb/README.md): Windows runs usbip-win2's client and virtual host controller
 * (certified drivers, BSD licensed), and this serves the phone's USB devices to it. The guest connects to 10.0.2.2:3240,
 * which the app's network turns into this loopback socket (GuestNet.kt); nothing listens on the phone's network.
 * Protocol version 0x0111: OP_REQ_DEVLIST, OP_REQ_IMPORT, then USBIP_CMD_SUBMIT / UNLINK and their replies, as in
 * the Linux usbip documentation. Control, bulk and interrupt transfers; isochronous is refused for now. The usbfs
 * file descriptor comes from UsbDeviceConnection (UsbBridge.kt). */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <jni.h>
#include <linux/usbdevice_fs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <android/log.h>

#define TAG "HyperWinUsbip"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

#define USBIP_VERSION 0x0111
#define OP_REQ_DEVLIST 0x8005
#define OP_REP_DEVLIST 0x0005
#define OP_REQ_IMPORT 0x8003
#define OP_REP_IMPORT 0x0003
#define CMD_SUBMIT 1
#define CMD_UNLINK 2
#define RET_SUBMIT 3
#define RET_UNLINK 4
#define MAX_DEV 8
#define MAX_REQ 512

struct dev {
    int used, fd, busnum, devnum;
    char busid[32];
    uint8_t desc[4096];
    uint32_t desc_len;
    uint16_t vid, pid;
    uint32_t speed;
    int in_use;               /* imported by a client */
};

struct req {
    struct usbdevfs_urb urb;  /* first: the pointer usbfs returns */
    int used;
    uint32_t seq;
    uint8_t dir;              /* 0 out, 1 in */
    uint8_t *buf;             /* bulk/interrupt: the data; control: setup + data */
    uint32_t len;
};

static struct dev g_dev[MAX_DEV];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_listen = -1;

static int rd_all(int s, void *b, size_t n)
{
    uint8_t *p = b;
    while (n) { ssize_t r = recv(s, p, n, 0); if (r <= 0) return -1; p += r; n -= (size_t)r; }
    return 0;
}
static int wr_all(int s, const void *b, size_t n)
{
    const uint8_t *p = b;
    while (n) { ssize_t r = send(s, p, n, MSG_NOSIGNAL); if (r <= 0) return -1; p += r; n -= (size_t)r; }
    return 0;
}

/* struct usbip_usb_device on the wire (312 bytes), and the interface records. */
static void put32(uint8_t *p, uint32_t v) { v = htonl(v); memcpy(p, &v, 4); }
static void put16(uint8_t *p, uint16_t v) { v = htons(v); memcpy(p, &v, 2); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return ntohl(v); }

static int ninterfaces(const struct dev *d, uint8_t *classes, int max)
{
    int n = 0;
    const uint8_t *p = d->desc + 18, *e = d->desc + d->desc_len;
    while (p + 2 <= e && p[0]) {
        if (p[1] == 4 && p + 9 <= e && p[3] == 0 && n < max) { classes[3 * n] = p[5]; classes[3 * n + 1] = p[6]; classes[3 * n + 2] = p[7]; n++; }
        p += p[0];
    }
    return n;
}

static void fill_dev(uint8_t *o, const struct dev *d)
{
    memset(o, 0, 312);
    snprintf((char *)o, 256, "/sys/devices/hyperwin/usb%d/%s", d->busnum, d->busid);
    memcpy(o + 256, d->busid, strlen(d->busid) < 31 ? strlen(d->busid) : 31);
    put32(o + 288, (uint32_t)d->busnum);
    put32(o + 292, (uint32_t)d->devnum);
    put32(o + 296, d->speed);
    put16(o + 300, d->vid);
    put16(o + 302, d->pid);
    put16(o + 304, (uint16_t)(d->desc[12] | d->desc[13] << 8));   /* bcdDevice */
    o[306] = d->desc[4]; o[307] = d->desc[5]; o[308] = d->desc[6];   /* class, subclass, protocol */
    o[309] = d->desc_len > 18 + 5 ? d->desc[18 + 5] : 1;           /* bConfigurationValue */
    o[310] = d->desc[17];                                          /* bNumConfigurations */
    uint8_t ic[3 * 32];
    o[311] = (uint8_t)ninterfaces(d, ic, 32);
}

static void op_reply(uint8_t *h, uint16_t code, uint32_t status)
{
    put16(h, USBIP_VERSION); put16(h + 2, code); put32(h + 4, status);
}

static uint8_t ep_type(const struct dev *d, uint8_t addr)
{
    const uint8_t *p = d->desc + 18, *e = d->desc + d->desc_len;
    while (p + 2 <= e && p[0]) {
        if (p[1] == 5 && p + 7 <= e && p[2] == addr) return p[3] & 3;   /* 2 bulk, 3 interrupt */
        p += p[0];
    }
    return 2;
}

struct conn {
    int s;
    struct dev *d;
    pthread_mutex_t lock;
    struct req reqs[MAX_REQ];
    volatile int run;
};

static void send_ret_submit(struct conn *c, struct req *q, int32_t status, uint32_t actual, int with_data)
{
    uint8_t h[48];
    memset(h, 0, sizeof h);
    put32(h, RET_SUBMIT); put32(h + 4, q->seq);
    put32(h + 20, (uint32_t)status); put32(h + 24, actual); put32(h + 32, 0xffffffffu);
    pthread_mutex_lock(&c->lock);
    int ok = !wr_all(c->s, h, 48);
    if (ok && with_data && actual) ok = !wr_all(c->s, q->buf, actual);
    pthread_mutex_unlock(&c->lock);
    if (!ok) c->run = 0;
}

static void free_req(struct req *q) { free(q->buf); q->buf = NULL; q->used = 0; }

static void *reaper(void *arg)
{
    struct conn *c = arg;
    while (c->run) {
        struct pollfd pf = {c->d->fd, POLLOUT, 0};
        int w = poll(&pf, 1, 100);
        if (w < 0 && errno != EINTR) break;
        if (w > 0 && (pf.revents & (POLLERR | POLLHUP | POLLNVAL))) { c->run = 0; break; }
        struct usbdevfs_urb *u = NULL;
        while (ioctl(c->d->fd, USBDEVFS_REAPURBNDELAY, &u) == 0 && u) {
            struct req *q = (struct req *)u;
            int32_t st = u->status ? (u->status < 0 ? u->status : -u->status) : 0;
            uint32_t act = (uint32_t)u->actual_length;
            if (q->urb.type == USBDEVFS_URB_TYPE_CONTROL) {
                /* usbfs counts the setup packet in neither length; the data is behind it */
                if (q->dir && act) memmove(q->buf, q->buf + 8, act);
            }
            send_ret_submit(c, q, st, act, q->dir);
            free_req(q);
            u = NULL;
        }
        if (errno == ENODEV) { c->run = 0; break; }
    }
    return NULL;
}

static int serve_urbs(struct conn *c)
{
    pthread_t th;
    c->run = 1;
    pthread_create(&th, NULL, reaper, c);
    while (c->run) {
        uint8_t h[48];
        if (rd_all(c->s, h, 48)) break;
        uint32_t cmd = get32(h), seq = get32(h + 4), dir = get32(h + 12), ep = get32(h + 16);
        if (cmd == CMD_SUBMIT) {
            uint32_t flags = get32(h + 20), len = get32(h + 24), npk = get32(h + 32);
            struct req *q = NULL;
            for (int i = 0; i < MAX_REQ; i++) if (!c->reqs[i].used) { q = &c->reqs[i]; break; }
            uint8_t *buf = malloc((size_t)len + 8 + 8);
            if (!q || !buf) { free(buf); break; }
            memset(q, 0, sizeof *q);
            q->used = 1; q->seq = seq; q->dir = (uint8_t)dir; q->len = len;
            if (dir == 0 && len) { if (rd_all(c->s, buf + (ep == 0 ? 8 : 0), len)) { free(buf); q->used = 0; break; } }
            if (npk != 0 && npk != 0xffffffffu) {   /* isochronous: not yet */
                uint8_t skip[16]; for (uint32_t i = 0; i < npk; i++) if (rd_all(c->s, skip, 16)) break;
                q->buf = buf; send_ret_submit(c, q, -EPROTO, 0, 0); free_req(q); continue;
            }
            q->buf = buf;
            q->urb.usercontext = q;
            if (ep == 0) {
                memcpy(buf, h + 40, 8);
                uint8_t rt = buf[0], rq = buf[1];
                LOG("ctl %02x %02x %02x%02x %02x%02x len %u", rt, rq, buf[3], buf[2], buf[5], buf[4], len);
                /* Standard requests that would change what Android configured: answer them without sending.
                 * SET_ADDRESS, SET_CONFIGURATION, SET_INTERFACE, SET_SEL, SET_ISOCH_DELAY. */
                if (rt == 0x00 && (rq == 0x05 || rq == 0x09 || rq == 0x30 || rq == 0x31) ||
                    rt == 0x01 && rq == 0x0b) {
                    send_ret_submit(c, q, 0, 0, 0); free_req(q); continue;
                }
                q->urb.type = USBDEVFS_URB_TYPE_CONTROL;
                q->urb.endpoint = 0;
                q->urb.buffer = buf;
                q->urb.buffer_length = (int)(8 + len);
            } else {
                uint8_t addr = (uint8_t)(ep | (dir ? 0x80 : 0));
                q->urb.type = ep_type(c->d, addr) == 3 ? USBDEVFS_URB_TYPE_INTERRUPT : USBDEVFS_URB_TYPE_BULK;
                q->urb.endpoint = addr;
                q->urb.buffer = buf;
                q->urb.buffer_length = (int)len;
                if (flags & 0x1) q->urb.flags |= USBDEVFS_URB_SHORT_NOT_OK;
                if (flags & 0x40) q->urb.flags |= USBDEVFS_URB_ZERO_PACKET;
            }
            if (ioctl(c->d->fd, USBDEVFS_SUBMITURB, &q->urb)) { int e = -errno; send_ret_submit(c, q, e, 0, 0); free_req(q); }
        } else if (cmd == CMD_UNLINK) {
            uint32_t victim = get32(h + 20);
            int32_t st = 0;
            for (int i = 0; i < MAX_REQ; i++)
                if (c->reqs[i].used && c->reqs[i].seq == victim) { if (!ioctl(c->d->fd, USBDEVFS_DISCARDURB, &c->reqs[i].urb)) st = -ECONNRESET; break; }
            uint8_t r[48];
            memset(r, 0, sizeof r);
            put32(r, RET_UNLINK); put32(r + 4, seq); put32(r + 20, (uint32_t)st);
            pthread_mutex_lock(&c->lock);
            wr_all(c->s, r, 48);
            pthread_mutex_unlock(&c->lock);
        } else break;
    }
    c->run = 0;
    pthread_join(th, NULL);
    for (int i = 0; i < MAX_REQ; i++) if (c->reqs[i].used) { ioctl(c->d->fd, USBDEVFS_DISCARDURB, &c->reqs[i].urb); }
    struct usbdevfs_urb *u = NULL;
    while (ioctl(c->d->fd, USBDEVFS_REAPURBNDELAY, &u) == 0 && u) u = NULL;
    for (int i = 0; i < MAX_REQ; i++) if (c->reqs[i].used) free_req(&c->reqs[i]);
    return 0;
}

static void *client(void *arg)
{
    int s = (int)(intptr_t)arg;
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    uint8_t h[8];
    while (!rd_all(s, h, 8)) {
        uint32_t code = (uint32_t)(h[2] << 8 | h[3]);
        if (code == OP_REQ_DEVLIST) {
            uint8_t out[8 + 4], rec[312];
            op_reply(out, OP_REP_DEVLIST, 0);
            pthread_mutex_lock(&g_lock);
            uint32_t n = 0;
            for (int i = 0; i < MAX_DEV; i++) if (g_dev[i].used) n++;
            put32(out + 8, n);
            wr_all(s, out, 12);
            for (int i = 0; i < MAX_DEV; i++) if (g_dev[i].used) {
                fill_dev(rec, &g_dev[i]);
                wr_all(s, rec, 312);
                uint8_t ic[3 * 32], ni = (uint8_t)ninterfaces(&g_dev[i], ic, 32);
                for (int k = 0; k < ni; k++) { uint8_t r4[4] = {ic[3 * k], ic[3 * k + 1], ic[3 * k + 2], 0}; wr_all(s, r4, 4); }
            }
            pthread_mutex_unlock(&g_lock);
        } else if (code == OP_REQ_IMPORT) {
            char busid[32];
            if (rd_all(s, busid, 32)) break;
            busid[31] = 0;
            struct dev *d = NULL;
            pthread_mutex_lock(&g_lock);
            for (int i = 0; i < MAX_DEV; i++) if (g_dev[i].used && !strcmp(g_dev[i].busid, busid)) d = &g_dev[i];
            if (d && d->in_use) d = NULL;
            if (d) d->in_use = 1;
            pthread_mutex_unlock(&g_lock);
            uint8_t out[8 + 312];
            if (!d) { op_reply(out, OP_REP_IMPORT, 1); wr_all(s, out, 8); continue; }
            op_reply(out, OP_REP_IMPORT, 0);
            fill_dev(out + 8, d);
            if (wr_all(s, out, sizeof out)) { d->in_use = 0; break; }
            LOG("%s imported", busid);
            struct conn *c = calloc(1, sizeof *c);
            c->s = s; c->d = d;
            pthread_mutex_init(&c->lock, NULL);
            serve_urbs(c);
            free(c);
            d->in_use = 0;
            LOG("%s released", busid);
            break;   /* a connection serves one device */
        } else break;
    }
    close(s);
    return NULL;
}

static void *acceptor(void *arg)
{
    (void)arg;
    for (;;) {
        int s = accept(g_listen, NULL, NULL);
        if (s < 0) { if (errno == EINTR) continue; break; }
        pthread_t th;
        pthread_create(&th, NULL, client, (void *)(intptr_t)s);
        pthread_detach(th);
    }
    return NULL;
}

JNIEXPORT jint JNICALL
Java_dev_hyperwin_app_UsbBridge_nativeUsbipStart(JNIEnv *env, jobject self, jint port)
{
    (void)env; (void)self;
    if (g_listen >= 0) return 0;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 4)) { int e = errno; close(s); LOG("listen failed %d", e); return -e; }
    g_listen = s;
    pthread_t th;
    pthread_create(&th, NULL, acceptor, NULL);
    pthread_detach(th);
    LOG("usbipd on 127.0.0.1:%d", port);
    return 0;
}

JNIEXPORT jint JNICALL
Java_dev_hyperwin_app_UsbBridge_nativeUsbipAdd(JNIEnv *env, jobject self, jint fd, jint busnum, jint devnum, jint vid, jint pid,
                                                jbyteArray desc)
{
    (void)self;
    pthread_mutex_lock(&g_lock);
    int slot = -1;
    for (int i = 0; i < MAX_DEV; i++) if (!g_dev[i].used) { slot = i; break; }
    if (slot < 0) { pthread_mutex_unlock(&g_lock); return -ENOSPC; }
    struct dev *d = &g_dev[slot];
    memset(d, 0, sizeof *d);
    d->fd = fd; d->busnum = busnum; d->devnum = devnum; d->vid = (uint16_t)vid; d->pid = (uint16_t)pid;
    snprintf(d->busid, sizeof d->busid, "%d-%d", busnum, devnum);
    d->desc_len = (uint32_t)(*env)->GetArrayLength(env, desc);
    if (d->desc_len > sizeof d->desc) d->desc_len = sizeof d->desc;
    (*env)->GetByteArrayRegion(env, desc, 0, (jsize)d->desc_len, (jbyte *)d->desc);
    int sp = ioctl(fd, USBDEVFS_GET_SPEED, 0);
    d->speed = sp > 0 ? (uint32_t)sp : 3;
    d->used = 1;
    pthread_mutex_unlock(&g_lock);
    LOG("%s %04x:%04x offered", d->busid, d->vid, d->pid);
    return slot;
}

JNIEXPORT void JNICALL
Java_dev_hyperwin_app_UsbBridge_nativeUsbipRemove(JNIEnv *env, jobject self, jint slot)
{
    (void)env; (void)self;
    if (slot < 0 || slot >= MAX_DEV) return;
    pthread_mutex_lock(&g_lock);
    g_dev[slot].used = 0;   /* a client that has it imported sees ENODEV from usbfs and lets go */
    pthread_mutex_unlock(&g_lock);
}
