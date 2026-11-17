#include "udf.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Read-only UDF walk: enough to find one file in a Windows install ISO (oscdimg -u2 puts the real
 * tree only in UDF). Supports File Entry / Extended File Entry with short or long allocation
 * descriptors; the file must be one contiguous run, which is how ISO writers lay files out. */

#define SECTOR 2048

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

typedef struct {
    int fd;
    uint32_t part_start;
    uint8_t sec[SECTOR];
} Udf;

static int rd(Udf *u, uint64_t lba, void *buf, size_t len)
{
    return pread(u->fd, buf, len, (off_t)(lba * SECTOR)) == (ssize_t)len ? 0 : -1;
}

typedef struct {
    uint64_t size;
    uint64_t lba;      /* absolute sector of the first extent */
    int contiguous;
    int is_dir;
    uint8_t *inline_data;  /* data embedded in the ICB (alloc type 3) */
} Node;

/* Reads the (Extended) File Entry at partition block lbn. */
static int read_fe(Udf *u, uint32_t lbn, Node *n)
{
    uint8_t *s = u->sec;
    uint16_t tag;
    uint32_t l_ea, l_ad, adoff, type;
    uint64_t next = 0, run = 0;
    uint32_t i;
    memset(n, 0, sizeof *n);
    if (rd(u, u->part_start + (uint64_t)lbn, s, SECTOR)) return -1;
    tag = le16(s);
    if (tag == 261) { l_ea = le32(s + 168); l_ad = le32(s + 172); adoff = 176; }
    else if (tag == 266) { l_ea = le32(s + 208); l_ad = le32(s + 212); adoff = 216; }
    else return -1;
    n->is_dir = s[16 + 11] == 4;
    n->size = le64(s + 56);
    adoff += l_ea;
    if (adoff + l_ad > SECTOR) return -1;
    type = le16(s + 16 + 18) & 7;
    if (type == 3) {
        n->inline_data = malloc(l_ad ? l_ad : 1);
        if (!n->inline_data) return -1;
        memcpy(n->inline_data, s + adoff, l_ad);
        n->contiguous = 1;
        return 0;
    }
    n->contiguous = 1;
    for (i = 0; i < l_ad;) {
        uint32_t len, pos;
        if (type == 0) { len = le32(s + adoff + i) & 0x3fffffff; pos = le32(s + adoff + i + 4); i += 8; }
        else if (type == 1) { len = le32(s + adoff + i) & 0x3fffffff; pos = le32(s + adoff + i + 4); i += 16; }
        else return -1;
        if (!len) break;
        if (run == 0) n->lba = u->part_start + (uint64_t)pos;
        else if (u->part_start + (uint64_t)pos != next) n->contiguous = 0;
        next = u->part_start + (uint64_t)pos + (len + SECTOR - 1) / SECTOR;
        run += len;
    }
    return 0;
}

static int name_eq(const uint8_t *d, unsigned len, const char *want, size_t wlen)
{
    size_t k = 0;
    unsigned i;
    if (!len) return 0;
    if (d[0] == 8) {
        for (i = 1; i < len; i++, k++)
            if (k >= wlen || tolower(d[i]) != tolower((unsigned char)want[k])) return 0;
    } else if (d[0] == 16) {
        for (i = 1; i + 1 < len; i += 2, k++) {
            unsigned c = (unsigned)d[i] << 8 | d[i + 1];
            if (k >= wlen || c > 127 || tolower((int)c) != tolower((unsigned char)want[k])) return 0;
        }
    } else {
        return 0;
    }
    return k == wlen;
}

/* Finds child `name` in directory node dir; returns its ICB block or -1. */
static long find_child(Udf *u, Node *dir, const char *name, size_t nlen)
{
    uint8_t *buf;
    uint64_t off = 0;
    long found = -1;
    if (!dir->is_dir || !dir->contiguous || dir->size > 64u << 20) return -1;
    buf = dir->inline_data;
    if (!buf) {
        buf = malloc((size_t)dir->size ? (size_t)dir->size : 1);
        if (!buf) return -1;
        if (dir->size && rd(u, dir->lba, buf, (size_t)dir->size)) { free(buf); return -1; }
    }
    while (off + 38 <= dir->size) {
        const uint8_t *f = buf + off;
        unsigned l_fi, l_iu;
        if (le16(f) != 257) break;
        l_fi = f[19];
        l_iu = le16(f + 36);
        if (!(f[18] & 0x0c) && name_eq(f + 38 + l_iu, l_fi, name, nlen)) {
            found = (long)le32(f + 20 + 4);
            break;
        }
        off += (38 + l_iu + l_fi + 3) & ~3u;
    }
    if (buf != dir->inline_data) free(buf);
    return found;
}

int hw_udf_find(int fd, const char *path, uint64_t *offset, uint64_t *size)
{
    Udf u;
    uint8_t *s = u.sec;
    uint32_t vds, vds_len, i, fsd_lbn = 0;
    int have_pd = 0, have_lvd = 0;
    Node node;
    long icb;
    const char *p = path;

    memset(&u, 0, sizeof u);
    u.fd = fd;
    if (rd(&u, 256, s, SECTOR) || le16(s) != 2) return -1;
    vds_len = le32(s + 16);
    vds = le32(s + 20);
    for (i = 0; i < vds_len / SECTOR && i < 64; i++) {
        uint16_t tag;
        if (rd(&u, vds + i, s, SECTOR)) return -1;
        tag = le16(s);
        if (tag == 5 && !have_pd) { u.part_start = le32(s + 188); have_pd = 1; }
        else if (tag == 6 && !have_lvd) { fsd_lbn = le32(s + 248 + 4); have_lvd = 1; }
        else if (tag == 8) break;
    }
    if (!have_pd || !have_lvd) return -1;
    if (rd(&u, u.part_start + (uint64_t)fsd_lbn, s, SECTOR) || le16(s) != 256) return -1;
    icb = (long)le32(s + 400 + 4);
    for (;;) {
        const char *slash;
        size_t len;
        if (read_fe(&u, (uint32_t)icb, &node)) return -1;
        while (*p == '/') p++;
        if (!*p) break;
        slash = strchr(p, '/');
        len = slash ? (size_t)(slash - p) : strlen(p);
        icb = find_child(&u, &node, p, len);
        free(node.inline_data);
        if (icb < 0) return -1;
        p += len;
    }
    if (node.is_dir || !node.contiguous || node.inline_data) { free(node.inline_data); return -1; }
    *offset = node.lba * SECTOR;
    *size = node.size;
    return 0;
}

#ifdef HW_UDF_MAIN
#include <fcntl.h>
#include <stdio.h>
int main(int argc, char **argv)
{
    uint64_t off, size;
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0 || hw_udf_find(fd, argv[2], &off, &size)) { puts("not found"); return 1; }
    printf("%llu %llu\n", (unsigned long long)off, (unsigned long long)size);
    return 0;
}
#endif
