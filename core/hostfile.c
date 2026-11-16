#include "hostfile.h"

#include <string.h>

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

static int plain_name(const char *path, uint32_t n)
{
    uint32_t i;
    if (!n || n > 255) return 0;
    for (i = 0; i < n; i++) {
        if (path[i] == '/' || path[i] == '\\' || path[i] == 0) return 0;
    }
    if (n == 1 && path[0] == '.') return 0;
    if (n == 2 && path[0] == '.' && path[1] == '.') return 0;
    return 1;
}

static struct hw_file_mbox *mbox(void *window)
{
    return (struct hw_file_mbox *)((uint8_t *)window + HW_FILES);
}

static int call(void *window, uint32_t op, const char *path, uint64_t offset,
                void *dst, uint32_t len, uint64_t *size)
{
    struct hw_file_mbox *m = mbox(window);
    uint32_t seq, n, i;
    uint8_t req[HW_FILE_REQ];
    n = (uint32_t)strlen(path);
    if (!plain_name(path, n) || 20u + n > HW_FILE_REQ) return -22;
    put32(req, HW_PAD_MAGIC);
    put32(req + 4, op);
    put64(req + 8, offset);
    put32(req + 16, len);
    for (i = 0; i < n; i++) req[20 + i] = (uint8_t)path[i];
    for (i = 0; i < 20 + n; i++) m->req[i] = req[i];
    m->req_len = 20 + n;
    seq = __atomic_add_fetch(&m->req_seq, 1, __ATOMIC_RELEASE);
    for (i = 0; i < 5000000u; i++) {
        if (__atomic_load_n(&m->resp_seq, __ATOMIC_ACQUIRE) == seq) {
            if (m->status != 0) return m->status;
            if (size) *size = get64(m->resp);
            if (dst && m->resp_len > 8) {
                uint32_t got = m->resp_len - 8;
                if (got > len) got = len;
                memcpy(dst, m->resp + 8, got);
                return (int)got;
            }
            return 0;
        }
    }
    return -110;
}

int hw_host_stat(void *window, const char *path, uint64_t *size)
{
    return call(window, HW_FILE_STAT, path, 0, 0, 0, size);
}

int hw_host_read(void *window, const char *path, uint64_t offset, void *dst, uint32_t len)
{
    if (!dst || !len || len > HW_FILE_RESP - 8) return -22;
    return call(window, HW_FILE_READ, path, offset, dst, len, 0);
}

int hw_file_poll(void *window, hw_file_fn fn, void *user)
{
    struct hw_file_mbox *m = mbox(window);
    uint32_t req, len, op, path_len, want;
    uint64_t offset, size = 0;
    char path[256];
    uint32_t i;
    int rc;
    if (!window || !fn) return 0;
    req = __atomic_load_n(&m->req_seq, __ATOMIC_ACQUIRE);
    if (req == __atomic_load_n(&m->resp_seq, __ATOMIC_RELAXED)) return 0;
    len = m->req_len;
    if (len < 20 || len > HW_FILE_REQ || get32(m->req) != HW_PAD_MAGIC) {
        m->status = -22;
        m->resp_len = 0;
        __atomic_store_n(&m->resp_seq, req, __ATOMIC_RELEASE);
        return 1;
    }
    op = get32(m->req + 4);
    offset = get64(m->req + 8);
    want = get32(m->req + 16);
    path_len = len - 20;
    if (path_len >= sizeof path || !plain_name((const char *)m->req + 20, path_len)) {
        m->status = -22;
        m->resp_len = 0;
        __atomic_store_n(&m->resp_seq, req, __ATOMIC_RELEASE);
        return 1;
    }
    for (i = 0; i < path_len; i++) path[i] = (char)m->req[20 + i];
    path[path_len] = 0;
    if (op == HW_FILE_STAT) {
        rc = fn(path, 0, 0, 0, &size, user);
        m->status = rc;
        if (rc == 0) {
            put64(m->resp, size);
            m->resp_len = 8;
        } else m->resp_len = 0;
    } else if (op == HW_FILE_READ) {
        if (want > HW_FILE_RESP - 8) want = HW_FILE_RESP - 8;
        rc = fn(path, offset, m->resp + 8, want, &size, user);
        m->status = rc < 0 ? rc : 0;
        if (rc >= 0) {
            put64(m->resp, size);
            m->resp_len = 8 + (uint32_t)rc;
        } else m->resp_len = 0;
    } else {
        m->status = -38;
        m->resp_len = 0;
    }
    __atomic_store_n(&m->resp_seq, req, __ATOMIC_RELEASE);
    return 1;
}
