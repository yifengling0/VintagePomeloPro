// SPDX-License-Identifier: LGPL-2.1-or-later
// Real backing resources for applications which do not enable nullDescriptor.
// This does not implement or advertise VK_EXT_robustness2. The caller must
// separately exclude absent inputs in its shader and retire GPU uses before
// destroying this object. All handles belong to the caller's Vulkan device.
#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <cstring>

namespace hsl {
class VulkanDescriptorFallback {
public:
    enum Slot { Rgba, Ycbcr, Shaper, Lut3d, Count };
    static constexpr uint32_t UploadBytes = 20;
    inline static constexpr std::array<unsigned char, UploadBytes> Upload = {
        0,0,0,0, 0,0,0,0, 128,128,0,0, 0,0,0,0, 0,0,0,0
    };
    VulkanDescriptorFallback() = default;
    VulkanDescriptorFallback(const VulkanDescriptorFallback&) = delete;
    VulkanDescriptorFallback& operator=(const VulkanDescriptorFallback&) = delete;
    ~VulkanDescriptorFallback() { reset(); }

    VkImageView view(Slot slot) const { return resources[slot].view; }
    VkResult init(VkPhysicalDevice physical, VkDevice owner,
                  PFN_vkGetDeviceProcAddr get,
                  PFN_vkGetPhysicalDeviceImageFormatProperties2 query,
                  const VkPhysicalDeviceMemoryProperties& properties,
                  VkSamplerYcbcrConversion conversion) {
        if (device || !owner || !get || !query || !conversion)
            return VK_ERROR_INITIALIZATION_FAILED;
        device = owner;
#define HSL_LOAD(name) name = reinterpret_cast<PFN_vk##name>(get(device, "vk" #name)); \
        if (!name) { reset(); return VK_ERROR_INITIALIZATION_FAILED; }
        HSL_LOAD(CreateImage) HSL_LOAD(DestroyImage) HSL_LOAD(GetImageMemoryRequirements2)
        HSL_LOAD(AllocateMemory) HSL_LOAD(FreeMemory) HSL_LOAD(BindImageMemory)
        HSL_LOAD(CreateImageView) HSL_LOAD(DestroyImageView)
        HSL_LOAD(CmdPipelineBarrier) HSL_LOAD(CmdCopyBufferToImage)
#undef HSL_LOAD
        for (unsigned i = 0; i < Count; ++i) {
            const bool ycbcr = i == Ycbcr;
            const VkImageType type = i == Shaper ? VK_IMAGE_TYPE_1D :
                                     i == Lut3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
            const VkFormat format = ycbcr ? VK_FORMAT_G8_B8R8_2PLANE_420_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
            const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                (i == Rgba ? VkImageUsageFlags(VK_IMAGE_USAGE_STORAGE_BIT) : 0u);
            VkPhysicalDeviceImageFormatInfo2 info = {};
            info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
            info.format = format; info.type = type; info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = usage;
            VkImageFormatProperties2 supported = {};
            supported.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
            VkResult result = query(physical, &info, &supported);
            if (result != VK_SUCCESS) { reset(); return result; }
            const uint32_t side = ycbcr ? 2 : 1;
            if (supported.imageFormatProperties.maxExtent.width < side ||
                supported.imageFormatProperties.maxExtent.height < side ||
                supported.imageFormatProperties.maxExtent.depth < 1 ||
                !supported.imageFormatProperties.maxMipLevels ||
                !supported.imageFormatProperties.maxArrayLayers ||
                !(supported.imageFormatProperties.sampleCounts & VK_SAMPLE_COUNT_1_BIT)) {
                reset(); return VK_ERROR_FORMAT_NOT_SUPPORTED;
            }
            VkImageCreateInfo image = {};
            image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            image.imageType = type; image.format = format; image.extent = {side, side, 1};
            image.mipLevels = 1; image.arrayLayers = 1; image.samples = VK_SAMPLE_COUNT_1_BIT;
            image.tiling = VK_IMAGE_TILING_OPTIMAL; image.usage = usage;
            auto& resource = resources[i];
            result = CreateImage(device, &image, nullptr, &resource.image);
            if (result != VK_SUCCESS) { reset(); return result; }
            VkImageMemoryRequirementsInfo2 requirements = {};
            requirements.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
            requirements.image = resource.image;
            VkMemoryDedicatedRequirements dedicated = {};
            dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
            VkMemoryRequirements2 requirements2 = {};
            requirements2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
            requirements2.pNext = &dedicated;
            GetImageMemoryRequirements2(device, &requirements, &requirements2);
            const VkMemoryRequirements& memory = requirements2.memoryRequirements;
            uint32_t index = properties.memoryTypeCount;
            for (uint32_t j = 0; j < properties.memoryTypeCount; ++j)
                if ((memory.memoryTypeBits & (1u << j)) &&
                    (properties.memoryTypes[j].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { index = j; break; }
            if (index == properties.memoryTypeCount) { reset(); return VK_ERROR_FEATURE_NOT_PRESENT; }
            VkMemoryAllocateInfo allocation = {};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            VkMemoryDedicatedAllocateInfo dedicatedAllocation = {};
            dedicatedAllocation.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
            dedicatedAllocation.image = resource.image;
            if (dedicated.requiresDedicatedAllocation || dedicated.prefersDedicatedAllocation)
                allocation.pNext = &dedicatedAllocation;
            allocation.allocationSize = memory.size; allocation.memoryTypeIndex = index;
            result = AllocateMemory(device, &allocation, nullptr, &resource.memory);
            if (result == VK_SUCCESS) result = BindImageMemory(device, resource.image, resource.memory, 0);
            if (result != VK_SUCCESS) { reset(); return result; }
            VkSamplerYcbcrConversionInfo converted = {};
            converted.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
            converted.conversion = conversion;
            VkImageViewCreateInfo view = {};
            view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.pNext = ycbcr ? &converted : nullptr;
            view.image = resource.image; view.format = format;
            view.viewType = i == Shaper ? VK_IMAGE_VIEW_TYPE_1D : i == Lut3d ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
            view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            result = CreateImageView(device, &view, nullptr, &resource.view);
            if (result != VK_SUCCESS) { reset(); return result; }
        }
        return VK_SUCCESS;
    }

    // Record each selected slot exactly once after uploading Upload to a
    // transfer-source buffer. A mask permits independent resource initialization.
    // Submission/completion is owned by the caller, using its normal queue.
    void record(VkCommandBuffer command, VkBuffer upload, VkDeviceSize offset,
                uint32_t slots = (1u << Count) - 1) const {
        const uint32_t offsets[Count] = { 0, 4, 12, 16 };
        for (unsigned i = 0; i < Count; ++i) {
            if (!(slots & (1u << i))) continue;
            VkImageMemoryBarrier barrier = {};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = resources[i].image;
            barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, 0, nullptr, 0, nullptr, 1, &barrier);
            VkBufferImageCopy regions[2] = {};
            regions[0].bufferOffset = offset + offsets[i];
            regions[0].imageSubresource = { i == Ycbcr ? VK_IMAGE_ASPECT_PLANE_0_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            regions[0].imageExtent = { i == Ycbcr ? 2u : 1u, i == Ycbcr ? 2u : 1u, 1 };
            if (i == Ycbcr) {
                regions[1].bufferOffset = offset + 8;
                regions[1].imageSubresource = { VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1 };
                regions[1].imageExtent = { 1, 1, 1 };
            }
            CmdCopyBufferToImage(command, upload, resources[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 i == Ycbcr ? 2 : 1, regions);
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | (i == Rgba ? VkAccessFlags(VK_ACCESS_SHADER_WRITE_BIT) : 0u);
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = i == Shaper || i == Lut3d ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
            CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
    }

private:
    struct Resource { VkImage image = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };
    std::array<Resource, Count> resources = {};
    VkDevice device = VK_NULL_HANDLE;
#define HSL_FUNCTION(name) PFN_vk##name name = nullptr;
    HSL_FUNCTION(CreateImage) HSL_FUNCTION(DestroyImage) HSL_FUNCTION(GetImageMemoryRequirements2)
    HSL_FUNCTION(AllocateMemory) HSL_FUNCTION(FreeMemory) HSL_FUNCTION(BindImageMemory)
    HSL_FUNCTION(CreateImageView) HSL_FUNCTION(DestroyImageView)
    HSL_FUNCTION(CmdPipelineBarrier) HSL_FUNCTION(CmdCopyBufferToImage)
#undef HSL_FUNCTION
    void reset() {
        for (auto& resource : resources) {
            if (resource.view) DestroyImageView(device, resource.view, nullptr);
            if (resource.image) DestroyImage(device, resource.image, nullptr);
            if (resource.memory) FreeMemory(device, resource.memory, nullptr);
            resource = {};
        }
        device = VK_NULL_HANDLE;
    }
};
} // namespace hsl
