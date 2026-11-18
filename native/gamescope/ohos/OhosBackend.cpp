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
#include "Embedded.h"
#include "EmbeddedContract.h"
#include "OutputTransport.h"
#include "FrameGeneration.h"
#include "FrameGenerationSchedule.h"
#include "convar.h"
#include <unordered_map>
#include <unistd.h>
#include <cmath>
#include <vulkan/vulkan_ohos.h>
#include <atomic>
#include <cstdlib>
#include <array>
#include <algorithm>
#include <bitset>
#include <cerrno>
#include <cstring>

extern int g_nPreferredOutputWidth, g_nPreferredOutputHeight;
extern uint32_t currentOutputWidth, currentOutputHeight;
extern PFN_vkGetInstanceProcAddr g_pfn_vkGetInstanceProcAddr;
extern uint64_t get_time_in_nanos();

extern void* OhosCloneOutputWindow(void*);
extern "C" bool VppGamescopeSetOutputWindow(void*);
namespace { bool embeddedOutput = false; std::atomic<bool> embeddedActive{false}; }
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
        VkCommandBuffer command = VK_NULL_HANDLE, acquire = VK_NULL_HANDLE;
        VkSemaphore released = VK_NULL_HANDLE;
        std::vector<VkSemaphore> waits;
        std::vector<Rc<CVulkanTexture>> inputs;
        std::vector<OwningRc<CVulkanTexture>> upright;
        OwningRc<CVulkanTexture> generated;
    };
    struct Output {
        bool storage = false;
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
            m_framegen.Destroy();
            m_history={};
            destroyOutput(m_output);
            for (auto& f : m_frames) {
                for (auto sem : f.waits) g_device.vk.DestroySemaphore(g_device.device(), sem, nullptr);
                if (f.released) g_device.vk.DestroySemaphore(g_device.device(), f.released, nullptr);
                if (f.acquired) g_device.vk.DestroySemaphore(g_device.device(), f.acquired, nullptr);
                if (f.fence) m_destroyFence(g_device.device(), f.fence, nullptr);
            }
            g_device.vk.DestroyCommandPool(g_device.device(), m_pool, nullptr);
        }
        if (m_surface && m_destroySurface) m_destroySurface(vulkan_get_instance(), m_surface, nullptr);
        if (m_window) DestroyOutputWindow(m_window);
    }
    int EmbeddedPresent(VppGamescopeFrame& frame);
    bool Init() override {
        if (embeddedOutput) {
            // Embedding does not run main's environment/config parser. Match
            // HSL's gamescope_shader_precompile=0 explicitly before device init.
            // Compile/cache only the actual session's pipelines on demand.
            std::array<std::string_view,2> args{"shader_precompile","0"};
            if (!ConCommand::Exec(args)) return error("shader compile policy", VK_ERROR_INITIALIZATION_FAILED);
        }
        const char* text = getenv("VPP_GAMESCOPE_SURFACE_ID");
        if (!embeddedOutput && (!text || !*text || *text == '-')) return error("output surface id", VK_ERROR_INITIALIZATION_FAILED);
        char* end = nullptr; errno = 0;
        const auto surfaceId = embeddedOutput ? 1ull : strtoull(text, &end, 10);
        if ((!embeddedOutput && (errno || !end || *end)) || !surfaceId || !(m_window = CreateOutputWindow(surfaceId)))
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
        if (!vulkan_init(instance, m_surface) || (!embeddedOutput && !wlsession_init())) return false;
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
            result = g_device.vk.AllocateCommandBuffers(g_device.device(), &command, &f.acquire);
            if (result != VK_SUCCESS) return error("input command", result);
            VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
            exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            const VkSemaphoreCreateInfo releaseInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &exportInfo};
            result = g_device.vk.CreateSemaphore(g_device.device(), &releaseInfo, nullptr, &f.released);
            if (result != VK_SUCCESS) return error("input release semaphore", result);
        }
        return recreateOutput();
    }
    bool PostInit() override { m_ready = true; return true; }
    std::span<const char* const> GetInstanceExtensions() const override {
        static const char* const extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_OHOS_SURFACE_EXTENSION_NAME};
        return extensions;
    }
    std::span<const char* const> GetDeviceExtensions(VkPhysicalDevice) const override {
        static const char* const extensions[] = {VK_OHOS_EXTERNAL_MEMORY_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME};
        return extensions;
    }
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
    VppGamescopeFrame* embeddedFrame = nullptr;
    struct CachedInput { OwningRc<CVulkanTexture> texture; uint64_t serial = 0, seen = 0; bool opaque = false; };
    std::unordered_map<uint64_t, CachedInput> embeddedCache;
    bool PrepareEmbedded(Frame& slot, FrameInfo_t& info);
    void InputBarrier(VkCommandBuffer command, CVulkanTexture* texture, bool acquire, bool vulkan);

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
    VppWinfgEngine m_framegen;
    VppFrameGenerationSchedule m_fgSchedule;
    std::array<OwningRc<CVulkanTexture>,2> m_history{};
    uint32_t m_historyIndex=0;
    uint64_t m_fgIdentity=0;
    bool m_fgTried=false, m_fgReady=false, m_fgEnabled=false;
    bool m_fgCountPending=false;
    uint32_t m_fgBudgetFailures=0;
    uint64_t m_fgCompleted=0;
    std::array<uint64_t,2> m_historyNs{};
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
    const bool storage = VppGamescopeStorageOutput(caps.supportedUsageFlags);
    const bool copy = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0;
    if (!storage && !copy) return error("output image usage", VK_ERROR_FEATURE_NOT_PRESENT);
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
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
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
    auto create = [&](bool direct) -> VkResult {
        next.storage = direct; info.imageUsage = VppGamescopeOutputUsage(direct);
        VkResult status = g_device.vk.CreateSwapchainKHR(g_device.device(), &info, nullptr, &next.swapchain);
        if (status != VK_SUCCESS) return status;
        count = 0;
        status = g_device.vk.GetSwapchainImagesKHR(g_device.device(), next.swapchain, &count, nullptr);
        if (status != VK_SUCCESS) return status;
        if (count < 2 || count > 8) return VK_ERROR_INITIALIZATION_FAILED;
        next.images.resize(count); next.targets.resize(count);
        status = g_device.vk.GetSwapchainImagesKHR(g_device.device(), next.swapchain, &count, next.images.data());
        if (status != VK_SUCCESS) return status;
        if (count != next.images.size()) return VK_ERROR_INITIALIZATION_FAILED;
        next.rendered.resize(count, VK_NULL_HANDLE);
        for (auto& semaphore : next.rendered) {
            const VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            status = g_device.vk.CreateSemaphore(g_device.device(), &semaphoreInfo, nullptr, &semaphore);
            if (status != VK_SUCCESS) return status;
        }
        for (size_t i = 0; i < next.targets.size(); ++i) {
            auto& target = next.targets[i]; target = new CVulkanTexture();
            if (direct) {
                if (!target->BInitOhosStorageOutput(next.images[i],extent.width,extent.height))
                    return VK_ERROR_INITIALIZATION_FAILED;
            } else {
                CVulkanTexture::createFlags flags;
                flags.bStorage = flags.bSampled = flags.bTransferSrc = true;
                if (!target->BInit(extent.width,extent.height,1,DRM_FORMAT_ABGR8888,flags))
                    return VK_ERROR_INITIALIZATION_FAILED;
            }
        }
        return VK_SUCCESS;
    };
    result = create(storage);
    if (result != VK_SUCCESS && storage && copy) {
        fprintf(stderr,"VPP_GAMESCOPE_STORAGE_FALLBACK result=%d\n",int(result));
        // Passing oldSwapchain retires it even when creation fails. Retry
        // with the new chain if one exists, otherwise with no predecessor.
        Output rejected = std::move(next); next = Output{};
        info.oldSwapchain = rejected.swapchain;
        result = create(false);
        destroyOutput(rejected);
    }
    if (result != VK_SUCCESS) { destroyOutput(next); return error("output swapchain targets",result); }
    destroyOutput(m_output); m_output = std::move(next);
    g_nOutputWidth = extent.width; g_nOutputHeight = extent.height;
    currentOutputWidth = extent.width; currentOutputHeight = extent.height;
    m_connector.mode.uWidth = extent.width; m_connector.mode.uHeight = extent.height;
    m_resize = false;
    fprintf(stderr, "VPP_GAMESCOPE_OUTPUT_READY width=%u height=%u slots=%u storageOutput=%u gpuOutputCopies=%u cpuPixelCopies=0\n",
        extent.width, extent.height, count, m_output.storage, !m_output.storage);
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
    frame.inputs.clear();
    FrameInfo_t embeddedInfo{};
    if (embeddedFrame) {
        if (!PrepareEmbedded(frame, embeddedInfo)) { error("native input bridge", VK_ERROR_INITIALIZATION_FAILED); return -EIO; }
        info = &embeddedInfo;
    } else if (m_output.storage) {
        // Standalone SHM diagnostics have no native-input submission in which
        // to consume acquisition. Order the compute write after WSI release.
        const VkPipelineStageFlags stage = VppGamescopeOutputWaitStage(true);
        const VkSubmitInfo acquired{VK_STRUCTURE_TYPE_SUBMIT_INFO,nullptr,1,&frame.acquired,&stage};
        result = g_device.vk.QueueSubmit(g_device.queue(),1,&acquired,VK_NULL_HANDLE);
        if (result != VK_SUCCESS) { error("acquire storage output",result); return -EIO; }
    }
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
    VppGamescopeRecordOutput(g_device.vk,frame.command,target->vkImage(),m_output.images[index],
                             target->width(),target->height(),m_output.storage);
    if (embeddedFrame) {
        size_t nativeIndex = 0;
        for (uint32_t i = 0; i < embeddedFrame->count; ++i)
            if (embeddedFrame->layers[i].native)
                InputBarrier(frame.command, frame.inputs[nativeIndex++].get(), false, embeddedFrame->layers[i].vulkan);
    }
    result = g_device.vk.EndCommandBuffer(frame.command);
    if (result != VK_SUCCESS) { error("end output command", result); return -EIO; }
    result = g_device.vk.ResetFences(g_device.device(), 1, &frame.fence);
    if (result != VK_SUCCESS) { error("reset output fence", result); return -EIO; }
    const VkPipelineStageFlags wait = VppGamescopeOutputWaitStage(false);
    const VkSemaphore signals[] = {m_output.rendered[index], frame.released};
    const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO,nullptr,m_output.storage ? 0u : 1u,&frame.acquired,&wait,1,&frame.command,
        embeddedFrame ? 2u : 1u,signals};
    result = g_device.vk.QueueSubmit(g_device.queue(), 1, &submit, frame.fence);
    if (result != VK_SUCCESS) { error("output GPU copy", result); return -EIO; }
    if (embeddedFrame) {
        VkSemaphoreGetFdInfoKHR fdInfo{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
        fdInfo.semaphore = frame.released; fdInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        if (!g_device.vk.GetSemaphoreFdKHR || g_device.vk.GetSemaphoreFdKHR(g_device.device(), &fdInfo,
                &embeddedFrame->completion) != VK_SUCCESS) return -EIO;
    }
    const VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,nullptr,1,&m_output.rendered[index],1,&m_output.swapchain,&index,nullptr};
    result = g_device.vk.QueuePresentKHR(g_device.queue(), &present);
    if (embeddedFrame && embeddedFrame->generated) m_fgCountPending=result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR;
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) m_resize = true;
    else if (result != VK_SUCCESS) { error("present output", result); return -EIO; }
    ++m_frameNumber;
    if (m_frameNumber == 1) {
        fprintf(stderr, "VPP_GAMESCOPE_PRESENT frames=%llu cpuPixelCopies=0\n", (unsigned long long)m_frameNumber);
        ohos::ReportRuntimeStage(ohos::SessionStage::Presented, "vulkan-composite-present");
    }
    g_device.garbageCollect();
    if (!embeddedOutput) GetVBlankTimer().UpdateWasCompositing(true);
    return result == VK_ERROR_OUT_OF_DATE_KHR ? -EAGAIN : 0;
}

void COhosBackend::InputBarrier(VkCommandBuffer command, CVulkanTexture* texture, bool acquire, bool vulkan) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.image = texture->vkImage(); b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    const auto foreign = vulkan ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_GENERAL;
    b.oldLayout = acquire ? foreign : VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = acquire ? VK_IMAGE_LAYOUT_GENERAL : foreign;
    b.srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_FOREIGN_EXT : g_device.queueFamily();
    b.dstQueueFamilyIndex = acquire ? g_device.queueFamily() : VK_QUEUE_FAMILY_FOREIGN_EXT;
    b.srcAccessMask = acquire ? 0 : VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = acquire ? VK_ACCESS_SHADER_READ_BIT : 0;
    g_device.vk.CmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,0,nullptr,0,nullptr,1,&b);
}

bool COhosBackend::PrepareEmbedded(Frame& slot, FrameInfo_t& info) {
    auto& input = *embeddedFrame;
    input.generated=0; input.completedGenerated=0; input.nextTickNs=0; input.frameGenerationGpuNs=0;
    input.frameGenerationState=VPP_FG_OFF;
    if (bool(input.frameGeneration)!=m_fgEnabled) {
        m_fgEnabled=bool(input.frameGeneration);
        m_framegen.Destroy(); m_fgSchedule.Reset(); m_history={}; m_fgIdentity=0;
        m_fgTried=m_fgReady=false;
        m_fgBudgetFailures=0;
        m_fgCompleted=0;
        m_fgCountPending=false;
    }
    if (input.historyReset) { m_fgSchedule.Reset(); m_fgIdentity=0; }
    auto collectCompleted=[&]() {
        const bool idle=m_framegen.Idle();
        input.frameGenerationGpuNs=m_framegen.GpuNs();
        if (m_fgCompleted!=m_framegen.Completed()) {
            m_fgCompleted=m_framegen.Completed();
            input.completedGenerated+=m_fgCountPending && !m_framegen.SceneCut();
            m_fgCountPending=false;
            if (m_framegen.SceneCut()) { m_fgSchedule.Reset(); m_fgIdentity=0; }
            const auto budget=m_fgSchedule.GpuBudgetNs(input.displayPeriodNs);
            m_fgBudgetFailures=budget && input.frameGenerationGpuNs>budget?m_fgBudgetFailures+1:0;
        }
        return idle;
    };
    if (m_fgReady) collectCompleted();
    if (g_device.vk.ResetCommandBuffer(slot.acquire,0) != VK_SUCCESS) return false;
    const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,nullptr,VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (g_device.vk.BeginCommandBuffer(slot.acquire,&begin) != VK_SUCCESS) return false;
    std::vector<VkSemaphore> waits; std::vector<VkPipelineStageFlags> stages;
    std::vector<Rc<CVulkanTexture>> textures;
    slot.upright.resize(input.count);
    size_t uploadBytes = 0;
    for (uint32_t i=0;i<input.count;++i) {
        const auto& layer = input.layers[i];
        if (!layer.width || !layer.height || layer.width>8192 || layer.height>8192 ||
            !std::isfinite(layer.w) || !std::isfinite(layer.h) || layer.w<=0 || layer.h<=0) return false;
        uint64_t key = layer.native ? uint64_t(reinterpret_cast<uintptr_t>(layer.native)) : layer.key;
        // GPU and SHM identities occupy separate cache namespaces.
        key = (key << 1) | (layer.native ? 1 : 0);
        auto& cached = embeddedCache[key];
        if (!cached.texture || cached.texture->width()!=layer.width || cached.texture->height()!=layer.height ||
            cached.opaque!=bool(layer.opaque) ||
            (!layer.native && cached.serial!=layer.serial)) {
            OwningRc<CVulkanTexture> texture;
            if (layer.native) {
                texture = new CVulkanTexture();
                if (!texture->BInitNativeBuffer(layer.native,layer.width,layer.height,layer.opaque)) return false;
            } else {
                uploadBytes += size_t(layer.width)*layer.height*4;
                if (!layer.pixels || uploadBytes>32u*1024*1024) return false;
                CVulkanTexture::createFlags flags;
                texture = vulkan_create_texture_from_bits(layer.width,layer.height,layer.width,layer.height,
                    layer.opaque?DRM_FORMAT_XRGB8888:DRM_FORMAT_ARGB8888,flags,const_cast<void*>(layer.pixels));
                if (!texture) return false;
            }
            cached.texture=std::move(texture); cached.serial=layer.serial; cached.opaque=layer.opaque;
        }
        cached.seen=m_frameNumber;
        textures.push_back(cached.texture.get());
        if (!layer.native) continue;
        slot.inputs.push_back(cached.texture.get());
        if (layer.fence>=0) {
            const size_t n=waits.size();
            if (n>=slot.waits.size()) {
                VkSemaphore sem; const VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                if (g_device.vk.CreateSemaphore(g_device.device(),&create,nullptr,&sem)!=VK_SUCCESS) return false;
                slot.waits.push_back(sem);
            }
            int fd=dup(layer.fence); if (fd<0) return false;
            VkImportSemaphoreFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
            import.semaphore=slot.waits[n]; import.flags=VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
            import.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT; import.fd=fd;
            if (g_device.vk.ImportSemaphoreFdKHR(g_device.device(),&import)!=VK_SUCCESS) { close(fd); return false; }
            waits.push_back(slot.waits[n]); stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        }
        InputBarrier(slot.acquire,cached.texture.get(),true,layer.vulkan);
    }
    if (g_device.vk.EndCommandBuffer(slot.acquire)!=VK_SUCCESS) return false;
    if (m_output.storage) {
        waits.push_back(slot.acquired); stages.push_back(VppGamescopeOutputWaitStage(true));
    }
    const VkSubmitInfo acquire{VK_STRUCTURE_TYPE_SUBMIT_INFO,nullptr,uint32_t(waits.size()),waits.data(),stages.data(),1,&slot.acquire};
    if (g_device.vk.QueueSubmit(g_device.queue(),1,&acquire,VK_NULL_HANDLE)!=VK_SUCCESS) return false;
    // Storage writes do not apply framebuffer sRGB encoding. Decode input
    // once, blend in linear light, and explicitly encode the final SDR output.
    info.applyOutputColorMgmt=true;
    info.outputEncodingEOTF=EOTF_Gamma22;
    for (uint32_t i=0;i<input.count;++i) {
        const auto& src=input.layers[i]; auto texture=textures[i];
        bool fgLayer=m_fgEnabled && i==0 && src.native && src.opaque;
        bool fresh=false;
        if (fgLayer) {
            // Geometry/renderer changes are history boundaries. Exclude the
            // producer's rotating buffer address and its frame serial.
            uint64_t identity=1469598103934665603ull;
            auto hash=[&](const auto& value) {
                const auto* p=reinterpret_cast<const uint8_t*>(&value);
                for (size_t n=0;n<sizeof(value);++n) identity=(identity^p[n])*1099511628211ull;
            };
            hash(src.key); hash(src.width); hash(src.height); hash(src.vulkan);
            hash(src.u0); hash(src.u1); hash(src.v0); hash(src.v2);
            hash(src.x); hash(src.y); hash(src.w); hash(src.h); hash(input.width); hash(input.height);
            if (m_fgIdentity!=identity) {
                // Lifecycle boundary only. Do not destroy an image still used
                // by normalization/composition from the previous geometry.
                g_device.vk.QueueWaitIdle(g_device.queue()); g_device.waitIdle();
                m_fgSchedule.Reset(); m_history={}; m_historyNs={}; m_historyIndex=0; m_fgIdentity=identity;
                m_fgBudgetFailures=0;
            }
            fresh=m_fgSchedule.Observe(identity,src.serial,input.nowNs,input.displayPeriodNs);
            input.frameGenerationState=m_fgSchedule.Reason(input.displayPeriodNs);
            if (fresh) m_historyIndex^=1;
            // Saturated/unsupported paths keep the original compositor cost.
            // Capture only while warming, generating, or releasing a held real.
            if (!input.displayPeriodNs || (input.frameGenerationState==VPP_FG_DISPLAY_RATE &&
                    !m_fgSchedule.CanGenerate() && !m_fgSchedule.Pending())) fgLayer=false;
            if (m_fgTried && !m_fgReady) { fgLayer=false; input.frameGenerationState=VPP_FG_UNSUPPORTED; }
            if (m_fgBudgetFailures>=3 || std::abs(src.u1*src.v2)*double(src.width)*src.height>1920u*1080u) {
                fgLayer=false; input.frameGenerationState=VPP_FG_BUDGET;
            }
        } else if (m_fgEnabled && i==0) {
            m_fgSchedule.Reset(); m_fgIdentity=0;
            input.frameGenerationState=VPP_FG_NO_GPU_GAME;
        }
        // Normalize GLES row direction and client cropping on GPU at source
        // resolution. FSR then receives upright client pixels, never borders.
        const bool crop=src.u0!=0 || src.u1!=1 || src.v0!=0 || src.v2!=1;
        const bool normalize=crop || fgLayer;
        if (normalize) {
            const uint32_t w=std::lround(std::abs(src.u1)*src.width), h=std::lround(std::abs(src.v2)*src.height);
            if (!w || !h || w>8192 || h>8192) return false;
            auto* upright=fgLayer?&m_history[m_historyIndex]:&slot.upright[i];
            auto allocate=[&]() {
                if (*upright && (*upright)->width()==w && (*upright)->height()==h) return true;
                *upright=new CVulkanTexture(); CVulkanTexture::createFlags flags;
                flags.bStorage=flags.bSampled=true;
                if ((*upright)->BInit(w,h,1,DRM_FORMAT_ABGR8888,flags)) return true;
                *upright=nullptr; return false;
            };
            if (!allocate()) {
                if (!fgLayer) return false;
                fgLayer=false; m_fgBudgetFailures=3; input.frameGenerationState=VPP_FG_BUDGET;
                upright=crop?&slot.upright[i]:nullptr;
                if (upright && !allocate()) return false;
            }
            if (upright) {
            if (!fgLayer || fresh) {
            FrameInfo_t normalizeInfo{}; auto* l=normalizeInfo.layers.push(); l->tex=texture;
            l->scale={src.u1*src.width/w,src.v2*src.height/h};
            l->offset={src.u0*src.width/l->scale.x,src.v0*src.height/l->scale.y};
            l->opacity=1; l->eAlphaBlendingMode=src.opaque?ALPHA_BLENDING_MODE_NONE:ALPHA_BLENDING_MODE_PREMULTIPLIED;
            // Crop/row normalization preserves encoded channel values. It is
            // not a color conversion; FSR expects the original sRGB samples.
            l->colorspace=GAMESCOPE_APP_TEXTURE_COLORSPACE_PASSTHRU;
            const auto outputW=currentOutputWidth, outputH=currentOutputHeight;
            currentOutputWidth=w; currentOutputHeight=h;
            auto submitted=vulkan_composite(&normalizeInfo,nullptr,false,upright->get(),false);
            currentOutputWidth=outputW; currentOutputHeight=outputH;
            if (!submitted) return false;
            if (fgLayer) m_historyNs[m_historyIndex]=input.nowNs;
            }
            texture=upright->get();
            }
        }
        const bool historyReady=m_history[m_historyIndex^1] && input.nowNs>=m_historyNs[m_historyIndex^1] &&
            input.nowNs-m_historyNs[m_historyIndex^1]<=m_fgSchedule.IntervalNs()*3/2;
        if (fgLayer && m_fgSchedule.CanGenerate() && !historyReady) input.frameGenerationState=VPP_FG_WARMING;
        if (fgLayer && m_fgSchedule.CanGenerate() && historyReady) {
            if (!m_fgTried) {
                m_fgTried=true;
                m_fgReady=m_framegen.Init(vulkan_get_instance(),g_pfn_vkGetInstanceProcAddr,
                    g_device.physDev(),g_device.device(),g_device.queueFamily(),g_device.queue());
            }
            if (!m_fgReady) input.frameGenerationState=VPP_FG_UNSUPPORTED;
            // Completion may arrive during source normalization. Account for
            // it before recording another generation or replacing its token.
            else if (!collectCompleted()) input.frameGenerationState=VPP_FG_GPU_BUSY;
            else if (!m_fgSchedule.CanGenerate()) input.frameGenerationState=VPP_FG_WARMING;
            else if (m_fgBudgetFailures>=3) input.frameGenerationState=VPP_FG_BUDGET;
            else if (!m_framegen.Resize({texture->width(),texture->height()})) {
                input.frameGenerationState=VPP_FG_FAILED; m_fgBudgetFailures=3;
            }
            else {
                auto& generated=slot.generated;
                if (!generated || generated->width()!=texture->width() || generated->height()!=texture->height()) {
                    generated=new CVulkanTexture(); CVulkanTexture::createFlags flags;
                    flags.bStorage=flags.bSampled=true;
                    if (!generated->BInit(texture->width(),texture->height(),1,DRM_FORMAT_ABGR8888,flags)) generated=nullptr;
                }
                auto previous=m_history[m_historyIndex^1].get();
                if (generated && m_framegen.Generate(previous,texture,generated.get())) {
                    texture=generated.get(); input.generated=1; input.frameGenerationState=VPP_FG_RUNNING;
                    m_fgSchedule.Generated(input.nowNs,input.displayPeriodNs);
                } else { input.frameGenerationState=VPP_FG_FAILED; m_fgBudgetFailures=3; }
            }
        }
        if (fgLayer) {
            if (!input.generated) m_fgSchedule.RealPresented();
            input.nextTickNs=m_fgSchedule.DueNs();
        }
        if (info.layers.count()==k_nMaxLayers) {
            // Preserve arbitrary desktop/UI stacking using bounded core passes.
            // VPP never composites a complete desktop before this entry point.
            OwningRc<CVulkanTexture> accumulated=new CVulkanTexture(); CVulkanTexture::createFlags flags;
            flags.bStorage=flags.bSampled=true;
            if (!accumulated->BInit(g_nOutputWidth,g_nOutputHeight,1,DRM_FORMAT_ABGR8888,flags) ||
                !vulkan_composite(&info,nullptr,false,accumulated.get(),false)) return false;
            info=FrameInfo_t{}; info.applyOutputColorMgmt=true; info.outputEncodingEOTF=EOTF_Gamma22;
            auto* base=info.layers.push(); base->tex=accumulated.get(); base->scale={1,1}; base->offset={0,0};
            base->opacity=1; base->eAlphaBlendingMode=ALPHA_BLENDING_MODE_NONE;
            base->colorspace=GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;
        }
        auto* layer=info.layers.push(); if (!layer) return false;
        layer->tex=texture; layer->scale={texture->width()/src.w,texture->height()/src.h};
        // A window is opaque only inside its own rectangle. Gamescope's
        // NONE mode replaces the whole output, including samples outside the
        // texture. Use opaque sampling views and transparent bounds instead,
        // so a raised taskbar or popup cannot erase the desktop behind it.
        layer->offset={-src.x,-src.y}; layer->opacity=1; layer->blackBorder=false;
        layer->filter=input.filter==1 || input.filter==2?GamescopeUpscaleFilter::NEAREST:GamescopeUpscaleFilter::LINEAR;
        layer->eAlphaBlendingMode=ALPHA_BLENDING_MODE_PREMULTIPLIED;
        layer->colorspace=GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;
        if (i==0 && src.upscale && input.filter==3 && src.w>texture->width() && src.h>texture->height()) {
            info.useFSRLayer0=true; info.nUpscaleSharpness=input.sharpness;
            ++input.fsrPasses;
        }
    }
    for (auto it=embeddedCache.begin();it!=embeddedCache.end();)
        if (it->second.seen!=m_frameNumber) it=embeddedCache.erase(it); else ++it;
    return true;
}
int COhosBackend::EmbeddedPresent(VppGamescopeFrame& frame) {
    if (!VppGamescopeValidFrame(frame)) return -EINVAL;
    frame.completion=-1;
    frame.fsrPasses=0; frame.gpuLayers=0;
    for (uint32_t i=0;i<frame.count;++i) frame.gpuLayers+=frame.layers[i].native!=nullptr;
    if (frame.width!=m_output.targets[0]->width() || frame.height!=m_output.targets[0]->height()) {
        g_nOutputWidth=frame.width; g_nOutputHeight=frame.height; m_resize=true;
    }
    embeddedFrame=&frame; int result=present(nullptr); embeddedFrame=nullptr;
    frame.width=g_nOutputWidth; frame.height=g_nOutputHeight;
    if (result && result!=-EAGAIN) { g_device.vk.QueueWaitIdle(g_device.queue()); g_device.waitIdle(); }
    return result;
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

extern "C" __attribute__((visibility("default"))) bool VppGamescopeEmbeddedStart(void* window,uint32_t width,uint32_t height) {
    if (!window || !width || !height || width>8192 || height>8192 || embeddedActive.exchange(true)) return false;
    auto* producer=OhosCloneOutputWindow(window);
    if (!producer || !VppGamescopeSetOutputWindow(producer)) { embeddedActive=false; return false; }
    embeddedOutput=true; g_nPreferredOutputWidth=width; g_nPreferredOutputHeight=height;
    if (!gamescope::IBackend::Set<gamescope::COhosBackend>() || !vulkan_init_formats() || !vulkan_make_output()) {
        gamescope::IBackend::Set(nullptr); embeddedOutput=false; embeddedActive=false; return false;
    }
    fprintf(stderr,"VPP_GAMESCOPE_EMBEDDED_READY gpuBridge=NativeBuffer cpuGameCopies=0 ncp=0\n");
    return true;
}
extern "C" __attribute__((visibility("default"))) int VppGamescopeEmbeddedPresent(VppGamescopeFrame* frame) {
    if (!frame || !embeddedActive.load() || !GetBackend()) return -EINVAL;
    return static_cast<gamescope::COhosBackend*>(GetBackend())->EmbeddedPresent(*frame);
}
extern "C" __attribute__((visibility("default"))) void VppGamescopeEmbeddedStop() {
    if (!embeddedActive.load()) return;
    gamescope::IBackend::Set(nullptr); embeddedOutput=false; embeddedActive=false;
}
