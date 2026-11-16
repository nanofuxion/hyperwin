#ifndef HYPERWIN_BCD_TESTSIGN_H
#define HYPERWIN_BCD_TESTSIGN_H

#include <stddef.h>

/* Turn on BCD test signing (element 16000049) for every Windows boot loader
 * on the installed system disk. The caller's ISO is not opened. Returns 0
 * when test signing is on, 1 when the disk has no installed BCD yet, and -1
 * when the store could not be updated. status is always NUL-terminated. */
int hw_bcd_enable_testsigning(const char *disk_path, char *status, size_t status_cap);

#endif
