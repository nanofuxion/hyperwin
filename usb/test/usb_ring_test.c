/* Host-only test of the USB ring (usb/README.md, phase 1): a fake guest thread and a loopback device behind the
 * engine, on any Mac or Linux: cc -std=c11 -O1 -pthread usb_ring_test.c ../host/usb_engine.c -o usb_ring_test */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../host/usb_engine.h"

static void *g_region;
static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the loopback device: control GET_DESCRIPTOR, bulk OUT 1 into a fifo, bulk IN 1 out of it ---- */
#define FIFO (1u << 22)
static const uint8_t k_dev_desc[18] = {18, 1, 0, 2, 0, 0, 0, 64, 0x34, 0x12, 0x78, 0x56, 0, 1, 1, 2, 3, 1};
struct loop {
    uint8_t *fifo;
    uint32_t in, out;
    struct { uint32_t seq, off, len; uint8_t op; int live; } wait[8];   /* IN requests waiting for data */
    struct usb_done ready[8]; int nready;
};
static int loop_submit(void *dev, const struct hw_usb_sub *s, uint8_t *arena, struct usb_done *d)
{
    struct loop *l = dev;
    if (s->op == HW_USB_OP_CTRL) {
        if (s->setup[0] == 0x80 && s->setup[1] == 6 && s->setup[3] == 1) {
            uint32_t n = s->length < 18 ? s->length : 18;
            memcpy(arena + s->arena_off, k_dev_desc, n);
            d->actual_length = n;
            return 0;
        }
        return -EPIPE;   /* a stall */
    }
    if (s->op == HW_USB_OP_BULK && s->ep == 1 && s->dir == HW_USB_DIR_OUT) {
        for (uint32_t i = 0; i < s->length; i++) l->fifo[l->in++ % FIFO] = arena[s->arena_off + i];
        d->actual_length = s->length;
        return 0;
    }
    if (s->op == HW_USB_OP_BULK && s->ep == 1 && s->dir == HW_USB_DIR_IN) {
        if (l->in - l->out >= s->length) {
            for (uint32_t i = 0; i < s->length; i++) arena[s->arena_off + i] = l->fifo[l->out++ % FIFO];
            d->actual_length = s->length;
            return 0;
        }
        for (int i = 0; i < 8; i++) if (!l->wait[i].live) { l->wait[i].live = 1; l->wait[i].seq = s->seqnum; l->wait[i].off = s->arena_off; l->wait[i].len = s->length; l->wait[i].op = s->op; return 1; }
        return -ENOMEM;
    }
    return -ENOTSUP;
}
static int loop_reap(void *dev, struct usb_done *d)
{
    struct loop *l = dev;
    if (l->nready) { *d = l->ready[--l->nready]; return 1; }
    for (int i = 0; i < 8; i++) {
        if (!l->wait[i].live || l->in - l->out < l->wait[i].len) continue;
        uint8_t *arena = hw_usb_slot_at(g_region, 0).arena;
        for (uint32_t k = 0; k < l->wait[i].len; k++) arena[l->wait[i].off + k] = l->fifo[l->out++ % FIFO];
        memset(d, 0, sizeof *d);
        d->seqnum = l->wait[i].seq; d->op = l->wait[i].op; d->actual_length = l->wait[i].len;
        l->wait[i].live = 0;
        return 1;
    }
    return 0;
}
static void loop_cancel(void *dev, uint32_t seq)
{
    struct loop *l = dev;
    for (int i = 0; i < 8; i++) if (l->wait[i].live && l->wait[i].seq == seq) {
        l->wait[i].live = 0;
        memset(&l->ready[l->nready], 0, sizeof l->ready[0]);
        l->ready[l->nready].seqnum = seq; l->ready[l->nready].op = l->wait[i].op; l->ready[l->nready].status = -ECONNRESET;
        l->nready++;
    }
}
static void loop_cancel_all(void *dev)
{
    struct loop *l = dev;
    for (int i = 0; i < 8; i++) if (l->wait[i].live) {
        l->wait[i].live = 0;
        memset(&l->ready[l->nready], 0, sizeof l->ready[0]);
        l->ready[l->nready].seqnum = l->wait[i].seq; l->ready[l->nready].op = l->wait[i].op; l->ready[l->nready].status = -ENODEV;
        l->nready++;
    }
}
static const struct usb_backend k_loop = {loop_submit, loop_reap, loop_cancel, loop_cancel_all};

/* ---- the fake guest ---- */
static uint32_t g_seq = 1;
static struct hw_usb_slot gs;
static void gsubmit(struct hw_usb_sub *s) { s->seqnum = g_seq++; while (hw_usb_submit_push(&gs, s)) sched_yield(); }
static void gwait(struct hw_usb_cmp *c) { while (hw_usb_cmp_pop(&gs, c)) sched_yield(); if (getenv("USBT")) printf("  cmp seq %u op %u status %d len %u\n", c->seqnum, c->op, c->status, c->actual_length); }

static void *guest(void *arg)
{
    (void)arg;
    struct hw_usb_global *g = g_region;
    while (__atomic_load_n(&g->dev[0].state, __ATOMIC_ACQUIRE) != HW_USB_OFFERED) sched_yield();
    CHECK(g->dev[0].vendor == 0x1234 && g->dev[0].product == 0x5678 && g->dev[0].speed == HW_USB_HIGH, "table entry");
    CHECK(g->dev[0].desc_len == 18 && !memcmp(g->dev[0].desc, k_dev_desc, 18), "descriptors published");
    __atomic_store_n(&g->dev[0].state, HW_USB_ATTACHED, __ATOMIC_RELEASE);
    gs = hw_usb_slot_at(g_region, 0);
    struct hw_usb_sub s;
    struct hw_usb_cmp c;
    /* control: GET_DESCRIPTOR(device) */
    memset(&s, 0, sizeof s);
    s.op = HW_USB_OP_CTRL; s.dir = HW_USB_DIR_IN; s.length = 18; s.arena_off = 0;
    s.setup[0] = 0x80; s.setup[1] = 6; s.setup[3] = 1; s.setup[6] = 18;
    gsubmit(&s); gwait(&c);
    CHECK(c.seqnum == 1 && c.status == 0 && c.actual_length == 18 && !memcmp(gs.arena, k_dev_desc, 18), "control read");
    /* control: an unknown request stalls */
    s.setup[1] = 99; gsubmit(&s); gwait(&c);
    CHECK(c.status == -EPIPE, "stall reported as %d", c.status);
    /* bulk OUT then IN, 1 MiB in 64 KiB pieces, data checked */
    uint32_t total = 1u << 20, piece = 64u << 10;
    for (uint32_t off = 0; off < total; off += piece) {
        for (uint32_t i = 0; i < piece; i++) gs.arena[HW_USB_PAGE + i] = (uint8_t)((off + i) * 2654435761u >> 24);
        memset(&s, 0, sizeof s);
        s.op = HW_USB_OP_BULK; s.dir = HW_USB_DIR_OUT; s.ep = 1; s.length = piece; s.arena_off = HW_USB_PAGE;
        gsubmit(&s); gwait(&c);
        CHECK(c.status == 0 && c.actual_length == piece, "bulk out %d %u", c.status, c.actual_length);
    }
    for (uint32_t off = 0; off < total; off += piece) {
        memset(gs.arena + HW_USB_PAGE, 0, piece);
        memset(&s, 0, sizeof s);
        s.op = HW_USB_OP_BULK; s.dir = HW_USB_DIR_IN; s.ep = 1; s.length = piece; s.arena_off = HW_USB_PAGE;
        gsubmit(&s); gwait(&c);
        int ok = c.status == 0 && c.actual_length == piece;
        for (uint32_t i = 0; ok && i < piece; i++) if (gs.arena[HW_USB_PAGE + i] != (uint8_t)((off + i) * 2654435761u >> 24)) { ok = 0; CHECK(0, "data differs at %u", off + i); }
        CHECK(ok, "bulk in %d", c.status);
    }
    /* an IN with nothing to read waits; data from a later OUT completes it */
    memset(&s, 0, sizeof s);
    s.op = HW_USB_OP_BULK; s.dir = HW_USB_DIR_IN; s.ep = 1; s.length = 512; s.arena_off = 2 * HW_USB_PAGE;
    gsubmit(&s);
    uint32_t waiting = s.seqnum;
    memset(&s, 0, sizeof s);
    s.op = HW_USB_OP_BULK; s.dir = HW_USB_DIR_OUT; s.ep = 1; s.length = 512; s.arena_off = HW_USB_PAGE;
    memset(gs.arena + HW_USB_PAGE, 0x5a, 512);
    gsubmit(&s);
    int seen_in = 0, seen_out = 0;
    for (int i = 0; i < 2; i++) { gwait(&c); if (c.seqnum == waiting) { seen_in = 1; CHECK(c.actual_length == 512 && gs.arena[2 * HW_USB_PAGE] == 0x5a, "late in"); } else seen_out = 1; }
    CHECK(seen_in && seen_out, "both completed");
    /* an IN that is cancelled */
    memset(&s, 0, sizeof s);
    s.op = HW_USB_OP_BULK; s.dir = HW_USB_DIR_IN; s.ep = 1; s.length = 4096; s.arena_off = 2 * HW_USB_PAGE;
    gsubmit(&s);
    waiting = s.seqnum;
    memset(&s, 0, sizeof s);
    s.op = HW_USB_OP_UNLINK; s.unlink_seq = waiting;
    gsubmit(&s);
    int got_unlink = 0, got_reset = 0;
    for (int i = 0; i < 2; i++) { gwait(&c); if (c.op == HW_USB_OP_UNLINK) got_unlink = 1; else if (c.seqnum == waiting && c.status == -ECONNRESET) got_reset = 1; }
    CHECK(got_unlink && got_reset, "unlink %d reset %d", got_unlink, got_reset);
    /* a request past the arena is refused */
    memset(&s, 0, sizeof s);
    s.op = HW_USB_OP_BULK; s.dir = HW_USB_DIR_OUT; s.ep = 1; s.length = 4096; s.arena_off = (uint32_t)HW_USB_ARENA_SIZE - 100;
    gsubmit(&s); gwait(&c);
    CHECK(c.status == -EINVAL, "bounds %d", c.status);
    /* a request left waiting when the device goes fails with ENODEV */
    memset(&s, 0, sizeof s);
    s.op = HW_USB_OP_BULK; s.dir = HW_USB_DIR_IN; s.ep = 1; s.length = 100; s.arena_off = 2 * HW_USB_PAGE;
    gsubmit(&s);
    waiting = s.seqnum;
    while (__atomic_load_n(&g->dev[0].state, __ATOMIC_ACQUIRE) != HW_USB_DETACHING) sched_yield();
    gwait(&c);
    CHECK(c.seqnum == waiting && c.status == -ENODEV, "detach completes with %d", c.status);
    __atomic_store_n(&g->dev[0].state, HW_USB_FREE, __ATOMIC_RELEASE);
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    alarm(60);   /* a stuck ring must fail the test, not hang it */
    g_region = mmap(NULL, HW_USB_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (g_region == MAP_FAILED) { perror("mmap"); return 2; }
    usb_region_init(g_region);
    CHECK(sizeof(struct hw_usb_ctrl) <= HW_USB_CTRL_SIZE, "ctrl size");
    struct loop *l = calloc(1, sizeof *l);
    l->fifo = malloc(FIFO);
    struct usb_engine e;
    usb_engine_init(&e, g_region, 0, &k_loop, l);
    pthread_t t;
    pthread_create(&t, NULL, guest, NULL);
    CHECK(usb_table_offer(g_region, 0, 7, 0x1234, 0x5678, HW_USB_HIGH, k_dev_desc, sizeof k_dev_desc) == 0, "offer");
    struct hw_usb_global *g = g_region;
    int idle = 0;
    for (long spin = 0; spin < 200000000L; spin++) {
        if (usb_engine_poll(&e, 16)) idle = 0; else idle++;
        /* when the guest has no more to do it waits for the retire: the last IN is the one left waiting */
        if (idle > 2000000 && e.inflight == 1 && g->dev[0].state == HW_USB_ATTACHED && e.submits >= 40) {
            usb_table_retire(g_region, 0);
            usb_engine_fail_all(&e);
            break;
        }
    }
    pthread_join(t, NULL);
    CHECK(g->dev[0].state == HW_USB_FREE, "slot freed");
    printf("%s: %llu requests, %llu completions, %llu bytes out\n", g_fail ? "FAIL" : "PASS",
           (unsigned long long)e.submits, (unsigned long long)e.completions,
           (unsigned long long)hw_usb_slot_at(g_region, 0).ctrl->bytes_out);
    return g_fail ? 1 : 0;
}
