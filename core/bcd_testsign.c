#include "bcd_testsign.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HIVE_BASE 0x1000u
#define WIN_LOADER 0x10200003u
#define CELL_NONE 0xffffffffu

static uint16_t r16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t r32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t r64(const uint8_t *p)
{
    return (uint64_t)r32(p) | ((uint64_t)r32(p + 4) << 32);
}

static void w16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void w32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void set_status(char *status, size_t cap, const char *msg)
{
    size_t i;
    if (!status || cap == 0) return;
    for (i = 0; i + 1 < cap && msg[i]; i++) status[i] = msg[i];
    status[i] = 0;
}

static int fold(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    return c;
}

static int name_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        int d = fold((unsigned char)*a) - fold((unsigned char)*b);
        if (d) return d;
        a++;
        b++;
    }
    return fold((unsigned char)*a) - fold((unsigned char)*b);
}

static int read_full(int fd, uint64_t off, void *buf, size_t n)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, p + got, n - got, (off_t)(off + got));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int write_full(int fd, uint64_t off, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    size_t put = 0;
    while (put < n) {
        ssize_t w = pwrite(fd, p + put, n - put, (off_t)(off + put));
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        put += (size_t)w;
    }
    return 0;
}

typedef struct {
    uint8_t *b;
    size_t n;
} Hive;

static int cell_ok(const Hive *h, uint32_t cell, uint32_t need)
{
    return (uint64_t)HIVE_BASE + cell + need <= h->n;
}

static uint8_t *cell_body(Hive *h, uint32_t cell, int32_t *size_out)
{
    int32_t sz;
    uint32_t absz;
    if (cell == CELL_NONE || !cell_ok(h, cell, 4)) return NULL;
    sz = (int32_t)r32(h->b + HIVE_BASE + cell);
    if (sz >= 0) return NULL;
    absz = (uint32_t)(-sz);
    if (absz < 8 || (absz & 7) || !cell_ok(h, cell, absz)) return NULL;
    if (size_out) *size_out = sz;
    return h->b + HIVE_BASE + cell + 4;
}

static int nk_name(const uint8_t *body, int32_t cellsz, char *out, size_t cap)
{
    uint32_t blen = (uint32_t)(-cellsz) - 4;
    uint16_t flags, nlen, i;
    if (blen < 0x4c || cap < 2) return -1;
    flags = r16(body + 2);
    nlen = r16(body + 0x48);
    if ((uint32_t)0x4c + nlen > blen || nlen + 1 > cap) return -1;
    if (flags & 0x20) {
        memcpy(out, body + 0x4c, nlen);
        out[nlen] = 0;
        return 0;
    }
    if (nlen & 1) return -1;
    for (i = 0; i < nlen / 2; i++) {
        uint16_t c = r16(body + 0x4c + (uint32_t)i * 2);
        if (c > 127) return -1;
        out[i] = (char)c;
    }
    out[nlen / 2] = 0;
    return 0;
}

static int walk_index(Hive *h, uint32_t list, int (*cb)(Hive *, uint32_t, void *), void *ctx);

static int walk_index(Hive *h, uint32_t list, int (*cb)(Hive *, uint32_t, void *), void *ctx)
{
    int32_t sz;
    uint8_t *body = cell_body(h, list, &sz);
    uint32_t blen, stride, i;
    uint16_t count;
    if (!body) return -1;
    blen = (uint32_t)(-sz) - 4;
    if (blen < 4) return -1;
    count = r16(body + 2);
    if (memcmp(body, "ri", 2) == 0) {
        if (4u + (uint32_t)count * 4 > blen) return -1;
        for (i = 0; i < count; i++) {
            if (walk_index(h, r32(body + 4 + i * 4), cb, ctx)) return -1;
        }
        return 0;
    }
    if (memcmp(body, "li", 2) == 0) stride = 4;
    else if (memcmp(body, "lf", 2) == 0 || memcmp(body, "lh", 2) == 0) stride = 8;
    else return -1;
    if (4u + (uint32_t)count * stride > blen) return -1;
    for (i = 0; i < count; i++) {
        if (cb(h, r32(body + 4 + i * stride), ctx)) return -1;
    }
    return 0;
}

static int each_sub(Hive *h, uint32_t parent, int (*cb)(Hive *, uint32_t, void *), void *ctx)
{
    int32_t sz;
    uint8_t *body = cell_body(h, parent, &sz);
    uint32_t nsub, list;
    if (!body || memcmp(body, "nk", 2) != 0) return -1;
    nsub = r32(body + 0x14);
    list = r32(body + 0x1c);
    if (nsub == 0 || list == CELL_NONE) return 0;
    return walk_index(h, list, cb, ctx);
}

typedef struct {
    const char *name;
    uint32_t found;
    int bad;
} FindCtx;

static int find_cb(Hive *h, uint32_t off, void *v)
{
    FindCtx *c = v;
    int32_t sz;
    uint8_t *body = cell_body(h, off, &sz);
    char name[128];
    if (!body || memcmp(body, "nk", 2) != 0 || nk_name(body, sz, name, sizeof name)) {
        c->bad = 1;
        return 1;
    }
    if (name_cmp(name, c->name) == 0) {
        c->found = off;
        return 1;
    }
    return 0;
}

static uint32_t find_sub(Hive *h, uint32_t parent, const char *name, int *bad)
{
    FindCtx c;
    c.name = name;
    c.found = CELL_NONE;
    c.bad = 0;
    if (each_sub(h, parent, find_cb, &c)) {
        if (c.bad && bad) *bad = 1;
        if (!c.found && c.bad) return CELL_NONE;
    }
    if (c.bad && bad) *bad = 1;
    return c.found;
}

static int value_of(Hive *h, uint32_t nk, const char *want, uint32_t *type, uint32_t *vk_out,
                    const uint8_t **data, uint32_t *len)
{
    int32_t sz, lsz, vsz;
    uint8_t *body = cell_body(h, nk, &sz);
    uint8_t *list, *vb;
    uint32_t nval, list_off, blen, i;
    if (!body || memcmp(body, "nk", 2) != 0) return -1;
    nval = r32(body + 0x24);
    list_off = r32(body + 0x28);
    if (nval == 0 || list_off == CELL_NONE) return 0;
    list = cell_body(h, list_off, &lsz);
    if (!list) return -1;
    blen = (uint32_t)(-lsz) - 4;
    if (nval > 1024 || nval * 4 > blen) return -1;
    for (i = 0; i < nval; i++) {
        uint32_t vk = r32(list + i * 4);
        uint16_t nlen, flags, k;
        uint32_t dlen, doff, nbytes, vblen;
        char name[64];
        vb = cell_body(h, vk, &vsz);
        if (!vb) return -1;
        vblen = (uint32_t)(-vsz) - 4;
        if (vblen < 0x14 || memcmp(vb, "vk", 2) != 0) return -1;
        nlen = r16(vb + 2);
        flags = r16(vb + 0x10);
        if ((uint32_t)0x14 + nlen > vblen || nlen >= sizeof name) return -1;
        if (flags & 1) {
            memcpy(name, vb + 0x14, nlen);
            name[nlen] = 0;
        } else {
            if (nlen & 1) return -1;
            for (k = 0; k < nlen / 2; k++) {
                uint16_t c = r16(vb + 0x14 + (uint32_t)k * 2);
                if (c > 127) return -1;
                name[k] = (char)c;
            }
            name[nlen / 2] = 0;
        }
        if (name_cmp(name, want) != 0) continue;
        dlen = r32(vb + 4);
        doff = r32(vb + 8);
        nbytes = dlen & 0x7fffffffu;
        *type = r32(vb + 0x0c);
        *vk_out = vk;
        if (dlen & 0x80000000u) {
            if (nbytes > 4) return -1;
            *data = vb + 8;
            *len = nbytes;
            return 1;
        }
        {
            int32_t dsz;
            uint8_t *db = cell_body(h, doff, &dsz);
            if (!db || (uint32_t)(-dsz) - 4 < nbytes) return -1;
            *data = db;
            *len = nbytes;
            return 1;
        }
    }
    return 0;
}

static int dword_value(Hive *h, uint32_t nk, const char *name, uint32_t *out)
{
    uint32_t type = 0, vk = 0, len = 0;
    const uint8_t *data = NULL;
    int rc = value_of(h, nk, name, &type, &vk, &data, &len);
    (void)vk;
    if (rc <= 0) return rc;
    if (type != 4 || len < 4 || !data) return -1;
    *out = r32(data);
    return 1;
}

static int alloc_cell(Hive *h, uint32_t need, uint32_t *out)
{
    uint32_t end, off;
    if (need < 8 || (need & 7) || h->n < HIVE_BASE + 4) return -1;
    end = r32(h->b + 0x28);
    if ((uint64_t)HIVE_BASE + end > h->n) return -1;
    off = 0;
    while (off + 4 <= end) {
        int32_t sz;
        uint32_t absz;
        if (memcmp(h->b + HIVE_BASE + off, "hbin", 4) == 0) {
            uint32_t binsz = r32(h->b + HIVE_BASE + off + 8);
            if (binsz < 0x20 || off + binsz > end) return -1;
            off += 0x20;
            continue;
        }
        sz = (int32_t)r32(h->b + HIVE_BASE + off);
        if (sz == 0) return -1;
        absz = sz < 0 ? (uint32_t)(-sz) : (uint32_t)sz;
        if (absz < 8 || (absz & 7) || off + absz > end) return -1;
        if (sz > 0 && absz >= need) {
            uint32_t take = need;
            uint32_t left = absz - need;
            if (left && left < 8) {
                take = absz;
                left = 0;
            }
            w32(h->b + HIVE_BASE + off, (uint32_t)(-(int32_t)take));
            memset(h->b + HIVE_BASE + off + 4, 0, take - 4);
            if (left) w32(h->b + HIVE_BASE + off + take, left);
            *out = off;
            return 0;
        }
        off += absz;
    }
    return -1;
}

static uint32_t hint_for(const uint8_t *sig, const char *name)
{
    uint32_t hash = 0;
    const unsigned char *p;
    if (memcmp(sig, "lh", 2) != 0) {
        uint8_t raw[4] = {0, 0, 0, 0};
        size_t n = strlen(name);
        if (n > 4) n = 4;
        memcpy(raw, name, n);
        return r32(raw);
    }
    for (p = (const unsigned char *)name; *p; p++) {
        unsigned char c = *p;
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        hash = hash * 37u + c;
    }
    return hash;
}

static int index_sorted(Hive *h, uint8_t *body, uint16_t count, uint32_t stride)
{
    uint16_t i;
    char prev[128], name[128];
    prev[0] = 0;
    for (i = 0; i < count; i++) {
        int32_t sz;
        uint8_t *nb = cell_body(h, r32(body + 4 + (uint32_t)i * stride), &sz);
        if (!nb || nk_name(nb, sz, name, sizeof name)) return -1;
        if (i && name_cmp(prev, name) > 0) return -1;
        memcpy(prev, name, strlen(name) + 1);
    }
    return 0;
}

static int insert_subkey(Hive *h, uint32_t parent, uint32_t child, const char *name)
{
    int32_t psz, lsz;
    uint8_t *parent_body = cell_body(h, parent, &psz);
    uint32_t nsub, list_off, stride, blen, slot, i;
    uint16_t count;
    uint8_t *list;
    uint8_t sig[2];
    int32_t new_sz = 0;
    uint32_t new_off = CELL_NONE;
    if (!parent_body) return -1;
    nsub = r32(parent_body + 0x14);
    list_off = r32(parent_body + 0x1c);
    if (nsub == 0 || list_off == CELL_NONE) {
        uint32_t off;
        uint8_t *body;
        if (alloc_cell(h, 16, &off)) return -1;
        body = cell_body(h, off, &lsz);
        if (!body) return -1;
        memcpy(body, "lf", 2);
        w16(body + 2, 1);
        w32(body + 4, child);
        w32(body + 8, hint_for(body, name));
        parent_body = cell_body(h, parent, &psz);
        if (!parent_body) return -1;
        w32(parent_body + 0x14, 1);
        w32(parent_body + 0x1c, off);
        return 0;
    }
    list = cell_body(h, list_off, &lsz);
    if (!list) return -1;
    if (memcmp(list, "lf", 2) != 0 && memcmp(list, "lh", 2) != 0 && memcmp(list, "li", 2) != 0)
        return -1;
    count = r16(list + 2);
    if (count != nsub) return -1;
    stride = memcmp(list, "li", 2) == 0 ? 4u : 8u;
    blen = (uint32_t)(-lsz) - 4;
    sig[0] = list[0];
    sig[1] = list[1];
    slot = count;
    for (i = 0; i < count; i++) {
        int32_t csz;
        char child_name[128];
        uint8_t *cb = cell_body(h, r32(list + 4 + i * stride), &csz);
        if (!cb || nk_name(cb, csz, child_name, sizeof child_name)) return -1;
        if (name_cmp(child_name, name) > 0) {
            slot = i;
            break;
        }
    }
    if (4u + ((uint32_t)count + 1) * stride > blen) {
        uint32_t body_need = 4u + ((uint32_t)count + 1) * stride;
        uint32_t cell_need = (body_need + 4u + 7u) & ~7u;
        uint8_t *fresh;
        if (alloc_cell(h, cell_need, &new_off)) return -1;
        fresh = cell_body(h, new_off, &new_sz);
        list = cell_body(h, list_off, &lsz);
        if (!fresh || !list) return -1;
        memset(fresh, 0, (uint32_t)(-new_sz) - 4);
        memcpy(fresh, list, 4u + (uint32_t)count * stride);
        list = fresh;
        blen = (uint32_t)(-new_sz) - 4;
    }
    if (4u + ((uint32_t)count + 1) * stride > blen) return -1;
    memmove(list + 4 + (slot + 1) * stride, list + 4 + slot * stride, (count - slot) * stride);
    w32(list + 4 + slot * stride, child);
    if (stride == 8) w32(list + 8 + slot * stride, hint_for(sig, name));
    w16(list + 2, (uint16_t)(count + 1));
    if (index_sorted(h, list, (uint16_t)(count + 1), stride)) return -1;
    parent_body = cell_body(h, parent, &psz);
    if (!parent_body) return -1;
    w32(parent_body + 0x14, count + 1);
    if (new_off != CELL_NONE) {
        w32(parent_body + 0x1c, new_off);
        w32(h->b + HIVE_BASE + list_off, (uint32_t)(-lsz));
    }
    return 0;
}

typedef struct {
    uint32_t found;
    int bad;
} Tpl;

static int tpl_cb(Hive *h, uint32_t off, void *v)
{
    Tpl *t = v;
    uint32_t type = 0, vk = 0, len = 0;
    const uint8_t *data = NULL;
    int32_t sz;
    uint8_t *body = cell_body(h, off, &sz);
    char name[32];
    int rc;
    if (!body || nk_name(body, sz, name, sizeof name)) {
        t->bad = 1;
        return 1;
    }
    if (strlen(name) != 8) return 0;
    rc = value_of(h, off, "Element", &type, &vk, &data, &len);
    if (rc < 0) {
        t->bad = 1;
        return 1;
    }
    if (rc == 1 && type == 3 && len == 1) {
        t->found = off;
        return 1;
    }
    return 0;
}

static int add_testsigning(Hive *h, uint32_t elements)
{
    Tpl tpl;
    int32_t nsz = 0, vsz = 0, lsz = 0, esz = 0;
    uint8_t nkbuf[256], vkbuf[128], listbuf[64];
    uint32_t nk_off, vk_off, list_off, vlist, vk, sec;
    uint8_t *nb, *vb, *lb, *eb;
    uint32_t nlen, vlen, llen;
    tpl.found = CELL_NONE;
    tpl.bad = 0;
    if (each_sub(h, elements, tpl_cb, &tpl) && tpl.bad) return -1;
    eb = cell_body(h, elements, &esz);
    if (!eb) return -1;
    sec = r32(eb + 0x2c);
    if (tpl.found != CELL_NONE) {
        nb = cell_body(h, tpl.found, &nsz);
        if (!nb) return -1;
        nlen = (uint32_t)(-nsz) - 4;
        if (nlen > sizeof nkbuf) return -1;
        memcpy(nkbuf, nb, nlen);
        vlist = r32(nkbuf + 0x28);
        lb = cell_body(h, vlist, &lsz);
        if (!lb) return -1;
        llen = (uint32_t)(-lsz) - 4;
        if (llen < 4 || llen > sizeof listbuf) return -1;
        memcpy(listbuf, lb, llen);
        vk = r32(listbuf);
        vb = cell_body(h, vk, &vsz);
        if (!vb) return -1;
        vlen = (uint32_t)(-vsz) - 4;
        if (vlen > sizeof vkbuf || vlen < 0x14) return -1;
        memcpy(vkbuf, vb, vlen);
    } else {
        memset(nkbuf, 0, sizeof nkbuf);
        memcpy(nkbuf, "nk", 2);
        w16(nkbuf + 2, 0x20);
        w32(nkbuf + 0x1c, CELL_NONE);
        w32(nkbuf + 0x24, 1);
        w32(nkbuf + 0x30, CELL_NONE);
        w16(nkbuf + 0x48, 8);
        nlen = 0x4c + 8;
        nsz = -(int32_t)((nlen + 4u + 7u) & ~7u);
        memset(vkbuf, 0, sizeof vkbuf);
        memcpy(vkbuf, "vk", 2);
        w16(vkbuf + 2, 7);
        w32(vkbuf + 4, 0x80000001u);
        w32(vkbuf + 0x0c, 3);
        w16(vkbuf + 0x10, 1);
        memcpy(vkbuf + 0x14, "Element", 7);
        vlen = 0x14 + 7;
        vsz = -(int32_t)((vlen + 4u + 7u) & ~7u);
        llen = 4;
        lsz = -8;
        memset(listbuf, 0, sizeof listbuf);
    }
    if (alloc_cell(h, (uint32_t)(-vsz), &vk_off)) return -1;
    if (alloc_cell(h, (uint32_t)(-lsz), &list_off)) return -1;
    if (alloc_cell(h, (uint32_t)(-nsz), &nk_off)) return -1;
    vb = cell_body(h, vk_off, &vsz);
    lb = cell_body(h, list_off, &lsz);
    nb = cell_body(h, nk_off, &nsz);
    if (!vb || !lb || !nb) return -1;
    memcpy(vb, vkbuf, vlen);
    memcpy(lb, listbuf, llen);
    memcpy(nb, nkbuf, nlen);
    w32(vb + 4, 0x80000001u);
    w32(vb + 8, 1);
    w32(vb + 0x0c, 3);
    w32(lb, vk_off);
    memcpy(nb + 0x4c, "16000049", 8);
    w16(nb + 0x48, 8);
    w16(nb + 2, 0x20);
    w32(nb + 0x10, elements);
    w32(nb + 0x14, 0);
    w32(nb + 0x18, 0);
    w32(nb + 0x1c, CELL_NONE);
    w32(nb + 0x20, CELL_NONE);
    w32(nb + 0x24, 1);
    w32(nb + 0x28, list_off);
    w32(nb + 0x2c, sec);
    return insert_subkey(h, elements, nk_off, "16000049");
}

static int set_resident_one(Hive *h, uint32_t vk)
{
    int32_t sz;
    uint8_t *vb = cell_body(h, vk, &sz);
    uint32_t dlen;
    if (!vb) return -1;
    dlen = r32(vb + 4);
    if (!(dlen & 0x80000000u) || (dlen & 0x7fffffffu) != 1 || r32(vb + 0x0c) != 3) return -1;
    if (vb[8] == 1) return 0;
    vb[8] = 1;
    return 1;
}

typedef struct {
    int loaders;
    int changed;
    int bad;
} EditCtx;

static int edit_object(Hive *h, uint32_t object, void *v)
{
    EditCtx *e = v;
    uint32_t desc, elements, existing, type = 0, vk = 0, len = 0;
    const uint8_t *data = NULL;
    int bad = 0, rc;
    desc = find_sub(h, object, "Description", &bad);
    if (bad) {
        e->bad = 1;
        return 1;
    }
    if (desc == CELL_NONE) return 0;
    rc = dword_value(h, desc, "Type", &type);
    if (rc < 0) {
        e->bad = 1;
        return 1;
    }
    if (rc == 0 || type != WIN_LOADER) return 0;
    elements = find_sub(h, object, "Elements", &bad);
    if (bad || elements == CELL_NONE) {
        e->bad = 1;
        return 1;
    }
    e->loaders++;
    existing = find_sub(h, elements, "16000049", &bad);
    if (bad) {
        e->bad = 1;
        return 1;
    }
    if (existing != CELL_NONE) {
        rc = value_of(h, existing, "Element", &type, &vk, &data, &len);
        if (rc != 1) {
            e->bad = 1;
            return 1;
        }
        rc = set_resident_one(h, vk);
        if (rc < 0) {
            e->bad = 1;
            return 1;
        }
        if (rc > 0) e->changed = 1;
        return 0;
    }
    if (add_testsigning(h, elements)) {
        e->bad = 1;
        return 1;
    }
    e->changed = 1;
    return 0;
}

static void seal_hive(uint8_t *b)
{
    uint32_t seq = r32(b + 4) + 1;
    uint32_t sum = 0;
    uint32_t i;
    w32(b + 4, seq);
    w32(b + 8, seq);
    for (i = 0; i < 0x1fc; i += 4) sum ^= r32(b + i);
    w32(b + 0x1fc, sum);
}

static int hive_enable(uint8_t *b, size_t n, int *loaders, int *changed)
{
    Hive h;
    EditCtx ctx;
    uint32_t root, objects;
    int bad = 0;
    *loaders = 0;
    *changed = 0;
    if (n < HIVE_BASE + 32 || memcmp(b, "regf", 4) != 0) return -1;
    h.b = b;
    h.n = n;
    root = r32(b + 0x24);
    objects = find_sub(&h, root, "Objects", &bad);
    if (bad || objects == CELL_NONE) return -1;
    ctx.loaders = 0;
    ctx.changed = 0;
    ctx.bad = 0;
    if (each_sub(&h, objects, edit_object, &ctx) && ctx.bad) return -1;
    if (ctx.bad) return -1;
    *loaders = ctx.loaders;
    *changed = ctx.changed;
    if (ctx.changed) seal_hive(b);
    return 0;
}

typedef struct {
    int fd;
    uint64_t data_off;
    uint32_t cluster;
    uint8_t *fat;
    uint32_t fat_bytes;
    uint32_t root;
} Fat;

static uint32_t fat_next(const Fat *f, uint32_t c)
{
    if ((uint64_t)c * 4u + 4u > f->fat_bytes) return 0x0fffffffu;
    return r32(f->fat + (uint64_t)c * 4u) & 0x0fffffffu;
}

static uint8_t short_sum(const uint8_t *e)
{
    uint8_t sum = 0;
    int i;
    for (i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + e[i]);
    return sum;
}

static void pack83(const uint8_t *e, char *out)
{
    int i, n = 0;
    for (i = 0; i < 8 && e[i] != ' '; i++) out[n++] = (char)e[i];
    if (e[8] != ' ') {
        out[n++] = '.';
        for (i = 8; i < 11 && e[i] != ' '; i++) out[n++] = (char)e[i];
    }
    out[n] = 0;
}

static int dir_lookup(Fat *f, uint32_t dir, const char *want, int want_dir, uint32_t *out_cl, uint32_t *out_sz)
{
    uint8_t *chunk = malloc(f->cluster);
    uint16_t lfn[260];
    int lfn_ok = 0, lfn_chars = 0;
    uint8_t expect = 0;
    uint32_t c = dir;
    int guard = 0;
    if (!chunk) return -1;
    while (c >= 2 && c < 0x0ffffff8u && guard++ < 100000) {
        uint32_t i;
        if (read_full(f->fd, f->data_off + (uint64_t)(c - 2) * f->cluster, chunk, f->cluster)) {
            free(chunk);
            return -1;
        }
        for (i = 0; i + 32 <= f->cluster; i += 32) {
            uint8_t *e = chunk + i;
            char shortn[16], longn[256];
            int is_dir, match;
            if (e[0] == 0) {
                free(chunk);
                return 1;
            }
            if (e[0] == 0xe5) {
                lfn_ok = 0;
                continue;
            }
            if ((e[11] & 0x3f) == 0x0f) {
                int seq = e[0] & 0x1f;
                if (seq < 1 || seq > 20) {
                    lfn_ok = 0;
                    continue;
                }
                if (e[0] & 0x40) {
                    memset(lfn, 0xff, sizeof lfn);
                    lfn_chars = seq * 13;
                    lfn_ok = 1;
                    expect = e[13];
                }
                if (!lfn_ok || e[13] != expect) {
                    lfn_ok = 0;
                    continue;
                }
                memcpy(lfn + (seq - 1) * 13, e + 1, 10);
                memcpy(lfn + (seq - 1) * 13 + 5, e + 14, 12);
                memcpy(lfn + (seq - 1) * 13 + 11, e + 28, 4);
                continue;
            }
            if (e[11] & 0x08) {
                lfn_ok = 0;
                continue;
            }
            is_dir = (e[11] & 0x10) != 0;
            pack83(e, shortn);
            longn[0] = 0;
            if (lfn_ok && short_sum(e) == expect) {
                int n = 0, k;
                for (k = 0; k < lfn_chars && n < 255; k++) {
                    if (lfn[k] == 0 || lfn[k] == 0xffff) break;
                    if (lfn[k] > 127) {
                        n = -1;
                        break;
                    }
                    longn[n++] = (char)lfn[k];
                }
                if (n >= 0) longn[n] = 0;
            }
            lfn_ok = 0;
            match = name_cmp(shortn, want) == 0 || (longn[0] && name_cmp(longn, want) == 0);
            if (!match || want_dir != is_dir) continue;
            *out_cl = (uint32_t)r16(e + 26) | ((uint32_t)r16(e + 20) << 16);
            *out_sz = r32(e + 28);
            free(chunk);
            return 0;
        }
        c = fat_next(f, c);
    }
    free(chunk);
    return 1;
}

static int lookup_bcd(Fat *f, uint32_t *cl, uint32_t *sz)
{
    static const char *parts[] = {"EFI", "Microsoft", "Boot", "BCD"};
    uint32_t dir = f->root;
    int i;
    for (i = 0; i < 4; i++) {
        uint32_t next = 0, size = 0;
        int rc = dir_lookup(f, dir, parts[i], i < 3, &next, &size);
        if (rc) return rc;
        if (i == 3) {
            *cl = next;
            *sz = size;
            return 0;
        }
        dir = next;
    }
    return -1;
}

static int chain_io(Fat *f, uint32_t cl, uint8_t *buf, uint32_t size, int write)
{
    uint8_t *chunk = malloc(f->cluster);
    uint32_t pos = 0;
    int guard = 0;
    if (!chunk) return -1;
    while (pos < size) {
        uint32_t n;
        if (cl < 2 || cl >= 0x0ffffff8u || guard++ > 100000) {
            free(chunk);
            return -1;
        }
        n = f->cluster;
        if (n > size - pos) n = size - pos;
        if (!write) {
            if (read_full(f->fd, f->data_off + (uint64_t)(cl - 2) * f->cluster, chunk, f->cluster)) {
                free(chunk);
                return -1;
            }
            memcpy(buf + pos, chunk, n);
        } else {
            memset(chunk, 0, f->cluster);
            if (n < f->cluster &&
                read_full(f->fd, f->data_off + (uint64_t)(cl - 2) * f->cluster, chunk, f->cluster)) {
                free(chunk);
                return -1;
            }
            memcpy(chunk, buf + pos, n);
            if (write_full(f->fd, f->data_off + (uint64_t)(cl - 2) * f->cluster, chunk, f->cluster)) {
                free(chunk);
                return -1;
            }
        }
        pos += n;
        cl = fat_next(f, cl);
    }
    free(chunk);
    return 0;
}

static int open_fat(int fd, Fat *f, uint8_t **fat_out)
{
    uint8_t sector[4096];
    uint8_t hdr[512];
    uint64_t entry_lba;
    uint32_t entry_bytes, i, nent, entsz, bps, spc, reserved, nfats, fatsz;
    const uint8_t esp[16] = {
        0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
        0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
    };
    memset(f, 0, sizeof *f);
    f->fd = fd;
    if (read_full(fd, 512, hdr, sizeof hdr) || memcmp(hdr, "EFI PART", 8) != 0) return 1;
    entry_lba = r64(hdr + 0x48);
    nent = r32(hdr + 0x50);
    entsz = r32(hdr + 0x54);
    if (entsz < 128 || nent == 0 || nent > 4096) return -1;
    entry_bytes = nent * entsz;
    {
        uint8_t *ents = malloc(entry_bytes);
        int found = 0;
        uint64_t part = 0;
        if (!ents) return -1;
        if (read_full(fd, entry_lba * 512, ents, entry_bytes)) {
            free(ents);
            return -1;
        }
        for (i = 0; i < nent; i++) {
            uint8_t *e = ents + (uint64_t)i * entsz;
            int unused = 1, k;
            for (k = 0; k < 16; k++) if (e[k]) unused = 0;
            if (unused || memcmp(e, esp, 16) != 0) continue;
            part = r64(e + 32) * 512;
            found = 1;
            break;
        }
        free(ents);
        if (!found) return 1;
        if (read_full(fd, part, sector, 512)) return -1;
        if (r16(sector + 22) != 0 || memcmp(sector + 82, "FAT32   ", 8) != 0) return 1;
        bps = r16(sector + 11);
        spc = sector[13];
        reserved = r16(sector + 14);
        nfats = sector[16];
        fatsz = r32(sector + 36);
        f->root = r32(sector + 44);
        if ((bps != 512 && bps != 4096) || spc == 0 || nfats == 0 || fatsz == 0 || f->root < 2)
            return -1;
        f->cluster = bps * spc;
        if (f->cluster == 0 || f->cluster > 65536) return -1;
        f->fat_bytes = fatsz * bps;
        if (f->fat_bytes > 16u * 1024u * 1024u) return -1;
        f->fat = malloc(f->fat_bytes);
        if (!f->fat) return -1;
        if (read_full(fd, part + (uint64_t)reserved * bps, f->fat, f->fat_bytes)) return -1;
        f->data_off = part + (uint64_t)(reserved + nfats * fatsz) * bps;
        *fat_out = f->fat;
    }
    return 0;
}

int hw_bcd_enable_testsigning(const char *disk_path, char *status, size_t status_cap)
{
    int fd, fat_rc, look, loaders = 0, changed = 0;
    Fat fat;
    uint8_t *owned_fat = NULL;
    uint8_t *bcd = NULL;
    uint32_t cl = 0, sz = 0;
    set_status(status, status_cap, "Could not update the BCD.");
    if (!disk_path) return -1;
    fd = open(disk_path, O_RDWR);
    if (fd < 0) {
        set_status(status, status_cap, "Could not read the Windows disk.");
        return -1;
    }
    fat_rc = open_fat(fd, &fat, &owned_fat);
    if (fat_rc != 0) {
        free(fat.fat);
        close(fd);
        if (fat_rc < 0) return -1;
        set_status(status, status_cap, "No Windows BCD on this disk yet.");
        return 1;
    }
    look = lookup_bcd(&fat, &cl, &sz);
    if (look != 0) {
        free(fat.fat);
        close(fd);
        if (look < 0) return -1;
        set_status(status, status_cap, "No Windows BCD on this disk yet.");
        return 1;
    }
    if (sz < HIVE_BASE + 32 || sz > 1024u * 1024u) {
        free(fat.fat);
        close(fd);
        return -1;
    }
    bcd = malloc(sz);
    if (!bcd || chain_io(&fat, cl, bcd, sz, 0) || hive_enable(bcd, sz, &loaders, &changed)) {
        free(bcd);
        free(fat.fat);
        close(fd);
        return -1;
    }
    if (loaders == 0) {
        free(bcd);
        free(fat.fat);
        close(fd);
        set_status(status, status_cap, "No Windows boot entry in the BCD.");
        return 1;
    }
    if (changed && chain_io(&fat, cl, bcd, sz, 1)) {
        free(bcd);
        free(fat.fat);
        close(fd);
        return -1;
    }
    if (changed && fsync(fd)) {
        free(bcd);
        free(fat.fat);
        close(fd);
        return -1;
    }
    free(bcd);
    free(fat.fat);
    close(fd);
    set_status(status, status_cap,
               changed ? "Test signing is on in the installed BCD." : "Test signing was already on.");
    return 0;
}

#ifdef HW_BCD_TEST
static void put_ent(uint8_t *e, const char *name11, uint8_t attr, uint32_t cluster, uint32_t size)
{
    memset(e, ' ', 11);
    memcpy(e, name11, strlen(name11) > 11 ? 11 : strlen(name11));
    e[11] = attr;
    w16(e + 20, (uint16_t)(cluster >> 16));
    w16(e + 26, (uint16_t)cluster);
    w32(e + 28, size);
}

static int build_image(const char *path, const uint8_t *bcd, uint32_t bcd_len)
{
    const uint32_t bps = 512, spc = 8, reserved = 32, nfats = 2, fatsz = 16, part_lba = 2048;
    uint32_t file_clusters = (bcd_len + 4095u) / 4096u;
    uint32_t vol_sectors = reserved + nfats * fatsz + (5u + file_clusters) * spc + 32u;
    uint64_t disk_bytes = (uint64_t)(part_lba + vol_sectors + 64u) * bps;
    uint8_t *img = calloc(1, (size_t)disk_bytes);
    uint8_t *vol, *fat, *data, *lfn, sum;
    uint32_t c, i;
    uint16_t chars[13];
    int fd;
    if (!img) return -1;
    img[510] = 0x55;
    img[511] = 0xaa;
    memcpy(img + 512, "EFI PART", 8);
    w32(img + 512 + 0x0c, 92);
    w32(img + 512 + 0x48, 2);
    w32(img + 512 + 0x50, 128);
    w32(img + 512 + 0x54, 128);
    {
        uint8_t *e = img + 2 * 512;
        static const uint8_t esp[16] = {
            0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
            0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
        };
        memcpy(e, esp, 16);
        w32(e + 32, part_lba);
        w32(e + 40, part_lba + vol_sectors - 1);
    }
    vol = img + (uint64_t)part_lba * bps;
    vol[0] = 0xeb;
    vol[1] = 0x58;
    vol[2] = 0x90;
    memcpy(vol + 3, "MSDOS5.0", 8);
    w16(vol + 11, 512);
    vol[13] = (uint8_t)spc;
    w16(vol + 14, (uint16_t)reserved);
    vol[16] = (uint8_t)nfats;
    vol[21] = 0xf8;
    w32(vol + 32, vol_sectors);
    w32(vol + 36, fatsz);
    w32(vol + 44, 2);
    vol[66] = 0x29;
    memcpy(vol + 82, "FAT32   ", 8);
    vol[510] = 0x55;
    vol[511] = 0xaa;
    fat = vol + reserved * bps;
    w32(fat + 0, 0x0ffffff8u);
    w32(fat + 4, 0x0fffffffu);
    for (c = 2; c <= 5; c++) w32(fat + c * 4, 0x0fffffffu);
    for (c = 6; c < 6 + file_clusters - 1; c++) w32(fat + c * 4, c + 1);
    w32(fat + (6 + file_clusters - 1) * 4, 0x0fffffffu);
    memcpy(fat + fatsz * bps, fat, fatsz * bps);
    data = vol + (reserved + nfats * fatsz) * bps;
    put_ent(data + 0, "EFI", 0x10, 3, 0);
    put_ent(data + 4096 + 0, ".", 0x10, 3, 0);
    put_ent(data + 4096 + 32, "..", 0x10, 2, 0);
    lfn = data + 4096 + 64;
    memset(lfn, 0xff, 32);
    lfn[0] = 0x41;
    lfn[11] = 0x0f;
    lfn[13] = 0;
    memset(chars, 0xff, sizeof chars);
    for (i = 0; i < 9; i++) chars[i] = (uint16_t)"Microsoft"[i];
    chars[9] = 0;
    memcpy(lfn + 1, chars, 10);
    memcpy(lfn + 14, chars + 5, 12);
    memcpy(lfn + 28, chars + 11, 4);
    put_ent(data + 4096 + 96, "MICROS~1", 0x10, 4, 0);
    sum = short_sum(data + 4096 + 96);
    lfn[13] = sum;
    put_ent(data + 8192 + 0, ".", 0x10, 4, 0);
    put_ent(data + 8192 + 32, "..", 0x10, 3, 0);
    put_ent(data + 8192 + 64, "BOOT", 0x10, 5, 0);
    put_ent(data + 12288 + 0, ".", 0x10, 5, 0);
    put_ent(data + 12288 + 32, "..", 0x10, 4, 0);
    put_ent(data + 12288 + 64, "BCD", 0x20, 6, bcd_len);
    memcpy(data + 4 * 4096, bcd, bcd_len);
    fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0 || write_full(fd, 0, img, (size_t)disk_bytes)) {
        if (fd >= 0) close(fd);
        free(img);
        return -1;
    }
    close(fd);
    free(img);
    return 0;
}

static int read_file(const char *path, uint8_t **out, uint32_t *sz)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return -1;
    }
    *out = malloc((size_t)n);
    *sz = (uint32_t)n;
    if (!*out || fread(*out, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static int extract_bcd(const char *disk, uint8_t **out, uint32_t *sz)
{
    int fd = open(disk, O_RDONLY);
    Fat fat;
    uint32_t cl = 0, size = 0;
    uint8_t *owned = NULL;
    int rc;
    if (fd < 0) return -1;
    rc = open_fat(fd, &fat, &owned);
    if (rc || lookup_bcd(&fat, &cl, &size)) {
        free(fat.fat);
        close(fd);
        return -1;
    }
    *out = malloc(size);
    *sz = size;
    rc = !*out || chain_io(&fat, cl, *out, size, 0);
    free(fat.fat);
    close(fd);
    return rc ? -1 : 0;
}

static int expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "fail: %s\n", msg);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    uint8_t *orig = NULL, *edited = NULL, *from_disk = NULL, *second = NULL;
    uint32_t sz = 0, got = 0, got2 = 0;
    int loaders = 0, changed = 0;
    char status[128];
    const char *image = "/tmp/hw-bcd-test.img";
    if (argc != 2) {
        fprintf(stderr, "usage: bcd_testsign BCD-original\n");
        return 2;
    }
    if (read_file(argv[1], &orig, &sz) || build_image(image, orig, sz)) return 1;
    if (expect(hw_bcd_enable_testsigning(image, status, sizeof status) == 0, status)) return 1;
    if (expect(strcmp(status, "Test signing is on in the installed BCD.") == 0, status)) return 1;
    if (extract_bcd(image, &from_disk, &got) || hive_enable(from_disk, got, &loaders, &changed)) return 1;
    if (expect(loaders >= 1 && changed == 0, "loaders not updated")) return 1;
    edited = malloc(got);
    if (!edited) return 1;
    memcpy(edited, from_disk, got);
    if (expect(hw_bcd_enable_testsigning(image, status, sizeof status) == 0, status)) return 1;
    if (expect(strcmp(status, "Test signing was already on.") == 0, status)) return 1;
    if (extract_bcd(image, &second, &got2) || got2 != got || memcmp(second, edited, got) != 0) {
        fprintf(stderr, "fail: second pass rewrote the BCD\n");
        return 1;
    }
    printf("%s (%d loaders)\n", status, loaders);
    free(orig);
    free(edited);
    free(from_disk);
    free(second);
    return 0;
}
#endif
