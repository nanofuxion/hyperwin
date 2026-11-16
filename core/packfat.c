#include "packfat.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SECTOR 512
#define SPC 4
#define RESERVED 1
#define FATS 2
#define ROOT_ENTRIES 512
#define MAX_ENT 96
#define MAX_NAME 64

typedef struct {
    char parent[8][MAX_NAME];
    int nparent;
    char name[MAX_NAME];
    int is_dir;
    unsigned char *data;
    unsigned len;
    unsigned cluster;
} Ent;

static void die_free(Ent *e, int n, unsigned char *img)
{
    int i;
    for (i = 0; i < n; i++) free(e[i].data);
    free(img);
}

static int walk(const char *path, Ent *e, int *n, char parent[][MAX_NAME], int nparent)
{
    DIR *d = opendir(path);
    struct dirent *de;
    if (!d) return -1;
    while ((de = readdir(d))) {
        char child[512];
        struct stat st;
        int i;
        if (de->d_name[0] == '.') continue;
        if (strlen(de->d_name) >= MAX_NAME || *n >= MAX_ENT) { closedir(d); return -1; }
        snprintf(child, sizeof child, "%s/%s", path, de->d_name);
        if (stat(child, &st) != 0) { closedir(d); return -1; }
        Ent *ent = &e[*n];
        memset(ent, 0, sizeof *ent);
        ent->nparent = nparent;
        for (i = 0; i < nparent; i++) memcpy(ent->parent[i], parent[i], MAX_NAME);
        memcpy(ent->name, de->d_name, strlen(de->d_name) + 1);
        if (S_ISDIR(st.st_mode)) {
            char next[8][MAX_NAME];
            ent->is_dir = 1;
            (*n)++;
            if (nparent + 1 > 8) { closedir(d); return -1; }
            for (i = 0; i < nparent; i++) memcpy(next[i], parent[i], MAX_NAME);
            memcpy(next[nparent], de->d_name, strlen(de->d_name) + 1);
            if (walk(child, e, n, next, nparent + 1) != 0) { closedir(d); return -1; }
        } else if (S_ISREG(st.st_mode)) {
            FILE *f = fopen(child, "rb");
            if (!f) { closedir(d); return -1; }
            ent->data = malloc((size_t)st.st_size ? (size_t)st.st_size : 1);
            if (!ent->data || fread(ent->data, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
                fclose(f); closedir(d); return -1;
            }
            fclose(f);
            ent->len = (unsigned)st.st_size;
            (*n)++;
        }
    }
    closedir(d);
    return 0;
}

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static int short_seq;

static void short_name(const char *name, unsigned char out[11])
{
    const char *dot = strrchr(name, '.');
    char stem[32], ext[8];
    int si = 0, ei = 0, i, fits;
    memset(out, ' ', 11);
    if (name[0] == '.' && name[1] == 0) { out[0] = '.'; return; }
    if (name[0] == '.' && name[1] == '.' && name[2] == 0) { out[0] = out[1] = '.'; return; }
    if (!dot || dot == name) dot = name + strlen(name);
    for (i = 0; name + i < dot && si < 31; i++)
        stem[si++] = name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i];
    if (*dot == '.') for (dot++; *dot && ei < 7; dot++)
        ext[ei++] = *dot >= 'a' && *dot <= 'z' ? *dot - 32 : *dot;
    fits = si <= 8 && ei <= 3;
    if (!fits) {
        int n = ++short_seq;
        int keep = si > 6 ? 6 : si;
        for (i = 0; i < keep; i++) out[i] = (unsigned char)stem[i];
        out[keep] = '~';
        out[keep + 1] = (unsigned char)('0' + (n % 10));
    } else {
        for (i = 0; i < si; i++) out[i] = (unsigned char)stem[i];
    }
    for (i = 0; i < ei && i < 3; i++) out[8 + i] = (unsigned char)ext[i];
}

static unsigned char checksum(const unsigned char s[11])
{
    unsigned char sum = 0;
    int i;
    for (i = 0; i < 11; i++) sum = (unsigned char)(((sum & 1) << 7) + (sum >> 1) + s[i]);
    return sum;
}

static int write_lfn(unsigned char *dir, int slot, const char *name, const unsigned char shortn[11])
{
    unsigned short chars[256];
    int n = 0, chunks, c, i;
    while (name[n] && n < 255) { chars[n] = (unsigned char)name[n]; n++; }
    chars[n++] = 0;
    chunks = (n + 12) / 13;
    for (c = chunks; c >= 1; c--) {
        unsigned char *ent = dir + slot;
        int base = (c - 1) * 13;
        static const int offs[] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        memset(ent, 0, 32);
        ent[0] = (unsigned char)(c | (c == chunks ? 0x40 : 0));
        ent[11] = 0x0F;
        ent[13] = checksum(shortn);
        for (i = 0; i < 13; i++) {
            unsigned short ch = base + i < n ? chars[base + i] : 0xFFFF;
            ent[offs[i]] = (unsigned char)ch;
            ent[offs[i] + 1] = (unsigned char)(ch >> 8);
        }
        slot += 32;
    }
    return slot;
}

static int add_dirent(unsigned char *dir, int cap, const char *name, int attr, unsigned cluster, unsigned size)
{
    unsigned char shortn[11];
    int chunks, need, slot = 0, i;
    short_name(name, shortn);
    chunks = (name[0] == '.') ? 0 : ((int)strlen(name) + 1 + 12) / 13;
    need = (chunks + 1) * 32;
    while (slot + need <= cap) {
        for (i = 0; i < need; i += 32) if (dir[slot + i] != 0) break;
        if (i >= need) break;
        slot += 32;
    }
    if (slot + need > cap) return -1;
    if (chunks) slot = write_lfn(dir, slot, name, shortn);
    memcpy(dir + slot, shortn, 11);
    dir[slot + 11] = (unsigned char)attr;
    put16(dir + slot + 26, cluster);
    put32(dir + slot + 28, size);
    return 0;
}

int hw_pack_tree(const char *dir, const char *image_path)
{
    Ent ents[MAX_ENT];
    int n = 0, i, cluster = 2, fat_sectors, root_sectors, data_sectors, part_sectors;
    unsigned char *img = NULL, *fat, *root;
    char parent[8][MAX_NAME];
    FILE *out;
    unsigned long long bytes;
    memset(ents, 0, sizeof ents);
    short_seq = 0;
    if (walk(dir, ents, &n, parent, 0) != 0) { die_free(ents, n, NULL); return -1; }
    for (i = 0; i < n; i++) {
        ents[i].cluster = (unsigned)cluster;
        if (ents[i].is_dir) cluster += 1;
        else cluster += (int)((ents[i].len + SPC * SECTOR - 1) / (SPC * SECTOR));
        if (!ents[i].is_dir && ents[i].len == 0) cluster += 1;
    }
    root_sectors = ROOT_ENTRIES * 32 / SECTOR;
    fat_sectors = ((cluster + 2) * 2 + SECTOR - 1) / SECTOR;
    data_sectors = (cluster - 2) * SPC;
    part_sectors = RESERVED + FATS * fat_sectors + root_sectors + data_sectors;
    part_sectors = (part_sectors + 63) & ~63;
    bytes = (unsigned long long)part_sectors * SECTOR;
    img = calloc(1, (size_t)bytes);
    if (!img) { die_free(ents, n, NULL); return -1; }
    img[0] = 0xEB; img[1] = 0x3C; img[2] = 0x90;
    memcpy(img + 3, "HYPERWIN", 8);
    put16(img + 11, SECTOR);
    img[13] = SPC;
    put16(img + 14, RESERVED);
    img[16] = FATS;
    put16(img + 17, ROOT_ENTRIES);
    img[21] = 0xF8;
    put16(img + 22, (unsigned)fat_sectors);
    put16(img + 24, 32);
    put16(img + 26, 64);
    put32(img + 32, (unsigned)part_sectors);
    img[36] = 0x80;
    img[38] = 0x29;
    put32(img + 39, 0x4857494E);
    memcpy(img + 43, "HYPERWIN   ", 11);
    memcpy(img + 54, "FAT16   ", 8);
    img[510] = 0x55; img[511] = 0xAA;
    fat = img + RESERVED * SECTOR;
    put16(fat, 0xFFF8);
    put16(fat + 2, 0xFFFF);
    for (i = 0; i < n; i++) {
        int nclus = ents[i].is_dir || ents[i].len == 0 ? 1
            : (int)((ents[i].len + SPC * SECTOR - 1) / (SPC * SECTOR));
        int c;
        for (c = 0; c < nclus; c++) {
            unsigned next = c + 1 == nclus ? 0xFFFF : ents[i].cluster + (unsigned)c + 1;
            put16(fat + (ents[i].cluster + (unsigned)c) * 2, next);
        }
    }
    memcpy(fat + fat_sectors * SECTOR, fat, (size_t)fat_sectors * SECTOR);
    root = img + (RESERVED + FATS * fat_sectors) * SECTOR;
    for (i = 0; i < n; i++) {
        unsigned char *parent_dir = root;
        int cap = root_sectors * SECTOR;
        int p;
        if (ents[i].nparent) {
            for (p = 0; p < n; p++) {
                if (!ents[p].is_dir || ents[p].nparent + 1 != ents[i].nparent) continue;
                if (strcmp(ents[p].name, ents[i].parent[ents[i].nparent - 1]) != 0) continue;
                parent_dir = img + ((RESERVED + FATS * fat_sectors + root_sectors
                    + (ents[p].cluster - 2) * SPC) * SECTOR);
                cap = SPC * SECTOR;
                break;
            }
        }
        if (add_dirent(parent_dir, cap, ents[i].name, ents[i].is_dir ? 0x10 : 0x20,
                ents[i].cluster, ents[i].is_dir ? 0 : ents[i].len) != 0) {
            die_free(ents, n, img);
            return -1;
        }
        if (ents[i].is_dir) {
            unsigned char *buf = img + ((RESERVED + FATS * fat_sectors + root_sectors
                + (ents[i].cluster - 2) * SPC) * SECTOR);
            unsigned dotdot = 0;
            if (ents[i].nparent) {
                int p;
                for (p = 0; p < n; p++)
                    if (ents[p].is_dir && ents[p].nparent + 1 == ents[i].nparent &&
                        strcmp(ents[p].name, ents[i].parent[ents[i].nparent - 1]) == 0)
                        dotdot = ents[p].cluster;
            }
            if (add_dirent(buf, SPC * SECTOR, ".", 0x10, ents[i].cluster, 0) != 0 ||
                add_dirent(buf, SPC * SECTOR, "..", 0x10, dotdot, 0) != 0) {
                die_free(ents, n, img);
                return -1;
            }
        } else if (ents[i].len) {
            unsigned off = (RESERVED + FATS * fat_sectors + root_sectors
                + (ents[i].cluster - 2) * SPC) * SECTOR;
            memcpy(img + off, ents[i].data, ents[i].len);
        }
    }
    out = fopen(image_path, "wb");
    if (!out || fwrite(img, 1, (size_t)bytes, out) != (size_t)bytes) {
        if (out) fclose(out);
        die_free(ents, n, img);
        return -1;
    }
    fclose(out);
    die_free(ents, n, img);
    return 0;
}

#ifdef HW_PACK_MAIN
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    return hw_pack_tree(argv[1], argv[2]) == 0 ? 0 : 1;
}
#endif
