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
// Explicit GNU HSL output: local compute target -> GPU copy -> existing WSI.
// No OHOS object or platform driver is loaded into this process.
#include "backend.h"
#include "backends.h"
#include "main.hpp"
#include "rendervulkan.hpp"
#include "HslImage.h"
#include "wlserver.hpp"
#include "vblankmanager.hpp"
#include "refresh_rate.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <xkbcommon/xkbcommon.h>
#include <vulkan/vulkan_xlib.h>
#include <linux/input-event-codes.h>
#include <array>
#include <algorithm>
#include <bitset>
#include <cerrno>
#include <cstring>

extern int g_nPreferredOutputWidth, g_nPreferredOutputHeight;
extern PFN_vkGetInstanceProcAddr g_pfn_vkGetInstanceProcAddr;
extern uint64_t get_time_in_nanos();

namespace gamescope {
class CHslBackend;
class CHslConnector final : public CBaseBackendConnector {
public:
    explicit CHslConnector(CHslBackend* backend) : m_backend(backend) {}
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
    const char* GetName() const override { return "HSL GNU output"; }
    const char* GetMake() const override { return "Gamescope"; }
    const char* GetModel() const override { return "Nested HSL WSI"; }
    void GetNativeColorimetry(bool, displaycolorimetry_t* display, EOTF* displayTF,
                             displaycolorimetry_t* output, EOTF* outputTF) const override {
        *display = *output = displaycolorimetry_709; *displayTF = *outputTF = EOTF_Gamma22;
    }
    int Present(const FrameInfo_t*, bool) override;
    BackendMode mode{};
private:
    CHslBackend* m_backend;
    BackendConnectorHDRInfo m_hdr{};
};

class CHslBackend final : public CBaseBackend {
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
    CHslBackend() : m_connector(this) {}
    ~CHslBackend() override {
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
        if (m_window) XDestroyWindow(m_display, m_window);
        if (m_display) XCloseDisplay(m_display);
    }
    bool Init() override {
        if (!XInitThreads() || !(m_display = XOpenDisplay(nullptr))) return error("X11 display", VK_ERROR_INITIALIZATION_FAILED);
        int opcode = 0, xkbError = 0, major = XkbMajorVersion, minor = XkbMinorVersion;
        if (!XkbQueryExtension(m_display, &opcode, &m_xkbEvent, &xkbError, &major, &minor) || !updateKeymap())
            return error("XKB physical key mapping", VK_ERROR_FEATURE_NOT_PRESENT);
        const unsigned xkbEvents = XkbNewKeyboardNotifyMask | XkbMapNotifyMask | XkbNamesNotifyMask;
        if (!XkbSelectEvents(m_display, XkbUseCoreKbd, xkbEvents, xkbEvents))
            return error("XKB map change subscription", VK_ERROR_FEATURE_NOT_PRESENT);
        Bool detectable = False;
        if (!XkbSetDetectableAutoRepeat(m_display, True, &detectable) || !detectable)
            return error("XKB detectable repeat required", VK_ERROR_FEATURE_NOT_PRESENT);
        const int height = g_nPreferredOutputHeight ? g_nPreferredOutputHeight : 720;
        const int width = g_nPreferredOutputWidth ? g_nPreferredOutputWidth : height * 16 / 9;
        if (height <= 0 || width <= 0 || height > 8192 || width > 8192) return error("output size", VK_ERROR_INITIALIZATION_FAILED);
        g_nOutputWidth = width; g_nOutputHeight = height;
        // This is the requested nested pacing rate, never a scanout timestamp.
        g_nOutputRefresh = g_nNestedRefresh ? g_nNestedRefresh : ConvertHztomHz(60);
        m_connector.mode = {uint32_t(width), uint32_t(height), uint32_t(g_nOutputRefresh / 1000)};
        m_window = XCreateSimpleWindow(m_display, DefaultRootWindow(m_display), 0, 0, width, height, 0, 0, 0);
        if (!m_window) return error("X11 window", VK_ERROR_INITIALIZATION_FAILED);
        XStoreName(m_display, m_window, "Gamescope - HSL");
        // Set the standard initial EWMH state before mapping. The desktop WM
        // owns fullscreen sizing/focus; output recreation handles ConfigureNotify.
        if (g_bFullscreen) {
            Atom fullscreen = XInternAtom(m_display, "_NET_WM_STATE_FULLSCREEN", False);
            XChangeProperty(m_display, m_window, XInternAtom(m_display, "_NET_WM_STATE", False),
                            XA_ATOM, 32, PropModeReplace, reinterpret_cast<unsigned char*>(&fullscreen), 1);
        }
        XSelectInput(m_display, m_window, StructureNotifyMask | ExposureMask | FocusChangeMask |
                     KeyPressMask | KeyReleaseMask | PointerMotionMask | ButtonPressMask | ButtonReleaseMask);
        m_delete = XInternAtom(m_display, "WM_DELETE_WINDOW", False);
        m_protocols = XInternAtom(m_display, "WM_PROTOCOLS", False);
        XSetWMProtocols(m_display, m_window, &m_delete, 1);
        XMapWindow(m_display, m_window); XSync(m_display, False);
        auto instance = vulkan_get_instance();
        auto get = g_pfn_vkGetInstanceProcAddr;
        if (!instance || !get) return error("GNU Vulkan loader", VK_ERROR_INITIALIZATION_FAILED);
        auto createSurface = reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(get(instance, "vkCreateXlibSurfaceKHR"));
        m_destroySurface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(get(instance, "vkDestroySurfaceKHR"));
        if (!createSurface || !m_destroySurface) return error("Xlib WSI functions", VK_ERROR_EXTENSION_NOT_PRESENT);
        const VkXlibSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR, nullptr, 0, m_display, m_window};
        VkResult result = createSurface(instance, &info, nullptr, &m_surface);
        if (result != VK_SUCCESS) return error("Xlib surface", result);
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
        static const char* const extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XLIB_SURFACE_EXTENSION_NAME};
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
        fprintf(stderr, "HSL_GAMESCOPE_BACKEND_ERROR stage=%s result=%d\n", stage, int(result));
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
    void traceFrame(CVulkanTexture*, const FrameInfo_t*);
    bool updateKeymap();
    void releaseInput(uint32_t time);
    Display* m_display = nullptr;
    Window m_window = 0;
    Atom m_delete = 0, m_protocols = 0;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    PFN_vkDestroySurfaceKHR m_destroySurface = nullptr;
    PFN_vkDestroyFence m_destroyFence = nullptr;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    std::array<Frame, 3> m_frames{};
    Output m_output;
    CHslConnector m_connector;
    std::bitset<KEY_CNT> m_keys;
    std::array<uint32_t, 256> m_keyMap{};
    int m_xkbEvent = 0;
    std::bitset<3> m_buttons;
    uint64_t m_frameNumber = 0;
    bool m_ready = false, m_visible = true, m_resize = false, m_failed = false;
};

bool CHslBackend::recreateOutput() {
    VkSurfaceCapabilitiesKHR caps{};
    VkResult result = g_device.vk.GetPhysicalDeviceSurfaceCapabilitiesKHR(g_device.physDev(), m_surface, &caps);
    if (result != VK_SUCCESS) return error("surface capabilities", result);
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) return error("output transfer usage", VK_ERROR_FEATURE_NOT_PRESENT);
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
    info.preTransform = caps.currentTransform; info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
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
    fprintf(stderr, "HSL_GAMESCOPE_OUTPUT_READY width=%u height=%u slots=%u cpuPixelCopies=0\n", extent.width, extent.height, count);
    return true;
}

int CHslBackend::present(const FrameInfo_t* info) {
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
    // Explicit bounded diagnostic only. Normal rendering never reads pixels.
    const char* trace = getenv("HSL_GAMESCOPE_TRACE_PIXELS");
    if (trace && !strcmp(trace, "1")) {
        // A late bounded window can cover the library/game transition rather
        // than just the boot logo. Invalid offsets leave tracing disabled.
        const char* startText = getenv("HSL_GAMESCOPE_TRACE_START_FRAME");
        char* end = nullptr;
        errno = 0;
        const unsigned long long start = startText ? strtoull(startText, &end, 10) : 0;
        if ((!startText || (end != startText && !*end)) && !errno && start <= 1000000 &&
            m_frameNumber >= start && m_frameNumber - start < 180 && (m_frameNumber - start) % 15 == 0)
            traceFrame(target.get(), info);
    }
    const VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,nullptr,1,&m_output.rendered[index],1,&m_output.swapchain,&index,nullptr};
    result = g_device.vk.QueuePresentKHR(g_device.queue(), &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) m_resize = true;
    else if (result != VK_SUCCESS) { error("present output", result); return -EIO; }
    ++m_frameNumber;
    if (m_frameNumber == 1 || m_frameNumber % 120 == 0) fprintf(stderr, "HSL_GAMESCOPE_PRESENT frames=%llu cpuPixelCopies=0\n", (unsigned long long)m_frameNumber);
    g_device.garbageCollect();
    GetVBlankTimer().UpdateWasCompositing(true);
    return result == VK_ERROR_OUT_OF_DATE_KHR ? -EAGAIN : 0;
}

void CHslBackend::traceFrame(CVulkanTexture* output, const FrameInfo_t* info) {
    // Read a 4x4 grid through a standard transfer buffer. Linear storage images
    // are optional and are not a valid requirement for diagnostic readback.
    auto get = g_device.vk.GetDeviceProcAddr;
    auto copy = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(get(g_device.device(), "vkCmdCopyImageToBuffer"));
    auto freeCommands = reinterpret_cast<PFN_vkFreeCommandBuffers>(get(g_device.device(), "vkFreeCommandBuffers"));
    if (!copy || !freeCommands) return;
    VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
    bool submitted = false;
    auto cleanup = [&] {
        if (submitted) g_device.vk.QueueWaitIdle(g_device.queue());
        if (fence) m_destroyFence(g_device.device(), fence, nullptr);
        if (command) freeCommands(g_device.device(), m_pool, 1, &command);
        if (buffer) g_device.vk.DestroyBuffer(g_device.device(), buffer, nullptr);
        if (memory) g_device.vk.FreeMemory(g_device.device(), memory, nullptr);
    };
    auto run = [&]() -> VkResult {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=128; bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkResult r=g_device.vk.CreateBuffer(g_device.device(), &bi, nullptr, &buffer); if(r) return r;
        VkMemoryRequirements requirements{}; g_device.vk.GetBufferMemoryRequirements(g_device.device(),buffer,&requirements);
        uint32_t type=g_device.findMemoryType(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, requirements.memoryTypeBits);
        if(type==~0u) return VK_ERROR_FEATURE_NOT_PRESENT;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,nullptr,requirements.size,type};
        r=g_device.vk.AllocateMemory(g_device.device(),&ai,nullptr,&memory); if(r) return r;
        r=g_device.vk.BindBufferMemory(g_device.device(),buffer,memory,0); if(r) return r;
        VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,nullptr,m_pool,VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};
        r=g_device.vk.AllocateCommandBuffers(g_device.device(),&ci,&command); if(r) return r;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; r=g_device.vk.CreateFence(g_device.device(),&fi,nullptr,&fence); if(r) return r;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,nullptr,VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        r=g_device.vk.BeginCommandBuffer(command,&begin); if(r) return r;
        CVulkanTexture* textures[2]={output,info->layers.count()?info->layers.get(0).tex.get():nullptr};
        unsigned copied=0;
        for(unsigned n=0;n<2;++n) {
            auto* texture=textures[n]; if(!texture) continue;
            auto format=texture->format();
            if(format!=VK_FORMAT_R8G8B8A8_UNORM && format!=VK_FORMAT_B8G8R8A8_UNORM) continue;
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.image=texture->vkImage();
            b.oldLayout=b.newLayout=VK_IMAGE_LAYOUT_GENERAL; b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            g_device.vk.CmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
            std::array<VkBufferImageCopy,16> regions{};
            for(unsigned p=0;p<16;++p) {auto& v=regions[p]; v.bufferOffset=n*64+p*4; v.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
                v.imageOffset={int32_t((p%4)*texture->width()/4),int32_t((p/4)*texture->height()/4),0}; v.imageExtent={1,1,1};}
            copy(command,texture->vkImage(),VK_IMAGE_LAYOUT_GENERAL,buffer,regions.size(),regions.data());
            copied|=1u<<n;
        }
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER,nullptr,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT};
        g_device.vk.CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        r=g_device.vk.EndCommandBuffer(command); if(r) return r;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&command;
        r=g_device.vk.QueueSubmit(g_device.queue(),1,&submit,fence); if(r) return r; submitted=true;
        r=g_device.vk.WaitForFences(g_device.device(),1,&fence,VK_TRUE,5000000000ull); if(r) return r; submitted=false;
        void* data=nullptr; r=g_device.vk.MapMemory(g_device.device(),memory,0,VK_WHOLE_SIZE,0,&data); if(r) return r;
        fprintf(stderr,"HSL_GAMESCOPE_PIXEL_TRACE frame=%llu layers=%u copied=%u diagnosticReadBytes=%u",
            (unsigned long long)m_frameNumber,info->layers.count(),copied,64*((copied&1)+((copied>>1)&1)));
        for(unsigned n=0;n<2;++n) if(copied&(1u<<n)) {fprintf(stderr," %s=",n?"input":"output");
            for(unsigned p=0;p<64;++p) fprintf(stderr,"%02x",static_cast<unsigned char*>(data)[n*64+p]);}
        for (unsigned n=0; n<info->layers.count(); ++n) {
            const auto& layer=info->layers.get(n);
            fprintf(stderr," layer%u=%ux%u,scale:%g,%g,offset:%g,%g,opacity:%g,colorspace:%u",n,
                layer.tex?layer.tex->width():0,layer.tex?layer.tex->height():0,
                layer.scale.x,layer.scale.y,layer.offset.x,layer.offset.y,layer.opacity,unsigned(layer.colorspace));
        }
        fputc('\n',stderr);g_device.vk.UnmapMemory(g_device.device(),memory); return VK_SUCCESS;
    };
    VkResult result=run(); cleanup();
    if(result!=VK_SUCCESS) fprintf(stderr,"HSL_GAMESCOPE_PIXEL_TRACE_ERROR result=%d\n",int(result));
}

void CHslBackend::releaseInput(uint32_t time) {
    for (unsigned key = 0; key < m_keys.size(); ++key) if (m_keys[key]) wlserver_key(key, false, time);
    const int buttons[] = {BTN_LEFT, BTN_MIDDLE, BTN_RIGHT};
    for (unsigned b = 0; b < m_buttons.size(); ++b) if (m_buttons[b]) wlserver_mousebutton(buttons[b], false, time);
    m_keys.reset(); m_buttons.reset(); wlserver_keyboard_release_modifiers();
}
bool CHslBackend::updateKeymap() {
    // X servers may rename an evdev map or use different X keycodes. Map the
    // actual XKB physical names into a compiled evdev reference; never assume
    // host keycode-8. Layout symbols do not change the physical key identity.
    auto* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    const xkb_rule_names rules{"evdev", "pc105", "us", nullptr, nullptr};
    auto* reference = context ? xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS) : nullptr;
    auto* keyboard = XkbGetMap(m_display, XkbAllClientInfoMask, XkbUseCoreKbd);
    bool valid = reference && keyboard && XkbGetNames(m_display, XkbKeyNamesMask, keyboard) == Success &&
                 keyboard->names && keyboard->names->keys;
    std::array<uint32_t, 256> mapping{};
    unsigned mapped = 0;
    if (valid) for (unsigned key = keyboard->min_key_code; key <= keyboard->max_key_code && key < mapping.size(); ++key) {
        char name[5]{}; memcpy(name, keyboard->names->keys[key].name, 4);
        const auto evdev = xkb_keymap_key_by_name(reference, name);
        if (evdev != XKB_KEYCODE_INVALID && evdev > 8 && evdev - 8 < KEY_CNT) {
            mapping[key] = evdev - 8; ++mapped;
        }
    }
    if (keyboard) XkbFreeKeyboard(keyboard, XkbAllComponentsMask, True);
    if (reference) xkb_keymap_unref(reference);
    if (context) xkb_context_unref(context);
    if (!valid || !mapped) return false;
    m_keyMap = mapping;
    fprintf(stderr, "HSL_GAMESCOPE_XKB_MAP mapped=%u\n", mapped);
    return true;
}
bool CHslBackend::PollState() {
    hsl_image_collect();
    if (!m_ready) return false;
    bool changed = false;
    while (XPending(m_display)) {
        XEvent event; XNextEvent(m_display, &event);
        if (event.type == m_xkbEvent) {
            wlserver_lock(); releaseInput(uint32_t(get_time_in_nanos()/1000000)); wlserver_unlock();
            if (!updateKeymap()) error("updated XKB physical key mapping", VK_ERROR_FEATURE_NOT_PRESENT);
            continue;
        }
        if (event.xany.window != m_window) continue;
        if (event.type == ClientMessage && event.xclient.message_type == m_protocols &&
            event.xclient.format == 32 && Atom(event.xclient.data.l[0]) == m_delete) { g_bRun = false; continue; }
        if (event.type == ConfigureNotify && event.xconfigure.width > 0 && event.xconfigure.height > 0 &&
            (uint32_t(event.xconfigure.width) != g_nOutputWidth || uint32_t(event.xconfigure.height) != g_nOutputHeight)) {
            g_nOutputWidth = event.xconfigure.width; g_nOutputHeight = event.xconfigure.height;
            m_resize = true; changed = true;
        }
        if (event.type == UnmapNotify) m_visible = false;
        if (event.type == MapNotify) { m_visible = true; changed = true; }
        wlserver_lock();
        if (event.type == FocusOut || event.type == UnmapNotify) releaseInput(uint32_t(get_time_in_nanos()/1000000));
        if ((event.type == KeyPress || event.type == KeyRelease) && event.xkey.keycode < m_keyMap.size()) {
            const unsigned key = m_keyMap[event.xkey.keycode];
            const bool pressed = event.type == KeyPress;
            if (key && m_keys[key] != pressed) { m_keys[key] = pressed; wlserver_key(key, pressed, event.xkey.time); }
        }
        if (event.type == MotionNotify) wlserver_touchmotion(double(event.xmotion.x)/g_nOutputWidth, double(event.xmotion.y)/g_nOutputHeight, 0, event.xmotion.time);
        if (event.type == ButtonPress || event.type == ButtonRelease) {
            const unsigned button = event.xbutton.button; const bool pressed = event.type == ButtonPress;
            const int buttons[] = {BTN_LEFT, BTN_MIDDLE, BTN_RIGHT};
            if (button >= 1 && button <= 3) { m_buttons[button-1] = pressed; wlserver_mousebutton(buttons[button-1], pressed, event.xbutton.time); }
            else if (pressed && button >= 4 && button <= 7) wlserver_mousewheel(button >= 6 ? (button == 6 ? -1 : 1) : 0, button < 6 ? (button == 4 ? -1 : 1) : 0, event.xbutton.time);
        }
        wlserver_unlock();
    }
    return changed;
}
int CHslConnector::Present(const FrameInfo_t* info, bool) { return m_backend->present(info); }
template<> bool IBackend::Set<CHslBackend>() { return Set(new CHslBackend{}); }
} // namespace gamescope
