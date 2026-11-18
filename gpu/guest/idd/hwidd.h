// HyperWin's indirect display driver (gpu/guest/idd, wddm/README.md phase 6): a UMDF 2 driver on IddCx.
#pragma once
#include <wdf.h>
#include <IddCx.h>
#include <stddef.h>

void hwidd_log(const char *fmt, ...);
