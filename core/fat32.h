#ifndef HW_FAT32_H
#define HW_FAT32_H

#include <stdint.h>

/* Formats [offset, offset + sectors*512) of fd as FAT32 (4 KiB clusters) holding the tree at dir.
 * Needs at least ~260 MiB. Returns 0 or -1. */
int hw_fat32_write(const char *dir, int fd, uint64_t offset, uint64_t sectors, const char *label);

/* Replaces the contents of an existing file in the FAT32 volume at offset. path uses '/' and is
 * matched without case against long or 8.3 names. The file may grow or shrink. Returns 0 or -1. */
/* The contents of a file in the FAT32 volume at offset, malloc'd, or NULL. */
void *hw_fat32_read(int fd, uint64_t offset, const char *path, uint32_t *len);

int hw_fat32_replace(int fd, uint64_t offset, const char *path, const void *data, uint32_t len);

#endif
