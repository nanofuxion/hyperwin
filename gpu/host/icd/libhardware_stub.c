// libhardware.so for drivers added from files. Vulkan drivers built as Android HAL modules (PanVK
// and Turnip builds for Winlator) link to libhardware, which Android keeps from apps. They only use
// it to find gralloc for Android window surfaces, which gfxstream never asks for, so every lookup
// fails here the way it does on a device without the module.
#include <errno.h>

struct hw_module_t;

__attribute__((visibility("default"))) int hw_get_module(const char* id, const struct hw_module_t** module) {
    (void)id;
    *module = 0;
    return -ENOENT;
}

__attribute__((visibility("default"))) int hw_get_module_by_class(const char* class_id, const char* inst,
                                                                  const struct hw_module_t** module) {
    (void)class_id;
    (void)inst;
    *module = 0;
    return -ENOENT;
}
