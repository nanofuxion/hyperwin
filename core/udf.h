#ifndef HW_UDF_H
#define HW_UDF_H

#include <stdint.h>

/* Locates a contiguous file in the UDF tree of an ISO; path like "sources/install.esd",
 * matched case-insensitively. Returns 0 and the byte offset and size, or -1. */
int hw_udf_find(int fd, const char *path, uint64_t *offset, uint64_t *size);

#endif
