// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <array>
#include <vulkan/vulkan.h>

inline bool VppGamescopeStorageOutput(VkImageUsageFlags usage) {
    return (usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
}
inline VkImageUsageFlags VppGamescopeOutputUsage(bool storage) {
    return storage ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_TRANSFER_DST_BIT;
}
inline VkPipelineStageFlags VppGamescopeOutputWaitStage(bool storage) {
    return storage ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
}

// A storage-capable WSI image receives the final encoded compositor output.
// Other surfaces retain the compute target and GPU copy transport.
template<class Vulkan>
void VppGamescopeRecordOutput(Vulkan& vk, VkCommandBuffer command, VkImage target,
                             VkImage output, uint32_t width, uint32_t height, bool storage) {
    std::array<VkImageMemoryBarrier, 2> barriers{};
    for (auto& b : barriers) {
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    }
    barriers[0].image = target;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].newLayout = storage ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[0].dstAccessMask = storage ? 0u : VkAccessFlags(VK_ACCESS_TRANSFER_READ_BIT);
    if (storage) {
        vk.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                              0,0,nullptr,0,nullptr,1,barriers.data());
        return;
    }
    barriers[1].image = output;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vk.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0,0,nullptr,0,nullptr,barriers.size(),barriers.data());
    const VkImageCopy copy{{VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},{0,0,0},
                          {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},{0,0,0},{width,height,1}};
    vk.CmdCopyImage(command,target,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].dstAccessMask = 0;
    vk.CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          0,0,nullptr,0,nullptr,barriers.size(),barriers.data());
}
