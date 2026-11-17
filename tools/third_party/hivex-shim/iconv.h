#ifndef HW_HIVEX_ICONV_H
#define HW_HIVEX_ICONV_H
/* The four conversions hivex asks iconv for (UTF-8 <-> LATIN1, UTF-8 <-> UTF-16LE).
 * Android's iconv has no LATIN1, which made hivex miss existing keys and add duplicates. */
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int from, to; } *iconv_t;  /* 0 UTF-8, 1 LATIN1, 2 UTF-16LE */

static inline int hw_enc(const char *name)
{
    if (!strcmp(name, "UTF-8")) return 0;
    if (!strcmp(name, "LATIN1")) return 1;
    if (!strcmp(name, "UTF-16LE")) return 2;
    return -1;
}

static inline iconv_t hw_iconv_open(const char *to, const char *from)
{
    iconv_t cd;
    if (hw_enc(to) < 0 || hw_enc(from) < 0) { errno = EINVAL; return (iconv_t)-1; }
    cd = malloc(sizeof *cd);
    if (!cd) return (iconv_t)-1;
    cd->from = hw_enc(from);
    cd->to = hw_enc(to);
    return cd;
}

static inline int hw_iconv_close(iconv_t cd) { free(cd); return 0; }

/* Decodes one code point; returns bytes consumed, 0 if more input is needed, -1 if invalid. */
static inline int hw_decode(int enc, const unsigned char *p, size_t n, unsigned *cp)
{
    if (!n) return 0;
    if (enc == 1) { *cp = p[0]; return 1; }
    if (enc == 2) {
        unsigned u, v;
        if (n < 2) return 0;
        u = p[0] | p[1] << 8;
        if (u < 0xd800 || u > 0xdfff) { *cp = u; return 2; }
        if (u > 0xdbff) return -1;
        if (n < 4) return 0;
        v = p[2] | p[3] << 8;
        if (v < 0xdc00 || v > 0xdfff) return -1;
        *cp = 0x10000 + ((u - 0xd800) << 10) + (v - 0xdc00);
        return 4;
    }
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    {
        int len = p[0] >= 0xf0 ? 4 : p[0] >= 0xe0 ? 3 : p[0] >= 0xc0 ? 2 : -1, i;
        if (len < 0) return -1;
        if ((int)n < len) return 0;
        *cp = p[0] & (0x7f >> len);
        for (i = 1; i < len; i++) {
            if ((p[i] & 0xc0) != 0x80) return -1;
            *cp = *cp << 6 | (p[i] & 0x3f);
        }
        return len;
    }
}

/* Encodes one code point; returns bytes written, 0 if no room, -1 if not representable. */
static inline int hw_encode(int enc, unsigned cp, unsigned char *o, size_t n)
{
    if (enc == 1) {
        if (cp > 0xff) return -1;
        if (n < 1) return 0;
        o[0] = (unsigned char)cp;
        return 1;
    }
    if (enc == 2) {
        if (cp < 0x10000) {
            if (n < 2) return 0;
            o[0] = (unsigned char)cp; o[1] = (unsigned char)(cp >> 8);
            return 2;
        }
        if (n < 4) return 0;
        cp -= 0x10000;
        o[0] = (unsigned char)(0xd800 + (cp >> 10)); o[1] = (unsigned char)((0xd800 + (cp >> 10)) >> 8);
        o[2] = (unsigned char)(0xdc00 + (cp & 0x3ff)); o[3] = (unsigned char)((0xdc00 + (cp & 0x3ff)) >> 8);
        return 4;
    }
    if (cp < 0x80) { if (n < 1) return 0; o[0] = (unsigned char)cp; return 1; }
    if (cp < 0x800) { if (n < 2) return 0; o[0] = (unsigned char)(0xc0 | cp >> 6); o[1] = (unsigned char)(0x80 | (cp & 0x3f)); return 2; }
    if (cp < 0x10000) {
        if (n < 3) return 0;
        o[0] = (unsigned char)(0xe0 | cp >> 12); o[1] = (unsigned char)(0x80 | (cp >> 6 & 0x3f)); o[2] = (unsigned char)(0x80 | (cp & 0x3f));
        return 3;
    }
    if (n < 4) return 0;
    o[0] = (unsigned char)(0xf0 | cp >> 18); o[1] = (unsigned char)(0x80 | (cp >> 12 & 0x3f));
    o[2] = (unsigned char)(0x80 | (cp >> 6 & 0x3f)); o[3] = (unsigned char)(0x80 | (cp & 0x3f));
    return 4;
}

static inline size_t hw_iconv(iconv_t cd, char **in, size_t *inleft, char **out, size_t *outleft)
{
    if (!in || !*in) return 0;
    while (*inleft) {
        unsigned cp;
        int r = hw_decode(cd->from, (const unsigned char *)*in, *inleft, &cp), w;
        if (r == 0) { errno = EINVAL; return (size_t)-1; }
        if (r < 0) { errno = EILSEQ; return (size_t)-1; }
        w = hw_encode(cd->to, cp, (unsigned char *)*out, *outleft);
        if (w == 0) { errno = E2BIG; return (size_t)-1; }
        if (w < 0) { errno = EILSEQ; return (size_t)-1; }
        *in += r; *inleft -= (size_t)r;
        *out += w; *outleft -= (size_t)w;
    }
    return 0;
}

#define iconv_open hw_iconv_open
#define iconv_close hw_iconv_close
#define iconv hw_iconv
#endif
