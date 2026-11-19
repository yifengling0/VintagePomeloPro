// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "harmony_gpu_frame_generation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace winehua::graphics {

// Original orthographic scene with two depth planes. Its color and depth are
// drawn together, using the same camera transform passed to the system API.
class HarmonyGpuProbeScene {
public:
    static constexpr VkExtent2D Extent{640, 384};
    static constexpr VkFormat ColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    enum class Frame { Real, Generated, Failed };

    HarmonyGpuProbeScene(VkPhysicalDevice physical, VkDevice device) : physical_(physical), device_(device) {}
    HarmonyGpuProbeScene(const HarmonyGpuProbeScene&) = delete;
    HarmonyGpuProbeScene& operator=(const HarmonyGpuProbeScene&) = delete;

    ~HarmonyGpuProbeScene()
    {
        generator_.reset();
        if (framebuffer_) vkDestroyFramebuffer(device_, framebuffer_, nullptr);
        if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
        for (const auto& image : images_) {
            if (image.frame.view) vkDestroyImageView(device_, image.frame.view, nullptr);
            if (image.frame.image) vkDestroyImage(device_, image.frame.image, nullptr);
            if (image.memory) vkFreeMemory(device_, image.memory, nullptr);
        }
    }

    const char* Start(VkInstance instance)
    {
        constexpr VkImageUsageFlags colorUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkFormat depth = VK_FORMAT_UNDEFINED;
        for (const auto format : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(physical_, format, &props);
            constexpr auto required = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            if ((props.optimalTilingFeatures & required) == required) { depth = format; break; }
        }
        if (depth == VK_FORMAT_UNDEFINED) return "Sampleable depth/stencil format unavailable";
        if (!CreateImage(images_[0], ColorFormat, colorUsage, VK_IMAGE_ASPECT_COLOR_BIT) ||
            !CreateImage(images_[1], depth, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ||
            !CreateImage(images_[2], ColorFormat, colorUsage, VK_IMAGE_ASPECT_COLOR_BIT) ||
            !CreateRenderPass()) return "Vulkan scene attachments unavailable";
        generator_ = std::make_unique<HarmonyGpuFrameGeneration>();
        FG_ContextDescription_VK context{};
        context.vkInstance = instance;
        context.vkPhysicalDevice = physical_;
        context.vkDevice = device_;
        context.framesInFlight = 1;
        context.fnVulkanLoaderFunction = vkGetInstanceProcAddr;
        const char* failure = generator_->Start(context, Extent, images_[0].frame, images_[1].frame, images_[2].frame);
        error_ = generator_->Error();
        return failure;
    }

    Frame Record(VkCommandBuffer command, VkImage swapImage, VkExtent2D output, bool enabled)
    {
        Frame result = Frame::Real;
        if (!realPending_) {
            const float camera = std::sin(static_cast<float>(sourceFrame_++) * 0.07f) * 0.24f;
            Draw(command, camera);
            if (enabled) {
                FG_Mat4x4 view{};
                view.data[0] = view.data[5] = view.data[10] = view.data[15] = 1;
                view.data[12] = -camera;
                auto inverse = view;
                inverse.data[12] = camera;
                error_ = generator_->Record(command, view, inverse, generated_);
                if (error_ == FG_SUCCESS) {
                    generated_ = true;
                    realPending_ = true;
                    result = Frame::Generated;
                } else if (error_ != FG_COLLECTING_PREVIOUS_FRAMES) {
                    return Frame::Failed;
                }
            }
        } else {
            realPending_ = false;
        }
        Blit(command, images_[result == Frame::Generated ? 2 : 0].frame.image, swapImage, output);
        return result;
    }

    int Error() const { return error_; }

private:
    struct Image {
        FrameGenerationImage frame{};
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    bool CreateImage(Image& image, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect)
    {
        image.frame.format = format;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {Extent.width, Extent.height, 1};
        info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(device_, &info, nullptr, &image.frame.image) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, image.frame.image, &requirements);
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(physical_, &memory);
        uint32_t type = 0;
        for (; type < memory.memoryTypeCount; ++type) {
            if ((requirements.memoryTypeBits & (1u << type)) &&
                (memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) break;
        }
        if (type == memory.memoryTypeCount) return false;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        if (vkAllocateMemory(device_, &allocation, nullptr, &image.memory) != VK_SUCCESS ||
            vkBindImageMemory(device_, image.frame.image, image.memory, 0) != VK_SUCCESS) return false;
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image.frame.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = {aspect, 0, 1, 0, 1};
        return vkCreateImageView(device_, &view, nullptr, &image.frame.view) == VK_SUCCESS;
    }

    bool CreateRenderPass()
    {
        std::array<VkAttachmentDescription, 2> attachments{};
        for (size_t i = 0; i < attachments.size(); ++i) {
            auto& attachment = attachments[i];
            attachment.format = images_[i].frame.format;
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp = i == 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = i == 1 ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        const VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        const VkAttachmentReference depth{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color;
        subpass.pDepthStencilAttachment = &depth;
        const VkSubpassDependency dependency{0, VK_SUBPASS_EXTERNAL,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, 0};
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.attachmentCount = attachments.size();
        info.pAttachments = attachments.data();
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = 1;
        info.pDependencies = &dependency;
        if (vkCreateRenderPass(device_, &info, nullptr, &renderPass_) != VK_SUCCESS) return false;
        const VkImageView views[]{images_[0].frame.view, images_[1].frame.view};
        VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = renderPass_;
        framebuffer.attachmentCount = 2;
        framebuffer.pAttachments = views;
        framebuffer.width = Extent.width;
        framebuffer.height = Extent.height;
        framebuffer.layers = 1;
        return vkCreateFramebuffer(device_, &framebuffer, nullptr, &framebuffer_) == VK_SUCCESS;
    }

    void Draw(VkCommandBuffer command, float camera) const
    {
        VkClearValue background[2]{};
        background[0].color = {{0.025f, 0.04f, 0.07f, 1}};
        background[1].depthStencil = {1, 0};
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = renderPass_;
        begin.framebuffer = framebuffer_;
        begin.renderArea.extent = Extent;
        begin.clearValueCount = 2;
        begin.pClearValues = background;
        vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
        constexpr VkClearColorValue tiles[] = {{{0.19f, 0.37f, 0.55f, 1}}, {{0.22f, 0.49f, 0.52f, 1}},
                                              {{0.84f, 0.35f, 0.19f, 1}}, {{0.95f, 0.73f, 0.26f, 1}}};
        for (int i = 0; i < 11; ++i) {
            const bool foreground = i >= 7;
            const float center = foreground ? -0.62f + (i - 7) * 0.39f : -1.05f + i * 0.35f;
            const float halfWidth = foreground ? 0.14f : 0.15f;
            const auto project = [camera](float x) {
                return std::clamp(static_cast<int>((x - camera + 1) * (Extent.width / 2)), 0,
                                  static_cast<int>(Extent.width));
            };
            const int left = project(center - halfWidth);
            const int right = project(center + halfWidth);
            if (right <= left) continue;
            VkClearRect rect{{{left, foreground ? 150 : 69}, {static_cast<uint32_t>(right - left),
                                                              foreground ? 171u : 186u}}, 0, 1};
            VkClearAttachment clear[2]{};
            clear[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            clear[0].clearValue.color = tiles[i % 2 + (foreground ? 2 : 0)];
            clear[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            clear[1].clearValue.depthStencil = {foreground ? 0.28f : 0.72f, 0};
            vkCmdClearAttachments(command, 2, clear, 1, &rect);
        }
        vkCmdEndRenderPass(command);
    }

    static void Blit(VkCommandBuffer command, VkImage source, VkImage target, VkExtent2D output)
    {
        constexpr VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier barriers[2]{};
        for (auto& barrier : barriers) {
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange = range;
        }
        barriers[0].image = source;
        barriers[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[1].image = target;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 2, barriers);
        VkImageBlit region{};
        region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffsets[1] = {Extent.width, Extent.height, 1};
        region.dstOffsets[1] = {static_cast<int32_t>(output.width), static_cast<int32_t>(output.height), 1};
        vkCmdBlitImage(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
        std::swap(barriers[0].oldLayout, barriers[0].newLayout);
        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].dstAccessMask = 0;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 2, barriers);
    }

    VkPhysicalDevice physical_;
    VkDevice device_;
    std::array<Image, 3> images_{};
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    std::unique_ptr<HarmonyGpuFrameGeneration> generator_;
    uint32_t sourceFrame_ = 0;
    int error_ = FG_SUCCESS;
    bool generated_ = false;
    bool realPending_ = false;
};

} // namespace winehua::graphics
