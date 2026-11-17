// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2013-2022, Valve Corporation
// Copyright (c) 2022, NVIDIA CORPORATION
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// OHOS output: Gamescope compute target -> GPU copy -> NativeWindow.
// Input is delivered by the VPP session; this backend opens no input devices.
#include "backend.h"
#include "backends.h"
#include "main.hpp"
#include "rendervulkan.hpp"
#include "wlserver.hpp"
#include "vblankmanager.hpp"
#include "refresh_rate.h"
#include "OhosWindow.h"
#include "Runtime.h"
#include <vulkan/vulkan_ohos.h>
#include <atomic>
#include <cstdlib>
#include <array>
#include <algorithm>
#include <bitset>
#include <cerrno>
#include <cstring>

extern int g_nPreferredOutputWidth, g_nPreferredOutputHeight;
extern PFN_vkGetInstanceProcAddr g_pfn_vkGetInstanceProcAddr;
extern uint64_t get_time_in_nanos();

namespace gamescope {
class COhosBackend;
class COhosConnector final : public CBaseBackendConnector {
public:
    explicit COhosConnector(COhosBackend* backend) : m_backend(backend) {}
    GamescopeScreenType GetScreenType() const override { return GAMESCOPE_SCREEN_TYPE_INTERNAL; }
    GamescopePanelOrientation GetCurrentOrientation() const override { return GAMESCOPE_PANEL_ORIENTATION_0; }
    bool SupportsHDR() const override { return false; }
    bool IsHDRActive() const override { return false; }
    const BackendConnectorHDRInfo& GetHDRInfo() const override { return m_hdr; }
    bool IsVRRActive() const override { return false; }
    bool SupportsVRR() const override { return false; }
    std::span<const BackendMode> GetModes() const override { return {&mode, 1}; }
    std::span<const uint8_t> GetRawEDID() const override { return {}; }
    std::span<const uint32_t> GetValidDynamicRefreshRates() const override { return {}; }
    const char* GetName() const override { return "VPP OHOS output"; }
    const char* GetMake() const override { return "Gamescope"; }
    const char* GetModel() const override { return "OHOS NativeWindow"; }
    void GetNativeColorimetry(bool, displaycolorimetry_t* display, EOTF* displayTF,
                             displaycolorimetry_t* output, EOTF* outputTF) const override {
        *display = *output = displaycolorimetry_709; *displayTF = *outputTF = EOTF_Gamma22;
    }
    int Present(const FrameInfo_t*, bool) override;
    BackendMode mode{};
private:
    COhosBackend* m_backend;
    BackendConnectorHDRInfo m_hdr{};
};

class COhosBackend final : public CBaseBackend {
    struct Frame {
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkCommandBuffer command = VK_NULL_HANDLE;
    };
    struct Output {
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        std::vector<VkImage> images;
        std::vector<OwningRc<CVulkanTexture>> targets;
        // Present can outlive a submit fence. Reuse only after reacquiring the
        // same swapchain image, which establishes consumption of its wait.
        std::vector<VkSemaphore> rendered;
    };
public:
    COhosBackend() : m_connector(this) {}
    ~COhosBackend() override {
        if (m_pool) {
            // Lifecycle drain only. Normal frames use slot fences/semaphores.
            g_device.vk.QueueWaitIdle(g_device.queue());
            g_device.waitIdle();
            destroyOutput(m_output);
            for (auto& f : m_frames) {
                if (f.acquired) g_device.vk.DestroySemaphore(g_device.device(), f.acquired, nullptr);
                if (f.fence) m_destroyFence(g_device.device(), f.fence, nullptr);
            }
            g_device.vk.DestroyCommandPool(g_device.device(), m_pool, nullptr);
        }
        if (m_surface && m_destroySurface) m_destroySurface(vulkan_get_instance(), m_surface, nullptr);
        if (m_window) DestroyOutputWindow(m_window);
    }
    bool Init() override {
        const char* text = getenv("VPP_GAMESCOPE_SURFACE_ID");
        if (!text || !*text || *text == '-') return error("output surface id", VK_ERROR_INITIALIZATION_FAILED);
        char* end = nullptr; errno = 0;
        const auto surfaceId = strtoull(text, &end, 10);
        if (errno || !end || *end || !surfaceId || !(m_window = CreateOutputWindow(surfaceId)))
            return error("output NativeWindow", VK_ERROR_INITIALIZATION_FAILED);
        const int height = g_nPreferredOutputHeight ? g_nPreferredOutputHeight : 720;
        const int width = g_nPreferredOutputWidth ? g_nPreferredOutputWidth : height * 16 / 9;
        if (height <= 0 || width <= 0 || height > 8192 || width > 8192) return error("output size", VK_ERROR_INITIALIZATION_FAILED);
        g_nOutputWidth = width; g_nOutputHeight = height;
        // This is the requested nested pacing rate, never a scanout timestamp.
        g_nOutputRefresh = g_nNestedRefresh ? g_nNestedRefresh : ConvertHztomHz(60);
        m_connector.mode = {uint32_t(width), uint32_t(height), uint32_t(g_nOutputRefresh / 1000)};
        auto instance = vulkan_get_instance();
        auto get = g_pfn_vkGetInstanceProcAddr;
        if (!instance || !get) return error("OHOS Vulkan loader", VK_ERROR_INITIALIZATION_FAILED);
        auto createSurface = reinterpret_cast<PFN_vkCreateSurfaceOHOS>(get(instance, "vkCreateSurfaceOHOS"));
        m_destroySurface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(get(instance, "vkDestroySurfaceKHR"));
        if (!createSurface || !m_destroySurface) return error("OHOS WSI functions", VK_ERROR_EXTENSION_NOT_PRESENT);
        const VkSurfaceCreateInfoOHOS info{VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS, nullptr, 0, m_window};
        VkResult result = createSurface(instance, &info, nullptr, &m_surface);
        if (result != VK_SUCCESS) return error("OHOS surface", result);
        if (!vulkan_init(instance, m_surface) || !wlsession_init()) return false;
        m_destroyFence = reinterpret_cast<PFN_vkDestroyFence>(g_device.vk.GetDeviceProcAddr(g_device.device(), "vkDestroyFence"));
        if (!m_destroyFence) return error("fence function", VK_ERROR_INITIALIZATION_FAILED);
        const VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
            VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_device.queueFamily()};
        result = g_device.vk.CreateCommandPool(g_device.device(), &pool, nullptr, &m_pool);
        if (result != VK_SUCCESS) return error("output command pool", result);
        for (auto& f : m_frames) {
            const VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            result = g_device.vk.CreateSemaphore(g_device.device(), &semaphore, nullptr, &f.acquired);
            if (result != VK_SUCCESS) return error("acquire semaphore", result);
            const VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
            result = g_device.vk.CreateFence(g_device.device(), &fence, nullptr, &f.fence);
            if (result != VK_SUCCESS) return error("frame fence", result);
            const VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, m_pool,
                VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
            result = g_device.vk.AllocateCommandBuffers(g_device.device(), &command, &f.command);
            if (result != VK_SUCCESS) return error("frame command", result);
        }
        return recreateOutput();
    }
    bool PostInit() override { m_ready = true; return true; }
    std::span<const char* const> GetInstanceExtensions() const override {
        static const char* const extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_OHOS_SURFACE_EXTENSION_NAME};
        return extensions;
    }
    std::span<const char* const> GetDeviceExtensions(VkPhysicalDevice) const override { return {}; }
    VkImageLayout GetPresentLayout() const override { return VK_IMAGE_LAYOUT_GENERAL; }
    void GetPreferredOutputFormat(uint32_t* primary, uint32_t* overlay) const override {
        *primary = *overlay = VulkanFormatToDRM(VK_FORMAT_R8G8B8A8_UNORM);
    }
    bool ValidPhysicalDevice(VkPhysicalDevice) const override { return true; }
    void DirtyState(bool, bool) override {}
    std::shared_ptr<BackendBlob> CreateBackendBlob(const std::type_info&, std::span<const uint8_t> data) override {
        return std::make_shared<BackendBlob>(data);
    }
    OwningRc<IBackendFb> ImportDmabufToBackend(wlr_dmabuf_attributes*) override { return nullptr; }
    bool UsesModifiers() const override { return false; }
    std::span<const uint64_t> GetSupportedModifiers(uint32_t) const override { return {}; }
    IBackendConnector* GetCurrentConnector() override { return &m_connector; }
    IBackendConnector* GetConnector(GamescopeScreenType type) override {
        return type == GAMESCOPE_SCREEN_TYPE_INTERNAL ? &m_connector : nullptr;
    }
    bool SupportsPlaneHardwareCursor() const override { return false; }
    bool SupportsTearing() const override { return false; }
    bool UsesVulkanSwapchain() const override { return false; }
    bool UsesBackendSwapchain() const override { return true; }
    bool UsesDmaBufImages() const override { return false; }
    bool RequiresDynamicRendering() const override { return false; }
    // DeferredBackend requires an external-FD transport before its child is
    // initialized; this backend owns its surface and device from the start.
    bool SupportsDeferredInit() const override { return false; }
    bool IsSessionBased() const override { return false; }
    bool SupportsIndependentRefresh() const override { return true; }
    bool SupportsExplicitSync() const override { return false; } // no DRM syncobj API
    bool IsPaused() const override { return !m_visible; }
    bool IsVisible() const override { return m_visible; }
    glm::uvec2 CursorSurfaceSize(glm::uvec2 size) const override { return size; }
    bool PollState() override;
    int present(const FrameInfo_t* info);
protected:
    void OnBackendBlobDestroyed(BackendBlob*) override {}
private:
    bool error(const char* stage, VkResult result) {
        fprintf(stderr, "VPP_GAMESCOPE_BACKEND_ERROR stage=%s result=%d\n", stage, int(result));
        m_failed = true;
        if (m_ready) g_bRun = false;
        return false;
    }
    void destroyOutput(Output& output) {
        output.targets.clear(); output.images.clear();
        for (VkSemaphore semaphore : output.rendered)
            if (semaphore) g_device.vk.DestroySemaphore(g_device.device(), semaphore, nullptr);
        output.rendered.clear();
        if (output.swapchain) g_device.vk.DestroySwapchainKHR(g_device.device(), output.swapchain, nullptr);
        output.swapchain = VK_NULL_HANDLE;
    }
    bool recreateOutput();
    OHNativeWindow* m_window = nullptr;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    PFN_vkDestroySurfaceKHR m_destroySurface = nullptr;
    PFN_vkDestroyFence m_destroyFence = nullptr;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    std::array<Frame, 3> m_frames{};
    Output m_output;
    COhosConnector m_connector;
    uint64_t m_frameNumber = 0;
    uint64_t m_requestedSize = 0;
    bool m_ready = false, m_visible = true, m_resize = false, m_failed = false;
};

bool COhosBackend::recreateOutput() {
    if (!SetOutputWindowSize(m_window, g_nOutputWidth, g_nOutputHeight))
        return error("output buffer geometry", VK_ERROR_INITIALIZATION_FAILED);
    VkSurfaceCapabilitiesKHR caps{};
    VkResult result = g_device.vk.GetPhysicalDeviceSurfaceCapabilitiesKHR(g_device.physDev(), m_surface, &caps);
    if (result != VK_SUCCESS) return error("surface capabilities", result);
    fprintf(stderr, "VPP_GAMESCOPE_SURFACE_CAPS usage=0x%x alpha=0x%x extent=%ux%u minImages=%u maxImages=%u transforms=0x%x currentTransform=0x%x\n",
        caps.supportedUsageFlags, caps.supportedCompositeAlpha, caps.currentExtent.width, caps.currentExtent.height,
        caps.minImageCount, caps.maxImageCount, caps.supportedTransforms, caps.currentTransform);
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return error("output transfer usage", VK_ERROR_FEATURE_NOT_PRESENT);
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    bool alphaFound = false;
    for (auto candidate : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                           VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
        if (caps.supportedCompositeAlpha & candidate) { alpha = candidate; alphaFound = true; break; }
    }
    if (!alphaFound) return error("output composite alpha", VK_ERROR_FEATURE_NOT_PRESENT);
    uint32_t count = 0;
    result = g_device.vk.GetPhysicalDeviceSurfaceFormatsKHR(g_device.physDev(), m_surface, &count, nullptr);
    if (result != VK_SUCCESS || !count || count > 1024) return error("surface formats", result == VK_SUCCESS ? VK_ERROR_FORMAT_NOT_SUPPORTED : result);
    std::vector<VkSurfaceFormatKHR> formats(count);
    result = g_device.vk.GetPhysicalDeviceSurfaceFormatsKHR(g_device.physDev(), m_surface, &count, formats.data());
    if (result != VK_SUCCESS) return error("surface format list", result);
    bool found = false;
    for (const auto& format : formats) if (format.format == VK_FORMAT_R8G8B8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) found = true;
    if (!found) return error("RGBA8 SDR output", VK_ERROR_FORMAT_NOT_SUPPORTED);
    // The local compute target is queried separately from the surface image.
    const VkPhysicalDeviceImageFormatInfo2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, nullptr,
        VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0};
    VkImageFormatProperties2 properties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    result = g_device.vk.GetPhysicalDeviceImageFormatProperties2(g_device.physDev(), &query, &properties);
    if (result != VK_SUCCESS) return error("compute target properties", result);
    VkExtent2D extent{uint32_t(g_nOutputWidth), uint32_t(g_nOutputHeight)};
    if (caps.currentExtent.width != UINT32_MAX) extent = caps.currentExtent;
    else {
        extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (!extent.width || !extent.height || extent.width > properties.imageFormatProperties.maxExtent.width ||
        extent.height > properties.imageFormatProperties.maxExtent.height) return error("surface extent", VK_ERROR_OUT_OF_DATE_KHR);
    uint32_t imageCount = std::max(2u, caps.minImageCount);
    if (caps.maxImageCount && imageCount > caps.maxImageCount) return error("output slot count", VK_ERROR_FEATURE_NOT_PRESENT);
    Output next;
    VkSwapchainCreateInfoKHR info{}; info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = m_surface; info.minImageCount = imageCount; info.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
    info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR; info.imageExtent = extent; info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT; info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // The final texture is already drawn in XComponent coordinates. Prefer
    // an unrotated buffer instead of rotating the logical frame a second time
    // with the display's current orientation when WSI supports that contract.
    info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
        ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    info.compositeAlpha = alpha;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR; info.clipped = VK_TRUE; info.oldSwapchain = m_output.swapchain;
    // Explicit resize/teardown boundary only; old commands are no longer used.
    if (m_output.swapchain) {
        result = g_device.vk.QueueWaitIdle(g_device.queue());
        if (result != VK_SUCCESS) return error("resize drain", result);
        g_device.waitIdle();
    }
    result = g_device.vk.CreateSwapchainKHR(g_device.device(), &info, nullptr, &next.swapchain);
    if (result != VK_SUCCESS) return error("output swapchain", result);
    result = g_device.vk.GetSwapchainImagesKHR(g_device.device(), next.swapchain, &count, nullptr);
    if (result != VK_SUCCESS || count < 2 || count > 8) { destroyOutput(next); return error("output images", result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result); }
    next.images.resize(count); next.targets.resize(count);
    result = g_device.vk.GetSwapchainImagesKHR(g_device.device(), next.swapchain, &count, next.images.data());
    if (result != VK_SUCCESS) { destroyOutput(next); return error("output image list", result); }
    if (count != next.images.size()) { destroyOutput(next); return error("changed output image count", VK_ERROR_INITIALIZATION_FAILED); }
    next.rendered.resize(count, VK_NULL_HANDLE);
    for (auto& semaphore : next.rendered) {
        const VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        result = g_device.vk.CreateSemaphore(g_device.device(), &create, nullptr, &semaphore);
        if (result != VK_SUCCESS) { destroyOutput(next); return error("present semaphore", result); }
    }
    for (auto& target : next.targets) {
        target = new CVulkanTexture(); CVulkanTexture::createFlags flags;
        flags.bStorage = true; flags.bSampled = true; flags.bTransferSrc = true;
        if (!target->BInit(extent.width, extent.height, 1, VulkanFormatToDRM(VK_FORMAT_R8G8B8A8_UNORM), flags)) {
            destroyOutput(next); return error("compute target", VK_ERROR_INITIALIZATION_FAILED);
        }
    }
    destroyOutput(m_output); m_output = std::move(next);
    g_nOutputWidth = extent.width; g_nOutputHeight = extent.height;
    m_connector.mode.uWidth = extent.width; m_connector.mode.uHeight = extent.height;
    m_resize = false;
    fprintf(stderr, "VPP_GAMESCOPE_OUTPUT_READY width=%u height=%u slots=%u cpuPixelCopies=0\n", extent.width, extent.height, count);
    ohos::ReportRuntimeStage(ohos::SessionStage::OutputReady, "native-window-swapchain");
    return true;
}

int COhosBackend::present(const FrameInfo_t* info) {
    if (m_failed) return -EIO;
    if (m_resize && !recreateOutput()) return -EIO;
    auto& frame = m_frames[m_frameNumber % m_frames.size()];
    VkResult result = g_device.vk.WaitForFences(g_device.device(), 1, &frame.fence, VK_TRUE, 5000000000ull);
    if (result != VK_SUCCESS) { error("output frame fence", result); return -EIO; }
    uint32_t index = 0;
    result = g_device.vk.AcquireNextImageKHR(g_device.device(), m_output.swapchain, 5000000000ull, frame.acquired, VK_NULL_HANDLE, &index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) { m_resize = true; return -EAGAIN; }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) { error("acquire output", result); return -EIO; }
    if (result == VK_SUBOPTIMAL_KHR) m_resize = true;
    if (index >= m_output.targets.size()) { error("output image index", VK_ERROR_INITIALIZATION_FAILED); return -EIO; }
    auto target = m_output.targets[index];
    if (m_frameNumber == 0 && info->layers.count() && info->layers.get(0).tex) {
        const auto& layer = info->layers.get(0);
        fprintf(stderr, "VPP_GAMESCOPE_FRAME source=%ux%u output=%dx%d target=%ux%u scale=%.6f,%.6f offset=%.3f,%.3f\n",
            layer.tex->width(), layer.tex->height(), g_nOutputWidth, g_nOutputHeight,
            target->width(), target->height(), layer.scale.x, layer.scale.y, layer.offset.x, layer.offset.y);
    }
    auto composed = vulkan_composite(const_cast<FrameInfo_t*>(info), nullptr, false, target, false);
    if (!composed) { error("composite submission", VK_ERROR_INITIALIZATION_FAILED); return -EIO; }
    result = g_device.vk.ResetCommandBuffer(frame.command, 0);
    if (result != VK_SUCCESS) { error("reset output command", result); return -EIO; }
    const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    result = g_device.vk.BeginCommandBuffer(frame.command, &begin);
    if (result != VK_SUCCESS) { error("begin output command", result); return -EIO; }
    std::array<VkImageMemoryBarrier, 2> barriers{};
    for (auto& b : barriers) { b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; }
    barriers[0].image = target->vkImage(); barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL; barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[1].image = m_output.images[index]; barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    g_device.vk.CmdPipelineBarrier(frame.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,0,nullptr,0,nullptr,barriers.size(),barriers.data());
    const VkImageCopy copy{{VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},{0,0,0},{VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},{0,0,0},{target->width(),target->height(),1}};
    g_device.vk.CmdCopyImage(frame.command, target->vkImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_output.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; barriers[0].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barriers[1].dstAccessMask = 0;
    g_device.vk.CmdPipelineBarrier(frame.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,0,nullptr,0,nullptr,barriers.size(),barriers.data());
    result = g_device.vk.EndCommandBuffer(frame.command);
    if (result != VK_SUCCESS) { error("end output command", result); return -EIO; }
    result = g_device.vk.ResetFences(g_device.device(), 1, &frame.fence);
    if (result != VK_SUCCESS) { error("reset output fence", result); return -EIO; }
    const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO,nullptr,1,&frame.acquired,&wait,1,&frame.command,1,&m_output.rendered[index]};
    result = g_device.vk.QueueSubmit(g_device.queue(), 1, &submit, frame.fence);
    if (result != VK_SUCCESS) { error("output GPU copy", result); return -EIO; }
    const VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,nullptr,1,&m_output.rendered[index],1,&m_output.swapchain,&index,nullptr};
    result = g_device.vk.QueuePresentKHR(g_device.queue(), &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) m_resize = true;
    else if (result != VK_SUCCESS) { error("present output", result); return -EIO; }
    ++m_frameNumber;
    if (m_frameNumber == 1) {
        fprintf(stderr, "VPP_GAMESCOPE_PRESENT frames=%llu cpuPixelCopies=0\n", (unsigned long long)m_frameNumber);
        ohos::ReportRuntimeStage(ohos::SessionStage::Presented, "vulkan-composite-present");
    }
    g_device.garbageCollect();
    GetVBlankTimer().UpdateWasCompositing(true);
    return result == VK_ERROR_OUT_OF_DATE_KHR ? -EAGAIN : 0;
}

namespace {
std::atomic<uint64_t> outputSize{0};
std::atomic<bool> outputVisible{true};
}
// The session owner updates output size/visibility. No second input chain.
extern "C" __attribute__((visibility("default"))) bool VppGamescopeOutputState(uint32_t width, uint32_t height, bool visible) {
    if (!width || !height || width > 8192 || height > 8192) return false;
    outputSize.store((uint64_t(width) << 32) | height); outputVisible.store(visible);
    return true;
}
bool COhosBackend::PollState() {
    if (!m_ready) return false;
    m_visible = outputVisible.load();
    const uint64_t size = outputSize.load();
    const uint32_t width = size >> 32, height = uint32_t(size);
    // WSI may clamp the requested extent to the actual XComponent size. Do
    // not recreate forever when that differs from the requested dimensions.
    if (width && height && size != m_requestedSize) {
        m_requestedSize = size;
        g_nOutputWidth = width; g_nOutputHeight = height;
        m_resize = true;
        return true;
    }
    return false;
}
int COhosConnector::Present(const FrameInfo_t* info, bool) { return m_backend->present(info); }
template<> bool IBackend::Set<COhosBackend>() { return Set(new COhosBackend{}); }
} // namespace gamescope
