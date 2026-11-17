/* Stands in for Vortek's vulkan_helper.h: engine ids, and the one driver call the inspector makes. */
#ifndef VULKAN_HELPER_H
#define VULKAN_HELPER_H

#include "vortek.h"

#define MAKE_ENGINE_VERSION(x, y, z) (((x) << 22) | ((y) << 12) | (z))
#define VK_ENGINE_ID_UNKNOWN 0
#define VK_ENGINE_ID_DXVK 1
#define VK_ENGINE_ID_VKD3D 2
#define VK_ENGINE_ID_ZINK 3

typedef struct VulkanWrapper {
    PFN_vkCreateShaderModule vkCreateShaderModule;
} VulkanWrapper;
extern VulkanWrapper vulkanWrapper;  /* vortek.cpp: forwards to the device's driver */

#endif
