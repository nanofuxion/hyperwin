// libvulkan_bcn.so: BC textures for GPUs without them (Mali), under gfxstream, and nothing else.
//
// BC textures are decoded on the GPU by bcn_layer (github.com/Pipetto-crypto/bcn_layer, MIT,
// Granite's compute shaders), the same layer GameNative ships on top of its own (closed) wrapper.
// This wrapper loads it between gfxstream and the phone's driver, from libbcn_layer.so beside this
// library, unless HWGPU_VK_LAYERS says otherwise. The layer itself runs unchanged.
//
// The layer only watches vkCmdCopyBufferToImage, and DXVK uploads with vkCmdCopyBufferToImage2.
// The copy2 calls are turned into the plain call here so BC uploads reach it.
#include <vector>

#include "wrap.h"

namespace {

// Runs when the library loads, before gfxstream asks for any entry point.
__attribute__((constructor)) void load_bcn_layer() { wrap::default_bcn_layer(reinterpret_cast<void*>(&load_bcn_layer)); }

VKAPI_ATTR void VKAPI_CALL copy_buffer_to_image2(VkCommandBuffer cmd, const VkCopyBufferToImageInfo2* info) {
    VkDevice dev = wrap::device_of(cmd);
    bool plain = !info->pNext;
    for (uint32_t i = 0; plain && i < info->regionCount; i++) plain = !info->pRegions[i].pNext;
    if (!plain) {
        NEXT_D(dev, vkCmdCopyBufferToImage2)(cmd, info);
        return;
    }
    std::vector<VkBufferImageCopy> regions(info->regionCount);
    for (uint32_t i = 0; i < info->regionCount; i++) {
        const VkBufferImageCopy2& r = info->pRegions[i];
        regions[i] = {r.bufferOffset, r.bufferRowLength, r.bufferImageHeight, r.imageSubresource, r.imageOffset, r.imageExtent};
    }
    NEXT_D(dev, vkCmdCopyBufferToImage)(cmd, info->srcBuffer, info->dstImage, info->dstImageLayout,
                                        info->regionCount, regions.data());
}

}  // namespace

namespace wrap {
const Hook* hooks() {
    static const Hook table[] = {
        {"vkCmdCopyBufferToImage2", reinterpret_cast<PFN_vkVoidFunction>(copy_buffer_to_image2)},
        {"vkCmdCopyBufferToImage2KHR", reinterpret_cast<PFN_vkVoidFunction>(copy_buffer_to_image2)},
        {nullptr, nullptr},
    };
    return table;
}
}  // namespace wrap
