#ifndef HW_HIVEX_C_CTYPE_H
#define HW_HIVEX_C_CTYPE_H
static inline int c_isprint(int c) { return c >= 0x20 && c < 0x7f; }
static inline int c_toupper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
#endif
