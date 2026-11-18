#include "hostfile.h"
#include "ring.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void die(const char *what)
{
    fprintf(stderr, "%s: %s\n", what, strerror(errno));
    exit(1);
}

static void *map_window(const char *path, int *fd, off_t offset)
{
    struct stat st;
    void *p;
    *fd = open(path, O_RDWR);
    if (*fd < 0) die(path);
    if (fstat(*fd, &st) != 0) die("fstat");
    if ((unsigned long long)st.st_size < (unsigned long long)offset + HW_WINDOW_SIZE) {
        fprintf(stderr, "%s is %lld bytes, need %lld\n", path, (long long)st.st_size,
                (long long)offset + HW_WINDOW_SIZE);
        exit(1);
    }
    p = mmap(NULL, HW_WINDOW_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, offset);
    if (p == MAP_FAILED) die("mmap");
    return p;
}

static int cmd_init(const char *path, off_t offset, off_t span)
{
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    void *p;
    if (fd < 0) die(path);
    if (span < offset + (off_t)HW_WINDOW_SIZE) span = offset + (off_t)HW_WINDOW_SIZE;
    if (ftruncate(fd, span) != 0) die("ftruncate");
    p = mmap(NULL, HW_WINDOW_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
    if (p == MAP_FAILED) die("mmap");
    hw_window_init(p, HW_WINDOW_SIZE);
    __atomic_store_n(&((struct hw_header *)p)->tpm_status, 1, __ATOMIC_RELEASE);
    msync(p, HW_WINDOW_SIZE, MS_SYNC);
    munmap(p, HW_WINDOW_SIZE);
    close(fd);
    printf("window %s offset %lld (%u bytes)\n", path, (long long)offset, HW_WINDOW_SIZE);
    return 0;
}

static int write_ppm(const char *path, const uint8_t *bgra, uint32_t w, uint32_t h, uint32_t stride)
{
    FILE *f = fopen(path, "wb");
    uint32_t x, y;
    if (!f) return -1;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (y = 0; y < h; y++) {
        const uint8_t *row = bgra + (size_t)y * stride;
        for (x = 0; x < w; x++) {
            uint8_t rgb[3] = {row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0]};
            if (fwrite(rgb, 1, 3, f) != 3) {
                fclose(f);
                return -1;
            }
        }
    }
    fclose(f);
    return 0;
}

static int cmd_run(const char *path, const char *ppm, int timeout_s, off_t offset)
{
    int fd;
    void *window = map_window(path, &fd, offset);
    struct hw_header *h = window;
    struct hw_tpm tpm;
    uint8_t seed[32];
    uint8_t *frame;
    time_t start = time(NULL);
    uint32_t last = 0;
    memset(seed, 0xA5, sizeof seed);
    hw_tpm_init(&tpm, seed);
    __atomic_store_n(&h->tpm_status, 1, __ATOMIC_RELEASE);
    frame = malloc(HW_WIDTH * HW_HEIGHT * 4);
    if (!frame) die("malloc");
    printf("serving %s\n", path);
    for (;;) {
        uint32_t flags, seq;
        hw_tpm_poll(window, &tpm);
        seq = __atomic_load_n(&h->seq, __ATOMIC_ACQUIRE);
        flags = __atomic_load_n(&h->guest_flags, __ATOMIC_ACQUIRE);
        if (seq != last && hw_copy_front(window, frame, HW_WIDTH * HW_HEIGHT * 4) > 0) {
            last = seq;
            if (ppm && write_ppm(ppm, frame, h->width, h->height, h->stride) == 0)
                printf("frame seq %u -> %s\n", seq, ppm);
        }
        if ((flags & (HW_GUEST_FOUND | HW_GUEST_FRAME | HW_GUEST_TPM)) ==
            (HW_GUEST_FOUND | HW_GUEST_FRAME | HW_GUEST_TPM)) {
            printf("guest flags 0x%x tpm_rc 0x%x pci %04x:%04x seq %u\n",
                   flags, h->tpm_rc, h->pci_vendor, h->pci_device, seq);
            free(frame);
            munmap(window, HW_WINDOW_SIZE);
            close(fd);
            return 0;
        }
        if (time(NULL) - start > timeout_s) {
            printf("timeout flags 0x%x seq %u tpm_rc 0x%x req %u resp %u len %u\n",
                   flags, seq, h->tpm_rc, h->tpm_req_seq, h->tpm_resp_seq, h->tpm_req_len);
            free(frame);
            munmap(window, HW_WINDOW_SIZE);
            close(fd);
            return 2;
        }
        usleep(2000);
    }
}

static void be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int expect_rc(struct hw_tpm *t, const uint8_t *cmd, uint32_t n, uint32_t rc, const char *name)
{
    uint8_t resp[512];
    uint32_t got = hw_tpm_exec(t, cmd, n, resp, sizeof resp);
    if (got < 10 || rd32(resp + 6) != rc) {
        printf("FAIL %s rc got 0x%x want 0x%x len %u\n", name, got < 10 ? 0xffffffffu : rd32(resp + 6), rc, got);
        return 1;
    }
    printf("ok %s (%u bytes)\n", name, got);
    return 0;
}

struct test_file { const char *name; const char *bytes; };

static int file_read(const char *path, uint64_t offset, void *dst, uint32_t len,
                      uint64_t *size, void *user)
{
    struct test_file *f = user;
    if (strcmp(path, f->name) != 0) return -2;
    *size = strlen(f->bytes);
    if (!dst || !len) return 0;
    if (offset >= *size) return 0;
    if (offset + len > *size) len = (uint32_t)(*size - offset);
    memcpy(dst, f->bytes + offset, len);
    return (int)len;
}

static int cmd_test(void)
{
    uint8_t *window = calloc(1, HW_WINDOW_SIZE);
    uint8_t *frame = calloc(1, HW_WIDTH * HW_HEIGHT * 4);
    uint8_t seed[32], a[32], b[32];
    struct hw_tpm tpm;
    uint8_t cmd[64];
    int fail = 0;
    uint32_t x, y;
    if (!window || !frame) return 1;
    hw_window_init(window, HW_WINDOW_SIZE);
    for (y = 0; y < HW_HEIGHT; y++) {
        for (x = 0; x < HW_WIDTH; x++) {
            uint8_t *p = frame + ((size_t)y * HW_WIDTH + x) * 4;
            p[0] = (uint8_t)x; p[1] = (uint8_t)y; p[2] = 0x40; p[3] = 0xff;
        }
    }
    hw_publish_frame(window, frame, HW_WIDTH, HW_HEIGHT, HW_WIDTH * 4);
    memset(frame, 0, HW_WIDTH * HW_HEIGHT * 4);
    if (hw_copy_front(window, frame, HW_WIDTH * HW_HEIGHT * 4) <= 0 || frame[0] != 0 || frame[2] != 0x40) {
        printf("FAIL frame pixel %02x %02x %02x\n", frame[0], frame[1], frame[2]);
        fail++;
    } else printf("ok frame\n");
    {
        int16_t tone[8] = {1000, 1000, -1000, -1000, 1000, 1000, -1000, -1000};
        struct hw_header *h = (struct hw_header *)window;
        hw_publish_audio(window, tone, 4);
        if (h->audio_head != 16) {
            printf("FAIL audio head %llu\n", (unsigned long long)h->audio_head);
            fail++;
        } else printf("ok audio\n");
    }
    memset(seed, 0xA5, sizeof seed);
    hw_tpm_init(&tpm, seed);
    be16(cmd, 0x8001); be32(cmd + 2, 12); be32(cmd + 6, TPM_CC_GET_RANDOM); be16(cmd + 10, 16);
    fail += expect_rc(&tpm, cmd, 12, TPM_RC_INITIALIZE, "random before startup");
    be16(cmd, 0x8001); be32(cmd + 2, 12); be32(cmd + 6, TPM_CC_STARTUP); be16(cmd + 10, TPM_SU_CLEAR);
    fail += expect_rc(&tpm, cmd, 12, TPM_RC_SUCCESS, "startup");
    be16(cmd, 0x8001); be32(cmd + 2, 12); be32(cmd + 6, TPM_CC_GET_RANDOM); be16(cmd + 10, 16);
    {
        uint8_t resp[64];
        hw_tpm_exec(&tpm, cmd, 12, resp, sizeof resp);
        memcpy(a, resp + 12, 16);
        hw_tpm_exec(&tpm, cmd, 12, resp, sizeof resp);
        memcpy(b, resp + 12, 16);
        if (rd32(resp + 6) != 0 || memcmp(a, b, 16) == 0) {
            printf("FAIL random stream\n");
            fail++;
        } else printf("ok random\n");
    }
    be16(cmd, 0x8001); be32(cmd + 2, 22); be32(cmd + 6, TPM_CC_GET_CAPABILITY);
    be32(cmd + 10, TPM_CAP_TPM_PROPERTIES); be32(cmd + 14, TPM_PT_MANUFACTURER); be32(cmd + 18, 1);
    {
        uint8_t resp[64];
        uint32_t n = hw_tpm_exec(&tpm, cmd, 22, resp, sizeof resp);
        if (n < 27 || rd32(resp + 23) != 0x4857494Eu) {
            printf("FAIL capability n %u id %08x\n", n, n >= 27 ? rd32(resp + 23) : 0);
            fail++;
        } else printf("ok capability HWIN\n");
    }
    {
        uint8_t ext[10 + 4 + 4 + 2 + 32];
        uint8_t resp[128];
        uint32_t i;
        memset(ext, 0, sizeof ext);
        be16(ext, 0x8001);
        be32(ext + 2, sizeof ext);
        be32(ext + 6, TPM_CC_PCR_EXTEND);
        be32(ext + 10, 0);
        be32(ext + 14, 1);
        be16(ext + 18, TPM_ALG_SHA256);
        ext[20] = 0x11;
        hw_tpm_exec(&tpm, ext, sizeof ext, resp, sizeof resp);
        be16(cmd, 0x8001); be32(cmd + 2, 18); be32(cmd + 6, TPM_CC_PCR_READ);
        be32(cmd + 10, 1); be16(cmd + 14, TPM_ALG_SHA256); cmd[16] = 1; cmd[17] = 1;
        hw_tpm_exec(&tpm, cmd, 18, resp, sizeof resp);
        for (i = 0; i < 32; i++) if (resp[28 + i]) break;
        if (rd32(resp + 6) != 0 || i == 32) {
            printf("FAIL pcr extend rc 0x%x\n", rd32(resp + 6));
            fail++;
        } else printf("ok pcr\n");
    }
    be16(cmd, 0x8001); be32(cmd + 2, 10); be32(cmd + 6, 0x00abcdef);
    fail += expect_rc(&tpm, cmd, 10, TPM_RC_COMMAND_CODE, "unknown command");
    free(frame);
    free(window);
    if (fail) {
        printf("%d failed\n", fail);
        return 1;
    }
    {
        struct test_file file = {"woa.img", "HELLO-WOA"};
        struct hw_file_mbox *box = (struct hw_file_mbox *)(window + HW_FILES);
        const char *name = "woa.img";
        int rc;
        memset(box->req, 0, 32);
        box->req[0] = 'H'; box->req[1] = 'W'; box->req[2] = 'I'; box->req[3] = 'N';
        box->req[4] = HW_FILE_STAT;
        memcpy(box->req + 20, name, 7);
        box->req_len = 27;
        box->req_seq = 1;
        rc = hw_file_poll(window, file_read, &file);
        if (rc != 1 || box->status != 0 || box->resp[0] != 9) {
            printf("FAIL host file stat rc %d status %d size %u\n", rc, box->status, box->resp[0]);
            fail++;
        } else printf("ok host file\n");
    }
    printf("all host checks passed\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "test") == 0) return cmd_test();
    if (argc >= 3 && strcmp(argv[1], "init") == 0) {
        off_t offset = argc >= 4 ? strtoll(argv[3], NULL, 0) : 0;
        off_t span = argc >= 5 ? strtoll(argv[4], NULL, 0) : 0;
        return cmd_init(argv[2], offset, span);
    }
    if (argc >= 3 && strcmp(argv[1], "run") == 0) {
        const char *ppm = argc >= 4 ? argv[3] : NULL;
        int timeout = argc >= 5 ? atoi(argv[4]) : 60;
        off_t offset = argc >= 6 ? strtoll(argv[5], NULL, 0) : 0;
        return cmd_run(argv[2], ppm, timeout, offset);
    }
    fprintf(stderr, "usage: hwbridge test | init FILE [offset] [span] | run FILE [frame.ppm] [timeout] [offset]\n");
    return 1;
}
