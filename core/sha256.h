#ifndef HYPERWIN_SHA256_H
#define HYPERWIN_SHA256_H

#include <stdint.h>
#include <stddef.h>

void hw_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

#endif
