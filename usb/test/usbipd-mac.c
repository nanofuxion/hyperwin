/* A USB/IP server for the Mac, for testing the guest's USB/IP path with a device plugged into the Mac
 * instead of the phone (usb/README.md). Same protocol and rules as the app's app/src/main/cpp/usbipd.c,
 * on libusb instead of usbfs. Run as root so libusb can take interfaces from macOS's own drivers.
 *
 *   cc -O2 -I/opt/homebrew/include usb/test/usbipd-mac.c -L/opt/homebrew/lib -lusb-1.0 -o build/usbipd-mac
 *   sudo build/usbipd-mac 17e9:6011 05e3:0764      (vid:pid of each device to export; none = all non-hubs)
 *
 * Listens on port 3240 on every interface. In Windows: usbip.exe attach -r <Mac's IP> -b <busid>. */
#include <arpa/inet.h>
#include <errno.h>
#include <libusb-1.0/libusb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)

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
    libusb_device *ud;
    libusb_device_handle *h;
    int busnum, devnum;
    char busid[32];
    uint8_t desc[4096];
    uint32_t desc_len;
    uint16_t vid, pid;
    uint32_t speed;
    int in_use;
};

struct conn;
struct req {
    struct libusb_transfer *t;
    struct conn *c;
    int used, unlinked;
    uint32_t seq;
    uint8_t dir, ctl;
    uint8_t *buf;
};

struct conn {
    int s;
    struct dev *d;
    pthread_mutex_t lock;
    struct req reqs[MAX_REQ];
    volatile int run;
    int pending;
};

static struct dev g_dev[MAX_DEV];
static int g_ndev;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int rd_all(int s, void *b, size_t n)
{
    uint8_t *p = b;
    while (n) { ssize_t r = recv(s, p, n, 0); if (r <= 0) return -1; p += r; n -= (size_t)r; }
    return 0;
}
static int wr_all(int s, const void *b, size_t n)
{
    const uint8_t *p = b;
    while (n) { ssize_t r = send(s, p, n, 0); if (r <= 0) return -1; p += r; n -= (size_t)r; }
    return 0;
}
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
    snprintf((char *)o, 256, "/sys/devices/mac/usb%d/%s", d->busnum, d->busid);
    memcpy(o + 256, d->busid, strlen(d->busid) < 31 ? strlen(d->busid) : 31);
    put32(o + 288, (uint32_t)d->busnum);
    put32(o + 292, (uint32_t)d->devnum);
    put32(o + 296, d->speed);
    put16(o + 300, d->vid);
    put16(o + 302, d->pid);
    put16(o + 304, (uint16_t)(d->desc[12] | d->desc[13] << 8));
    o[306] = d->desc[4]; o[307] = d->desc[5]; o[308] = d->desc[6];
    o[309] = d->desc_len > 18 + 5 ? d->desc[18 + 5] : 1;
    o[310] = d->desc[17];
    uint8_t ic[3 * 32];
    o[311] = (uint8_t)ninterfaces(d, ic, 32);
}

static void op_reply(uint8_t *h, uint16_t code, uint32_t status) { put16(h, USBIP_VERSION); put16(h + 2, code); put32(h + 4, status); }

static uint8_t ep_type(const struct dev *d, uint8_t addr)
{
    const uint8_t *p = d->desc + 18, *e = d->desc + d->desc_len;
    while (p + 2 <= e && p[0]) {
        if (p[1] == 5 && p + 7 <= e && p[2] == addr) return p[3] & 3;
        p += p[0];
    }
    return 2;
}

static void send_ret(struct conn *c, uint32_t seq, int32_t status, uint32_t actual, const uint8_t *data)
{
    uint8_t h[48];
    memset(h, 0, sizeof h);
    put32(h, RET_SUBMIT); put32(h + 4, seq);
    put32(h + 20, (uint32_t)status); put32(h + 24, actual); put32(h + 32, 0xffffffffu);
    pthread_mutex_lock(&c->lock);
    int ok = !wr_all(c->s, h, 48);
    if (ok && data && actual) ok = !wr_all(c->s, data, actual);
    pthread_mutex_unlock(&c->lock);
    if (!ok) c->run = 0;
}

static int32_t status_of(enum libusb_transfer_status s)
{
    switch (s) {
    case LIBUSB_TRANSFER_COMPLETED: return 0;
    case LIBUSB_TRANSFER_STALL: return -EPIPE;
    case LIBUSB_TRANSFER_NO_DEVICE: return -ENODEV;
    case LIBUSB_TRANSFER_TIMED_OUT: return -ETIMEDOUT;
    case LIBUSB_TRANSFER_OVERFLOW: return -EOVERFLOW;
    case LIBUSB_TRANSFER_CANCELLED: return -ECONNRESET;
    default: return -EPROTO;
    }
}

static void done(struct libusb_transfer *t)
{
    struct req *q = t->user_data;
    struct conn *c = q->c;
    int32_t st = status_of(t->status);
    if (t->status == LIBUSB_TRANSFER_NO_DEVICE) c->run = 0;
    if (st && st != -ECONNRESET) {
        static int told;
        if (told++ < 40) LOG("%s ep %02x done with %d, %d bytes", c->d->busid, t->endpoint, st, t->actual_length);
    }
    if (!q->unlinked) {
        const uint8_t *data = q->dir ? (q->ctl ? q->buf + LIBUSB_CONTROL_SETUP_SIZE : q->buf) : NULL;
        send_ret(c, q->seq, st, (uint32_t)t->actual_length, data);
    }
    pthread_mutex_lock(&c->lock);
    libusb_free_transfer(t);
    free(q->buf);
    q->buf = NULL; q->t = NULL; q->used = 0;
    c->pending--;
    pthread_mutex_unlock(&c->lock);
}

static void serve_urbs(struct conn *c)
{
    struct dev *d = c->d;
    c->run = 1;
    while (c->run) {
        uint8_t h[48];
        if (rd_all(c->s, h, 48)) break;
        uint32_t cmd = get32(h), seq = get32(h + 4), dir = get32(h + 12), ep = get32(h + 16);
        if (cmd == CMD_SUBMIT) {
            uint32_t flags = get32(h + 20), len = get32(h + 24), npk = get32(h + 32);
            uint8_t *buf = malloc((size_t)len + LIBUSB_CONTROL_SETUP_SIZE + 8);
            if (!buf) break;
            uint8_t *data = buf + (ep == 0 ? LIBUSB_CONTROL_SETUP_SIZE : 0);
            if (dir == 0 && len && rd_all(c->s, data, len)) { free(buf); break; }
            if (npk != 0 && npk != 0xffffffffu) {   /* isochronous: not yet */
                uint8_t skip[16];
                for (uint32_t i = 0; i < npk; i++) if (rd_all(c->s, skip, 16)) break;
                send_ret(c, seq, -EPROTO, 0, NULL); free(buf); continue;
            }
            if (ep == 0) {
                memcpy(buf, h + 40, 8);
                uint8_t rt = buf[0], rq = buf[1];
                uint16_t wv = (uint16_t)(buf[2] | buf[3] << 8), wi = (uint16_t)(buf[4] | buf[5] << 8);
                LOG("%s ctl %02x %02x %04x %04x len %u", d->busid, rt, rq, wv, wi, len);
                /* As in usbipd.c: the host's stack owns addresses, configuration and link power. */
                if ((rq == 0x01 || rq == 0x03) && ((rt == 0x00 && (wv == 48 || wv == 49 || wv == 50)) || (rt == 0x01 && wv == 0))) {
                    send_ret(c, seq, 0, 0, NULL); free(buf); continue;
                }
                if (rt == 0x00 && (rq == 0x05 || rq == 0x09 || rq == 0x30 || rq == 0x31)) { send_ret(c, seq, 0, 0, NULL); free(buf); continue; }
                if (rt == 0x01 && rq == 0x0b) {
                    int r = libusb_set_interface_alt_setting(d->h, wi, wv);
                    if (r) LOG("%s set interface %u alt %u: %s", d->busid, wi, wv, libusb_error_name(r));
                    send_ret(c, seq, r ? -EPIPE : 0, 0, NULL); free(buf); continue;
                }
                if (rt == 0x02 && rq == 0x01 && wv == 0) {   /* CLEAR_FEATURE ENDPOINT_HALT */
                    int r = libusb_clear_halt(d->h, (unsigned char)wi);
                    send_ret(c, seq, r ? -EPIPE : 0, 0, NULL); free(buf); continue;
                }
            }
            pthread_mutex_lock(&c->lock);
            struct req *q = NULL;
            for (int i = 0; i < MAX_REQ; i++) if (!c->reqs[i].used) { q = &c->reqs[i]; break; }
            if (q) { memset(q, 0, sizeof *q); q->used = 1; c->pending++; }
            pthread_mutex_unlock(&c->lock);
            if (!q) { free(buf); break; }
            q->c = c; q->seq = seq; q->dir = (uint8_t)dir; q->buf = buf; q->ctl = ep == 0;
            struct libusb_transfer *t = libusb_alloc_transfer(0);
            q->t = t;
            if (ep == 0) {
                libusb_fill_control_transfer(t, d->h, buf, done, q, 5000);
            } else {
                uint8_t addr = (uint8_t)(ep | (dir ? 0x80 : 0));
                if (ep_type(d, addr) == 3) libusb_fill_interrupt_transfer(t, d->h, addr, buf, (int)len, done, q, 0);
                else libusb_fill_bulk_transfer(t, d->h, addr, buf, (int)len, done, q, 0);
                if (flags & 0x1) t->flags |= LIBUSB_TRANSFER_SHORT_NOT_OK;
                if (flags & 0x40) t->flags |= LIBUSB_TRANSFER_ADD_ZERO_PACKET;
            }
            int r = libusb_submit_transfer(t);
            if (r) {
                static int told;
                if (told++ < 20) LOG("%s submit ep %02x len %u: %s", d->busid, t->endpoint, len, libusb_error_name(r));
                send_ret(c, seq, r == LIBUSB_ERROR_NO_DEVICE ? -ENODEV : -EPIPE, 0, NULL);
                pthread_mutex_lock(&c->lock);
                libusb_free_transfer(t); free(buf); q->buf = NULL; q->t = NULL; q->used = 0; c->pending--;
                pthread_mutex_unlock(&c->lock);
            }
        } else if (cmd == CMD_UNLINK) {
            uint32_t victim = get32(h + 20);
            int32_t st = 0;
            pthread_mutex_lock(&c->lock);
            for (int i = 0; i < MAX_REQ; i++)
                if (c->reqs[i].used && c->reqs[i].seq == victim && c->reqs[i].t) {
                    c->reqs[i].unlinked = 1;
                    if (!libusb_cancel_transfer(c->reqs[i].t)) st = -ECONNRESET;
                    break;
                }
            uint8_t r[48];
            memset(r, 0, sizeof r);
            put32(r, RET_UNLINK); put32(r + 4, seq); put32(r + 20, (uint32_t)st);
            wr_all(c->s, r, 48);
            pthread_mutex_unlock(&c->lock);
        } else break;
    }
    c->run = 0;
    pthread_mutex_lock(&c->lock);
    for (int i = 0; i < MAX_REQ; i++) if (c->reqs[i].used && c->reqs[i].t) { c->reqs[i].unlinked = 1; libusb_cancel_transfer(c->reqs[i].t); }
    pthread_mutex_unlock(&c->lock);
    for (int i = 0; i < 300 && c->pending; i++) usleep(10000);
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
            uint8_t out[12], rec[312];
            op_reply(out, OP_REP_DEVLIST, 0);
            put32(out + 8, (uint32_t)g_ndev);
            wr_all(s, out, 12);
            for (int i = 0; i < g_ndev; i++) {
                fill_dev(rec, &g_dev[i]);
                wr_all(s, rec, 312);
                uint8_t ic[3 * 32], ni = (uint8_t)ninterfaces(&g_dev[i], ic, 32);
                for (int k = 0; k < ni; k++) { uint8_t r4[4] = {ic[3 * k], ic[3 * k + 1], ic[3 * k + 2], 0}; wr_all(s, r4, 4); }
            }
        } else if (code == OP_REQ_IMPORT) {
            char busid[32];
            if (rd_all(s, busid, 32)) break;
            busid[31] = 0;
            struct dev *d = NULL;
            pthread_mutex_lock(&g_lock);
            for (int i = 0; i < g_ndev; i++) if (!strcmp(g_dev[i].busid, busid) && !g_dev[i].in_use) d = &g_dev[i];
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
            break;
        } else break;
    }
    close(s);
    return NULL;
}

static void *events(void *arg)
{
    (void)arg;
    for (;;) libusb_handle_events(NULL);
    return NULL;
}

static int wanted(uint16_t vid, uint16_t pid, int argc, char **argv)
{
    if (argc < 2) return 1;
    for (int i = 1; i < argc; i++) {
        unsigned v, p;
        if (sscanf(argv[i], "%x:%x", &v, &p) == 2 && v == vid && p == pid) return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    if (libusb_init(NULL)) { LOG("libusb_init failed"); return 1; }
    libusb_device **list;
    ssize_t n = libusb_get_device_list(NULL, &list);
    for (ssize_t i = 0; i < n && g_ndev < MAX_DEV; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) || dd.bDeviceClass == 9) continue;   /* hubs stay */
        if (!wanted(dd.idVendor, dd.idProduct, argc, argv)) continue;
        struct dev *d = &g_dev[g_ndev];
        d->ud = libusb_ref_device(list[i]);
        int r = libusb_open(d->ud, &d->h);
        if (r) { LOG("%04x:%04x open: %s", dd.idVendor, dd.idProduct, libusb_error_name(r)); continue; }
        libusb_set_auto_detach_kernel_driver(d->h, 1);
        d->vid = dd.idVendor; d->pid = dd.idProduct;
        d->busnum = libusb_get_bus_number(d->ud);
        d->devnum = libusb_get_device_address(d->ud);
        snprintf(d->busid, sizeof d->busid, "%d-%d", d->busnum, d->devnum);
        memcpy(d->desc, &dd, 18);   /* the struct is the descriptor's own layout, little endian */
        int cl = libusb_get_descriptor(d->h, LIBUSB_DT_CONFIG, 0, d->desc + 18, (int)sizeof d->desc - 18);
        d->desc_len = 18 + (cl > 0 ? (uint32_t)cl : 0);
        int sp = libusb_get_device_speed(d->ud);
        d->speed = sp == LIBUSB_SPEED_SUPER ? 5 : sp == LIBUSB_SPEED_SUPER_PLUS ? 6 : (sp > 0 ? (uint32_t)sp : 3);
        struct libusb_config_descriptor *cfg;
        if (!libusb_get_active_config_descriptor(d->ud, &cfg)) {
            for (int k = 0; k < cfg->bNumInterfaces; k++) {
                r = libusb_claim_interface(d->h, k);
                if (r) LOG("%s interface %d: %s", d->busid, k, libusb_error_name(r));
            }
            libusb_free_config_descriptor(cfg);
        }
        LOG("%s %04x:%04x speed %u offered", d->busid, d->vid, d->pid, d->speed);
        g_ndev++;
    }
    libusb_free_device_list(list, 1);
    if (!g_ndev) { LOG("no devices to export"); return 1; }
    pthread_t ev;
    pthread_create(&ev, NULL, events, NULL);
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(3240), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 4)) { LOG("listen: %s", strerror(errno)); return 1; }
    LOG("usbipd-mac on port 3240");
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        pthread_t th;
        pthread_create(&th, NULL, client, (void *)(intptr_t)c);
        pthread_detach(th);
    }
}
