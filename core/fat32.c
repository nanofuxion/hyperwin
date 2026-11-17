#include "fat32.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Writes a FAT32 file system holding a host directory tree into a byte range of an open file
 * (an EFI system partition inside a raw disk image). Streams file data; only the FAT and the
 * directories are built in memory. */

#define SECTOR 512
#define SPC 8
#define CLUSTER (SECTOR * SPC)
#define RESERVED 32

typedef struct {
    char *path;      /* host path */
    char *name;      /* long name */
    int parent;      /* index, -1 for root */
    int is_dir;
    uint64_t len;    /* file bytes, or directory bytes */
    uint32_t cluster;
    uint32_t nclus;
    unsigned char shortn[11];
} Ent;

typedef struct {
    Ent *e;
    int n, cap;
    unsigned seq;
} Tree;

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }

static int add(Tree *t, const char *path, const char *name, int parent, int is_dir, uint64_t len)
{
    Ent *x;
    if (t->n == t->cap) {
        int cap = t->cap ? t->cap * 2 : 64;
        Ent *ne = realloc(t->e, (size_t)cap * sizeof *ne);
        if (!ne) return -1;
        t->e = ne;
        t->cap = cap;
    }
    x = &t->e[t->n];
    memset(x, 0, sizeof *x);
    x->path = strdup(path);
    x->name = strdup(name);
    if (!x->path || !x->name) return -1;
    x->parent = parent;
    x->is_dir = is_dir;
    x->len = len;
    return t->n++;
}

static int walk(Tree *t, int self)
{
    DIR *d = opendir(t->e[self].path);
    struct dirent *de;
    if (!d) return -1;
    while ((de = readdir(d))) {
        char child[1024];
        struct stat st;
        int idx;
        if (de->d_name[0] == '.' || strlen(de->d_name) > 255) continue;
        snprintf(child, sizeof child, "%s/%s", t->e[self].path, de->d_name);
        if (stat(child, &st) != 0) { closedir(d); return -1; }
        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) continue;
        if (st.st_size > 0xffffffffLL) { closedir(d); return -1; }
        idx = add(t, child, de->d_name, self, S_ISDIR(st.st_mode), S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size);
        if (idx < 0) { closedir(d); return -1; }
        if (S_ISDIR(st.st_mode) && walk(t, idx) != 0) { closedir(d); return -1; }
    }
    closedir(d);
    return 0;
}

static int lfn_slots(const char *name) { return ((int)strlen(name) + 1 + 12) / 13; }

/* 8.3 alias: "STEM~N.EXT", unique across the whole volume through the sequence number. */
static void short_name(Tree *t, Ent *x)
{
    const char *dot = strrchr(x->name, '.');
    char digits[12];
    int i, k = 0, keep;
    memset(x->shortn, ' ', 11);
    if (!dot || dot == x->name) dot = x->name + strlen(x->name);
    snprintf(digits, sizeof digits, "~%u", ++t->seq);
    keep = 8 - (int)strlen(digits);
    for (i = 0; x->name + i < dot && k < keep; i++) {
        char c = x->name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c == ' ' || c == '.' || c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']'
            || (unsigned char)c > 127) continue;
        x->shortn[k++] = (unsigned char)c;
    }
    memcpy(x->shortn + k, digits, strlen(digits));
    if (*dot == '.') {
        for (dot++, k = 0; *dot && k < 3; dot++) {
            char c = *dot;
            if (c >= 'a' && c <= 'z') c -= 32;
            if ((unsigned char)c > 127 || c == ' ' || c == '.') continue;
            x->shortn[8 + k++] = (unsigned char)c;
        }
    }
}

static unsigned char checksum(const unsigned char s[11])
{
    unsigned char sum = 0;
    int i;
    for (i = 0; i < 11; i++) sum = (unsigned char)(((sum & 1) << 7) + (sum >> 1) + s[i]);
    return sum;
}

static unsigned char *put_dirent(unsigned char *p, const unsigned char shortn[11], int attr, uint32_t cluster, uint32_t size)
{
    memcpy(p, shortn, 11);
    p[11] = (unsigned char)attr;
    put16(p + 20, cluster >> 16);
    put16(p + 26, cluster & 0xffff);
    put32(p + 28, size);
    return p + 32;
}

static unsigned char *put_entry(unsigned char *p, const Ent *x)
{
    static const int offs[] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    unsigned short chars[260];
    int n = 0, chunks, c, i;
    while (x->name[n] && n < 255) { chars[n] = (unsigned char)x->name[n]; n++; }
    chars[n++] = 0;
    chunks = (n + 12) / 13;
    for (c = chunks; c >= 1; c--) {
        int base = (c - 1) * 13;
        memset(p, 0, 32);
        p[0] = (unsigned char)(c | (c == chunks ? 0x40 : 0));
        p[11] = 0x0F;
        p[13] = checksum(x->shortn);
        for (i = 0; i < 13; i++) {
            unsigned short ch = base + i < n ? chars[base + i] : 0xFFFF;
            put16(p + offs[i], ch);
        }
        p += 32;
    }
    return put_dirent(p, x->shortn, x->is_dir ? 0x10 : 0x20, x->cluster, x->is_dir ? 0 : (uint32_t)x->len);
}

static int copy_in(int fd, uint64_t at, const char *path, uint64_t len)
{
    static unsigned char buf[1 << 16];
    int in = open(path, O_RDONLY);
    uint64_t done = 0;
    if (in < 0) return -1;
    while (done < len) {
        ssize_t r = read(in, buf, sizeof buf);
        if (r <= 0 || pwrite(fd, buf, (size_t)r, (off_t)(at + done)) != r) { close(in); return -1; }
        done += (uint64_t)r;
    }
    close(in);
    return 0;
}

int hw_fat32_write(const char *dir, int fd, uint64_t offset, uint64_t sectors, const char *label)
{
    Tree t = {0};
    uint32_t fat_sectors, clusters, next = 2, i;
    uint64_t data_start;
    uint32_t *fat = NULL;
    unsigned char boot[SECTOR], info[SECTOR];
    int rc = -1, k;

    if (add(&t, dir, "", -1, 1, 0) < 0 || walk(&t, 0) != 0) goto out;
    /* Directory sizes: dot entries plus each child's long-name slots and alias, plus an end mark. */
    for (k = 0; k < t.n; k++) if (t.e[k].is_dir) t.e[k].len = (k ? 2 : 0) * 32 + 32;
    for (k = 1; k < t.n; k++) {
        short_name(&t, &t.e[k]);
        t.e[t.e[k].parent].len += (uint64_t)(lfn_slots(t.e[k].name) + 1) * 32;
    }
    fat_sectors = (uint32_t)(((sectors - RESERVED) / SPC + 2) * 4 + SECTOR - 1) / SECTOR;
    clusters = (uint32_t)((sectors - RESERVED - 2ull * fat_sectors) / SPC);
    if (clusters < 65525) goto out;
    data_start = offset + (uint64_t)(RESERVED + 2 * fat_sectors) * SECTOR;
    for (k = 0; k < t.n; k++) {
        t.e[k].nclus = (uint32_t)((t.e[k].len + CLUSTER - 1) / CLUSTER);
        if (t.e[k].is_dir && !t.e[k].nclus) t.e[k].nclus = 1;
        t.e[k].cluster = t.e[k].nclus ? next : 0;
        next += t.e[k].nclus;
    }
    if (next > clusters + 2) goto out;
    fat = calloc((size_t)fat_sectors * SECTOR, 1);
    if (!fat) goto out;
    fat[0] = 0x0FFFFFF8;
    fat[1] = 0x0FFFFFFF;
    for (k = 0; k < t.n; k++)
        for (i = 0; i < t.e[k].nclus; i++)
            fat[t.e[k].cluster + i] = i + 1 == t.e[k].nclus ? 0x0FFFFFFF : t.e[k].cluster + i + 1;

    for (k = 0; k < t.n; k++) {
        Ent *x = &t.e[k];
        uint64_t at = data_start + (uint64_t)(x->cluster - 2) * CLUSTER;
        if (x->is_dir) {
            size_t bytes = (size_t)x->nclus * CLUSTER;
            unsigned char *buf = calloc(bytes, 1), *p = buf;
            int c;
            if (!buf) goto out;
            if (k) {
                static const unsigned char dot[11] = {'.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
                static const unsigned char dotdot[11] = {'.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
                p = put_dirent(p, dot, 0x10, x->cluster, 0);
                p = put_dirent(p, dotdot, 0x10, x->parent ? t.e[x->parent].cluster : 0, 0);
            } else {
                unsigned char vol[11];
                memset(vol, ' ', 11);
                memcpy(vol, label, strlen(label) < 11 ? strlen(label) : 11);
                p = put_dirent(p, vol, 0x08, 0, 0);
            }
            for (c = 1; c < t.n; c++) if (t.e[c].parent == k) p = put_entry(p, &t.e[c]);
            if (pwrite(fd, buf, bytes, (off_t)at) != (ssize_t)bytes) { free(buf); goto out; }
            free(buf);
        } else if (x->len && copy_in(fd, at, x->path, x->len) != 0) {
            goto out;
        }
    }

    memset(boot, 0, sizeof boot);
    boot[0] = 0xEB; boot[1] = 0x58; boot[2] = 0x90;
    memcpy(boot + 3, "MSWIN4.1", 8);
    put16(boot + 11, SECTOR);
    boot[13] = SPC;
    put16(boot + 14, RESERVED);
    boot[16] = 2;
    boot[21] = 0xF8;
    put16(boot + 24, 63);
    put16(boot + 26, 255);
    put32(boot + 28, (uint32_t)(offset / SECTOR));
    put32(boot + 32, (uint32_t)sectors);
    put32(boot + 36, fat_sectors);
    put32(boot + 44, 2);
    put16(boot + 48, 1);
    put16(boot + 50, 6);
    boot[64] = 0x80;
    boot[66] = 0x29;
    put32(boot + 67, 0x48575045);
    memset(boot + 71, ' ', 11);
    memcpy(boot + 71, label, strlen(label) < 11 ? strlen(label) : 11);
    memcpy(boot + 82, "FAT32   ", 8);
    boot[510] = 0x55; boot[511] = 0xAA;
    memset(info, 0, sizeof info);
    put32(info, 0x41615252);
    put32(info + 484, 0x61417272);
    put32(info + 488, clusters + 2 - next);
    put32(info + 492, next);
    info[510] = 0x55; info[511] = 0xAA;
    if (pwrite(fd, boot, SECTOR, (off_t)offset) != SECTOR ||
        pwrite(fd, info, SECTOR, (off_t)(offset + SECTOR)) != SECTOR ||
        pwrite(fd, boot, SECTOR, (off_t)(offset + 6 * SECTOR)) != SECTOR ||
        pwrite(fd, info, SECTOR, (off_t)(offset + 7 * SECTOR)) != SECTOR)
        goto out;
    for (k = 0; k < 2; k++) {
        size_t bytes = (size_t)fat_sectors * SECTOR;
        if (pwrite(fd, fat, bytes, (off_t)(offset + (uint64_t)(RESERVED + k * fat_sectors) * SECTOR)) != (ssize_t)bytes)
            goto out;
    }
    rc = 0;
out:
    for (k = 0; k < t.n; k++) { free(t.e[k].path); free(t.e[k].name); }
    free(t.e);
    free(fat);
    return rc;
}

/* ---- replacing one file in an existing volume (ours, or one Windows has since written to) ---- */

static uint32_t get16(const unsigned char *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const unsigned char *p) { return get16(p) | get16(p + 2) << 16; }

typedef struct {
    int fd;
    uint64_t fat_at, data_at;
    uint32_t cluster_bytes, fat_bytes, clusters, nfats;
    unsigned char *fat;          /* first FAT, in memory */
} Vol;

static uint32_t fat_next(const Vol *v, uint32_t c) { return get32(v->fat + 4ull * c) & 0x0FFFFFFF; }
static int is_cluster(const Vol *v, uint32_t c) { return c >= 2 && c < v->clusters + 2; }

static int same_name(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y || !x) return 0;
    }
    return a[n] == 0;
}

/* Finds name[0..n) in the directory starting at cluster dir. Gives the entry's disk position. */
static int find_entry(const Vol *v, uint32_t dir, const char *name, size_t n, uint64_t *at, unsigned char ent[32])
{
    static const int offs[] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    unsigned char *buf = malloc(v->cluster_bytes);
    char lfn[261] = "";
    uint32_t guard = 0, i;
    int found = -1;
    if (!buf) return -1;
    for (; found < 0 && is_cluster(v, dir) && guard++ < v->clusters; dir = fat_next(v, dir)) {
        uint64_t pos = v->data_at + (uint64_t)(dir - 2) * v->cluster_bytes;
        if (pread(v->fd, buf, v->cluster_bytes, (off_t)pos) != (ssize_t)v->cluster_bytes) break;
        for (i = 0; i < v->cluster_bytes; i += 32) {
            unsigned char *e = buf + i;
            char shortn[13];
            int k, j = 0;
            if (e[0] == 0) goto out;
            if (e[0] == 0xE5) { lfn[0] = 0; continue; }
            if (e[11] == 0x0F) {
                int seq = e[0] & 0x3F;
                if (seq < 1 || seq > 20) { lfn[0] = 0; continue; }
                if (e[0] & 0x40) memset(lfn, 0, sizeof lfn);
                for (k = 0; k < 13; k++) {
                    uint32_t ch = get16(e + offs[k]);
                    lfn[(seq - 1) * 13 + k] = ch == 0xFFFF ? 0 : ch < 128 ? (char)ch : '?';
                }
                continue;
            }
            for (k = 0; k < 8 && e[k] != ' '; k++) shortn[j++] = (char)e[k];
            if (e[8] != ' ') shortn[j++] = '.';
            for (k = 8; k < 11 && e[k] != ' '; k++) shortn[j++] = (char)e[k];
            shortn[j] = 0;
            if (!(e[11] & 0x08) && (same_name(lfn, name, n) || same_name(shortn, name, n))) {
                memcpy(ent, e, 32);
                *at = pos + i;
                found = 0;
                break;
            }
            lfn[0] = 0;
        }
    }
out:
    free(buf);
    return found;
}

/* Reads the volume's boot sector and first FAT, then finds path. Fills v, the entry and where it is. */
static int locate(int fd, uint64_t offset, const char *path, Vol *v, unsigned char boot[SECTOR],
                  unsigned char ent[32], uint64_t *at)
{
    uint32_t bps, reserved, cur;
    memset(v, 0, sizeof *v);
    if (pread(fd, boot, SECTOR, (off_t)offset) != SECTOR || boot[510] != 0x55 || boot[511] != 0xAA) return -1;
    bps = get16(boot + 11);
    reserved = get16(boot + 14);
    v->fd = fd;
    v->nfats = boot[16];
    v->cluster_bytes = bps * boot[13];
    v->fat_bytes = get32(boot + 36) * bps;
    if (bps < 512 || !boot[13] || !v->nfats || !v->fat_bytes || get16(boot + 22)) return -1;   /* not FAT32 */
    v->fat_at = offset + (uint64_t)reserved * bps;
    v->data_at = v->fat_at + (uint64_t)v->nfats * v->fat_bytes;
    v->clusters = (uint32_t)(((uint64_t)get32(boot + 32) * bps - (v->data_at - offset)) / v->cluster_bytes);
    if (v->clusters + 2 > v->fat_bytes / 4) v->clusters = v->fat_bytes / 4 - 2;
    v->fat = malloc(v->fat_bytes);
    if (!v->fat || pread(fd, v->fat, v->fat_bytes, (off_t)v->fat_at) != (ssize_t)v->fat_bytes) return -1;
    cur = get32(boot + 44);                      /* root directory */
    for (;;) {
        const char *slash = strchr(path, '/');
        size_t n = slash ? (size_t)(slash - path) : strlen(path);
        if (find_entry(v, cur, path, n, at, ent) != 0) return -1;
        cur = get16(ent + 20) << 16 | get16(ent + 26);
        if (!slash) break;
        if (!(ent[11] & 0x10)) return -1;
        path = slash + 1;
    }
    return ent[11] & 0x10 ? -1 : 0;
}

void *hw_fat32_read(int fd, uint64_t offset, const char *path, uint32_t *len)
{
    unsigned char boot[SECTOR], ent[32];
    Vol v;
    uint64_t at;
    uint32_t size, done = 0, c, guard = 0;
    char *data = NULL;
    if (locate(fd, offset, path, &v, boot, ent, &at) == 0) {
        size = get32(ent + 28);
        data = malloc(size ? size : 1);
        for (c = get16(ent + 20) << 16 | get16(ent + 26); data && done < size; c = fat_next(&v, c)) {
            uint32_t part = size - done < v.cluster_bytes ? size - done : v.cluster_bytes;
            if (!is_cluster(&v, c) || guard++ > v.clusters ||
                pread(fd, data + done, part, (off_t)(v.data_at + (uint64_t)(c - 2) * v.cluster_bytes)) != (ssize_t)part) {
                free(data);
                data = NULL;
                break;
            }
            done += part;
        }
        if (data) *len = size;
    }
    free(v.fat);
    return data;
}

int hw_fat32_replace(int fd, uint64_t offset, const char *path, const void *data, uint32_t len)
{
    unsigned char boot[SECTOR], ent[32], unknown[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    Vol v;
    uint32_t bps, reserved, cur, need, have = 0, c, scan = 2, k, info;
    uint32_t *chain = NULL;
    uint64_t at = 0;
    int rc = -1;

    v.fat = NULL;
    if (!len || locate(fd, offset, path, &v, boot, ent, &at) != 0) goto out;
    bps = get16(boot + 11);
    reserved = get16(boot + 14);
    info = get16(boot + 48);
    cur = get16(ent + 20) << 16 | get16(ent + 26);
    need = (len + v.cluster_bytes - 1) / v.cluster_bytes;
    chain = malloc((size_t)need * sizeof *chain);
    if (!chain) goto out;
    /* Keep the clusters the file has, take free ones for growth, release the rest. */
    for (c = cur; is_cluster(&v, c) && have < v.clusters; ) {
        uint32_t next = fat_next(&v, c);
        if (have < need) chain[have] = c; else put32(v.fat + 4ull * c, 0);
        have++;
        c = next;
    }
    for (k = have < need ? have : need; k < need; k++) {
        while (scan < v.clusters + 2 && fat_next(&v, scan)) scan++;
        if (scan >= v.clusters + 2) goto out;                   /* volume full; nothing written yet */
        chain[k] = scan;
        put32(v.fat + 4ull * scan, 0x0FFFFFFF);
    }
    for (k = 0; k < need; k++) {
        uint32_t part = k + 1 == need ? len - k * v.cluster_bytes : v.cluster_bytes;
        put32(v.fat + 4ull * chain[k], k + 1 == need ? 0x0FFFFFFF : chain[k + 1]);
        if (pwrite(fd, (const char *)data + (size_t)k * v.cluster_bytes, part,
                   (off_t)(v.data_at + (uint64_t)(chain[k] - 2) * v.cluster_bytes)) != (ssize_t)part) goto out;
    }
    put16(ent + 20, chain[0] >> 16);
    put16(ent + 26, chain[0] & 0xffff);
    put32(ent + 28, len);
    for (k = 0; k < v.nfats; k++)
        if (pwrite(fd, v.fat, v.fat_bytes, (off_t)(v.fat_at + (uint64_t)k * v.fat_bytes)) != (ssize_t)v.fat_bytes) goto out;
    if (pwrite(fd, ent, 32, (off_t)at) != 32) goto out;
    /* The free-space hint is stale now; "unknown" makes the next mount count again. */
    if (info && info < reserved) pwrite(fd, unknown, 4, (off_t)(offset + (uint64_t)info * bps + 488));
    rc = fsync(fd);
out:
    free(chain);
    free(v.fat);
    return rc;
}

#ifdef HW_FAT32_MAIN
int main(int argc, char **argv)
{
    if (argc == 5) {                              /* replace IMAGE OFFSET PATH FILE */
        int img = open(argv[1], O_RDWR), in = open(argv[4], O_RDONLY);
        static unsigned char data[1 << 24];
        ssize_t n = in < 0 ? -1 : read(in, data, sizeof data);
        return img < 0 || n <= 0 || hw_fat32_replace(img, strtoull(argv[2], NULL, 0), argv[3], data, (uint32_t)n) ? 1 : 0;
    }
    int fd = argc == 4 ? open(argv[2], O_RDWR | O_CREAT | O_TRUNC, 0644) : -1;
    uint64_t sectors = fd < 0 ? 0 : strtoull(argv[3], NULL, 0);
    if (fd < 0 || ftruncate(fd, (off_t)(sectors * SECTOR))) return 1;
    return hw_fat32_write(argv[1], fd, 0, sectors, "SYSTEM") ? 1 : 0;
}
#endif
