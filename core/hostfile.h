#ifndef HYPERWIN_HOSTFILE_H
#define HYPERWIN_HOSTFILE_H

#include "hw_abi.h"

/* Read up to len bytes of path at offset. On STAT (len request of zero via hw_file_poll),
 * *size is the file length. Return 0 or a negative errno. */
typedef int (*hw_file_fn)(const char *path, uint64_t offset, void *dst, uint32_t len,
                          uint64_t *size, void *user);

/* Guest side. path is a single name, no directories. */
int hw_host_stat(void *window, const char *path, uint64_t *size);
int hw_host_read(void *window, const char *path, uint64_t offset, void *dst, uint32_t len);

/* Host side. Answers one posted request. Returns 1 if it did. */
int hw_file_poll(void *window, hw_file_fn fn, void *user);

#endif
