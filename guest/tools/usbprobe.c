/* usbprobe: the guest side of the USB ring, as a plain program (usb/README.md, phase 2 test).
 *   usbprobe            find an offered device, print its descriptors, and if it is a flash drive
 *                       (mass storage, bulk-only) read its first sector and time a sequential read
 *   usbprobe <MiB>      read that much (default 64)
 * Run as SYSTEM (the agent does). The region is reached through \\.\HwUsbRaw (hwusbraw.sys, IOCTL_HWUSBRAW_IO: reads and writes by
 * copy; Windows would not map more than a megabyte or two of it into a program). Bulk data never comes to this
 * program: it stays in the arena, where the phone's usbfs put it, so the throughput is the ring's and the phone's.
 * Build: aarch64-w64-mingw32-clang -O2 guest/tools/usbprobe.c -Iinclude -o build/usbprobe.exe */
#include <windows.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw_usb.h"
#include "hwusbraw_ioctl.h"

static HANDLE g_h;
static uint64_t g_slot_off;   /* of the device's slot in the region */
static uint32_t g_seq = 1;

static int io(uint64_t off, void *data, uint32_t len, int write)
{
    uint8_t buf[sizeof(struct hwusbraw_io) + HWUSBRAW_IO_MAX];
    struct hwusbraw_io h = {off, len, (uint32_t)write};
    DWORD n = 0;
    memcpy(buf, &h, sizeof h);
    if (write) memcpy(buf + sizeof h, data, len);
    if (!DeviceIoControl(g_h, IOCTL_HWUSBRAW_IO, buf, (DWORD)(sizeof h + (write ? len : 0)), buf, write ? 0 : len, &n, NULL)) return -1;
    if (!write) memcpy(data, buf, len);
    return 0;
}
static uint32_t rd32(uint64_t off) { uint32_t v = 0; io(off, &v, 4, 0); return v; }
static void wr32(uint64_t off, uint32_t v) { io(off, &v, 4, 1); }
static void rd(uint64_t off, void *p, uint32_t len) { uint8_t *b = p; while (len) { uint32_t n = len > HWUSBRAW_IO_MAX ? HWUSBRAW_IO_MAX : len; io(off, b, n, 0); off += n; b += n; len -= n; } }
static void wr(uint64_t off, const void *p, uint32_t len) { const uint8_t *b = p; while (len) { uint32_t n = len > HWUSBRAW_IO_MAX ? HWUSBRAW_IO_MAX : len; io(off, (void *)b, n, 1); off += n; b += n; len -= n; } }

#define CTRL(f) (g_slot_off + HW_USB_CTRL_OFF + offsetof(struct hw_usb_ctrl, f))
#define ARENA(o) (g_slot_off + HW_USB_ARENA_OFF + (o))

static int submit(struct hw_usb_sub *s, struct hw_usb_cmp *c, DWORD timeout_ms)
{
    s->seqnum = g_seq++;
    uint32_t head = rd32(CTRL(sub_head));
    DWORD t0 = GetTickCount();
    while (head - rd32(CTRL(sub_tail)) >= HW_USB_RING_ENTRIES) if (GetTickCount() - t0 > timeout_ms) return -1;
    wr(g_slot_off + HW_USB_SUB_OFF + (uint64_t)(head % HW_USB_RING_ENTRIES) * HW_USB_ENTRY_SIZE, s, sizeof *s);
    wr32(CTRL(sub_head), head + 1);
    for (;;) {
        uint32_t tail = rd32(CTRL(cmp_tail));
        if (rd32(CTRL(cmp_head)) != tail) {
            rd(g_slot_off + HW_USB_CMP_OFF + (uint64_t)(tail % HW_USB_RING_ENTRIES) * HW_USB_ENTRY_SIZE, c, sizeof *c);
            wr32(CTRL(cmp_tail), tail + 1);
            if (c->seqnum == s->seqnum) return 0;
            continue;
        }
        if (GetTickCount() - t0 > timeout_ms) return -1;
    }
}

static int control(uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len, uint32_t off, struct hw_usb_cmp *c)
{
    struct hw_usb_sub s;
    memset(&s, 0, sizeof s);
    s.op = HW_USB_OP_CTRL;
    s.dir = (type & 0x80) ? HW_USB_DIR_IN : HW_USB_DIR_OUT;
    s.length = len; s.arena_off = len ? off : 0;
    s.setup[0] = type; s.setup[1] = req;
    s.setup[2] = (uint8_t)value; s.setup[3] = (uint8_t)(value >> 8);
    s.setup[4] = (uint8_t)index; s.setup[5] = (uint8_t)(index >> 8);
    s.setup[6] = (uint8_t)len; s.setup[7] = (uint8_t)(len >> 8);
    return submit(&s, c, 5000);
}

static int bulk(uint8_t ep, int in, uint32_t len, uint32_t off, struct hw_usb_cmp *c)
{
    struct hw_usb_sub s;
    memset(&s, 0, sizeof s);
    s.op = HW_USB_OP_BULK; s.dir = in ? HW_USB_DIR_IN : HW_USB_DIR_OUT; s.ep = ep & 0xf;
    s.length = len; s.arena_off = off;
    return submit(&s, c, 10000);
}

static uint8_t g_bin, g_bout;
/* One bulk-only transport command. The data stage lands at arena offset data_off. */
static int bot(const uint8_t *cdb, uint8_t cdblen, int in, uint32_t data_len, uint32_t data_off, uint32_t *got)
{
    static uint32_t tag = 0x1000;
    uint32_t cbw_off = 0, csw_off = 64;
    uint8_t cbw[31] = {'U', 'S', 'B', 'C'};
    uint32_t t = ++tag;
    memcpy(cbw + 4, &t, 4); memcpy(cbw + 8, &data_len, 4);
    cbw[12] = in ? 0x80 : 0; cbw[14] = cdblen; memcpy(cbw + 15, cdb, cdblen);
    wr(ARENA(cbw_off), cbw, 31);
    struct hw_usb_cmp c;
    if (bulk(g_bout, 0, 31, cbw_off, &c) || c.status) { printf("CBW failed %d\n", c.status); return -1; }
    uint32_t done = 0;
    if (data_len) {
        if (bulk(g_bin, in, data_len, data_off, &c)) { printf("data stage timed out\n"); return -1; }
        done = c.actual_length;
        if (c.status && c.status != -EPIPE) { printf("data stage status %d\n", c.status); return -1; }
    }
    if (bulk(g_bin, 1, 13, csw_off, &c) || c.status) { printf("CSW failed %d\n", c.status); return -1; }
    uint8_t csw[13];
    rd(ARENA(csw_off), csw, 13);
    if (memcmp(csw, "USBS", 4)) { printf("bad CSW signature\n"); return -1; }
    if (got) *got = done;
    return csw[12];   /* 0 = command passed */
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    unsigned mib = argc > 1 ? (unsigned)atoi(argv[1]) : 64;
    g_h = CreateFileA("\\\\.\\HwUsbRaw", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (g_h == INVALID_HANDLE_VALUE) { printf("cannot open \\\\.\\HwUsbRaw (install it: C:\\HyperWin\\gpu\\usb\\install-usb.cmd): %lu\n", GetLastError()); return 2; }
    struct hw_usb_global gl;
    if (io(0, &gl, 256, 0)) { printf("USB region read failed: %lu (SYSTEM only; driver and app up to date?)\n", GetLastError()); return 3; }
    printf("magic %.8s version %u host_state %u slots %u stride %u\n", gl.magic, gl.version, gl.host_state, gl.slots, gl.slot_stride);
    if (memcmp(gl.magic, HW_USB_MAGIC, 8)) { printf("no USB region (old app?)\n"); return 4; }
    int slot = -1;
    struct hw_usb_dev_entry d;
    printf("waiting for a device (plug one in; passthrough on: am broadcast -a dev.hyperwin.app.USB_PASS --ez enable true)...\n");
    for (int i = 0; i < 1200 && slot < 0; i++) {
        for (unsigned k = 0; k < HW_USB_SLOTS; k++)
            if (rd32(HW_USB_GLOBAL_SIZE ? offsetof(struct hw_usb_global, dev) + (uint64_t)k * sizeof(struct hw_usb_dev_entry) : 0) == HW_USB_OFFERED) { slot = (int)k; break; }
        if (slot < 0) Sleep(250);
    }
    if (slot < 0) { printf("no device offered in 5 minutes\n"); return 5; }
    uint64_t dev_off = offsetof(struct hw_usb_global, dev) + (uint64_t)slot * sizeof(struct hw_usb_dev_entry);
    rd(dev_off, &d, sizeof d);
    printf("slot %d: %04x:%04x speed %u bus %u, %u descriptor bytes\n", slot, d.vendor, d.product, d.speed, d.bus_id, d.desc_len);
    g_slot_off = HW_USB_SLOT_OFF(slot);
    wr32(dev_off + offsetof(struct hw_usb_dev_entry, state), HW_USB_ATTACHED);
    struct hw_usb_cmp c;
    uint8_t dd[18];
    if (control(0x80, 6, 0x0100, 0, 18, 0, &c) || c.status || c.actual_length != 18) { printf("GET_DESCRIPTOR failed (%d)\n", c.status); return 6; }
    rd(ARENA(0), dd, 18);
    printf("device descriptor via ring: USB %u.%02x class %02x vendor %04x product %04x\n", dd[3], dd[2], dd[4], dd[8] | dd[9] << 8, dd[10] | dd[11] << 8);
    const uint8_t *p = d.desc + 18, *end = d.desc + d.desc_len;
    int msc = 0;
    while (p + 2 <= end && p[0]) {
        if (p[1] == 4 && p[5] == 8 && p[6] == 6 && p[7] == 0x50) msc = 1;
        else if (p[1] == 4) msc = 0;
        if (msc && p[1] == 5) { if (p[2] & 0x80) g_bin = p[2] & 0xf; else g_bout = p[2] & 0xf; }
        p += p[0];
    }
    if (!g_bin || !g_bout) { printf("not a bulk-only flash drive: descriptors fine, nothing more to test\n"); return 0; }
    printf("mass storage, bulk in %u out %u\n", g_bin, g_bout);
    uint32_t got = 0;
    uint8_t buf[512];
    uint8_t inq[6] = {0x12, 0, 0, 0, 36, 0};
    if (bot(inq, 6, 1, 36, 4096, &got) == 0) { rd(ARENA(4096), buf, 36); printf("INQUIRY: %.8s %.16s\n", buf + 8, buf + 16); }
    uint8_t tur[6] = {0};
    for (int i = 0; i < 20 && bot(tur, 6, 0, 0, 0, NULL); i++) Sleep(200);
    uint8_t cap[10] = {0x25};
    uint32_t last = 0, bs = 512;
    if (bot(cap, 10, 1, 8, 4096, &got) == 0 && got == 8) {
        rd(ARENA(4096), buf, 8);
        last = buf[0] << 24 | buf[1] << 16 | buf[2] << 8 | buf[3];
        bs = buf[4] << 24 | buf[5] << 16 | buf[6] << 8 | buf[7];
        printf("capacity: %u blocks of %u bytes (%.1f MiB)\n", last + 1, bs, (double)(last + 1) * bs / 1048576.0);
    }
    uint8_t rdc[10] = {0x28};
    rdc[8] = 1;
    if (bs <= 512 && bot(rdc, 10, 1, bs, 4096, &got) == 0) { rd(ARENA(4096), buf, 512); printf("sector 0: %u bytes, signature %02x%02x (55aa is a partition table)\n", got, buf[510], buf[511]); }
    /* sequential read, 1 MiB at a time, straight into the arena (nothing comes to this program) */
    uint32_t per = (1u << 20) / bs, total = mib * (1u << 20) / bs;
    if (total > last + 1) total = last + 1;
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    uint64_t bytes = 0;
    for (uint32_t lba = 0; lba + per <= total; lba += per) {
        rdc[2] = lba >> 24; rdc[3] = lba >> 16; rdc[4] = lba >> 8; rdc[5] = lba;
        rdc[7] = per >> 8; rdc[8] = per;
        if (bot(rdc, 10, 1, per * bs, 4096, &got)) { printf("read failed at %u\n", lba); break; }
        bytes += got;
    }
    QueryPerformanceCounter(&t1);
    double secs = (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
    printf("read %.1f MiB in %.2f s: %.1f MB/s\n", bytes / 1048576.0, secs, bytes / 1e6 / secs);
    return 0;
}
