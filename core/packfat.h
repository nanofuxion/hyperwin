#ifndef HYPERWIN_PACKFAT_H
#define HYPERWIN_PACKFAT_H

/* Superfloppy FAT16 image of a directory tree. Returns 0 on success. */
int hw_pack_tree(const char *dir, const char *image_path);

#endif
