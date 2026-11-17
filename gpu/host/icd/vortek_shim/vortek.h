/* Stands in for Vortek's server vortek.h when its shader_inspector.c is built inside
 * libvulkan_vortek.so: the few types the inspector reads, none of Vortek's transport. */
#ifndef VORTEK_H
#define VORTEK_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "winlator.h"
#include "arrays.h"
#include "string_utils.h"

/* What ShaderInspector_create reads from Vortek's VkContext. */
typedef struct VkContext {
    VkDriverId driverID;
    int engineID;
    uint32_t engineVersion;
} VkContext;

#endif
