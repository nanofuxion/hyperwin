/* QEMU shared-memory GOP setup. The address is supplied at build time. */
#include "hw_abi.h"
typedef uint64_t (*allocate_pages_fn)(uint32_t, uint32_t, uint64_t, uint64_t *);
uint64_t QemuRingEntry(void *image, void *system)
{
    struct hw_header *h = (struct hw_header *)HW_WINDOW_PA;
    void *bs = *(void **)((uint8_t *)system + 0x60);
    allocate_pages_fn allocate = *(allocate_pages_fn *)((uint8_t *)bs + 24 + 2 * 8);
    uint64_t address = HW_WINDOW_PA, status;
    (void)image;
    if (h->magic[0] != 'H' || h->magic[1] != 'Y' || h->version != HW_VERSION)
        return 0x8000000000000002ull;
    status = allocate(2, 0, HW_WINDOW_SIZE / 4096, &address);
    if (status || address != HW_WINDOW_PA) return status ? status : 0x8000000000000002ull;
    __atomic_or_fetch(&h->guest_flags, HW_GUEST_RING_RESERVED, __ATOMIC_RELEASE);
    return 0;
}
uint64_t HyperwinInstallAcpi(void *system) { (void)system; return 0; }
void HyperwinTraceInstall(void *system) { (void)system; }
void HyperwinTraceNote(const char *what, uint64_t a, uint64_t b, uint64_t c)
{ (void)what; (void)a; (void)b; (void)c; }
