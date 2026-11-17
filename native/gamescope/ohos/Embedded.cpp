// SPDX-License-Identifier: LGPL-2.1-or-later
#include "rendervulkan.hpp"
#include <vulkan/vulkan_ohos.h>

extern bool OhosNativeBufferReference(void*);

bool CVulkanTexture::BInitNativeBuffer(void* buffer, uint32_t width, uint32_t height) {
    if (!buffer || !width || !height || width > 8192 || height > 8192) return false;
    auto get = reinterpret_cast<PFN_vkGetNativeBufferPropertiesOHOS>(
        g_device.vk.GetDeviceProcAddr(g_device.device(), "vkGetNativeBufferPropertiesOHOS"));
    if (!get) return false;
    VkNativeBufferFormatPropertiesOHOS format{VK_STRUCTURE_TYPE_NATIVE_BUFFER_FORMAT_PROPERTIES_OHOS};
    VkNativeBufferPropertiesOHOS props{VK_STRUCTURE_TYPE_NATIVE_BUFFER_PROPERTIES_OHOS, &format};
    if (get(g_device.device(), static_cast<OH_NativeBuffer*>(buffer), &props) != VK_SUCCESS ||
        !props.allocationSize || !props.memoryTypeBits || format.format == VK_FORMAT_UNDEFINED ||
        !(format.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return false;
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OHOS_NATIVE_BUFFER_BIT_OHOS;
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external};
    image.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    image.imageType = VK_IMAGE_TYPE_2D; image.format = format.format;
    image.extent = {width, height, 1}; image.mipLevels = image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT; image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    if (g_device.vk.CreateImage(g_device.device(), &image, nullptr, &m_vkImage) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    g_device.vk.GetImageMemoryRequirements(g_device.device(), m_vkImage, &req);
    uint32_t bits = req.memoryTypeBits & props.memoryTypeBits, type = 0;
    if (!bits) { g_device.vk.DestroyImage(g_device.device(), m_vkImage, nullptr); m_vkImage = VK_NULL_HANDLE; return false; }
    while (!(bits & (1u << type))) ++type;
    VkImportNativeBufferInfoOHOS import{VK_STRUCTURE_TYPE_IMPORT_NATIVE_BUFFER_INFO_OHOS};
    import.buffer = static_cast<OH_NativeBuffer*>(buffer);
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &import};
    dedicated.image = m_vkImage;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated, props.allocationSize, type};
    if (g_device.vk.AllocateMemory(g_device.device(), &allocation, nullptr, &m_vkImageMemory) != VK_SUCCESS) {
        g_device.vk.DestroyImage(g_device.device(), m_vkImage, nullptr); m_vkImage = VK_NULL_HANDLE; return false;
    }
    if (g_device.vk.BindImageMemory(g_device.device(), m_vkImage, m_vkImageMemory, 0) != VK_SUCCESS) return false;
    const auto memory = m_vkImageMemory;
    const bool views = BInitFromSwapchain(m_vkImage, width, height, format.format);
    m_vkImageMemory = memory; m_bOutputImage = false;
    if (!views || !OhosNativeBufferReference(buffer)) return false;
    m_ohosBuffer = buffer;
    // The embedding bridge records the FOREIGN acquisition/release. Core
    // compute sees a local GENERAL image, never a second implicit import.
    markLocalQueueDependency();
    return true;
}
