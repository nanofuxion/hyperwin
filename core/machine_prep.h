#ifndef HW_MACHINE_PREP_H
#define HW_MACHINE_PREP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *iso_path;       /* readable path, e.g. /proc/self/fd/N */
    const char *assets_dir;     /* drivers.json, drivers/, efi/, BCD, unattend.xml */
    const char *disk_path;      /* system.img to create */
    const char *work_dir;       /* scratch space, removed afterwards */
    uint64_t disk_bytes;
    int image_index;
    const char *username, *password, *computer_name;
    void (*progress)(void *ctx, int percent, const char *message);
    void *ctx;
} HwPrepRequest;

/* Where the boot (EFI system) partition starts in every disk this makes. */
#define HW_PREP_ESP_OFFSET (2048ull * 512)

/* Keeps Windows from diverting into its recovery environment (BCD store at path).
 * Returns 1 changed, 0 already so, -1 error. */
int hw_bcd_harden(const char *path);

/* Builds a bootable Windows disk image from an install ISO. Returns 0, or -1 with a message. */
int hw_prepare_machine(const HwPrepRequest *req, char *err, size_t errlen);

#endif
