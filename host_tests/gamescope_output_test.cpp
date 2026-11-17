// SPDX-License-Identifier: LGPL-2.1-or-later
#include "../native/gamescope/ohos/OutputTransport.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <memory>
#include <set>
#include <utility>
#include <vector>

template<class T> T handle(uintptr_t value) { return reinterpret_cast<T>(value); }
static constexpr uint32_t DRM_FORMAT_ABGR8888 = 1;
int g_nOutputWidth=64, g_nOutputHeight=48;
uint32_t currentOutputWidth=0, currentOutputHeight=0;
bool SetOutputWindowSize(void*, int, int) { return true; }
namespace ohos {
enum class SessionStage { OutputReady };
void ReportRuntimeStage(SessionStage, const char*) {}
}
struct MockVulkan {
    VkImageUsageFlags supported=VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    bool failCreate=false, failView=false, failSemaphore=false, failCopy=false;
    unsigned localTargets=0, viewAttempts=0, semaphoreAttempts=0, drains=0, copies=0;
    uintptr_t serial=10;
    std::vector<VkImageUsageFlags> usages;
    std::vector<VkSwapchainKHR> predecessors, created;
    std::set<VkSwapchainKHR> chains;
    std::set<VkSwapchainKHR> retired;
    std::set<VkImageView> views;
    std::set<VkSemaphore> semaphores;
    std::vector<VkImageMemoryBarrier> barriers;
    std::vector<VkPipelineStageFlags> srcStages,dstStages;
    VkResult GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice,VkSurfaceKHR,VkSurfaceCapabilitiesKHR* caps) {
        caps->supportedUsageFlags=supported; caps->supportedCompositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        caps->currentExtent={64,48}; caps->minImageCount=3; caps->maxImageCount=8;
        caps->supportedTransforms=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        return VK_SUCCESS;
    }
    VkResult GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice,VkSurfaceKHR,uint32_t* count,VkSurfaceFormatKHR* formats) {
        *count=1;
        if (formats) *formats={VK_FORMAT_R8G8B8A8_UNORM,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        return VK_SUCCESS;
    }
    VkResult GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice,const VkPhysicalDeviceImageFormatInfo2*,VkImageFormatProperties2* props) {
        props->imageFormatProperties.maxExtent={8192,8192,1}; return VK_SUCCESS;
    }
    VkResult QueueWaitIdle(VkQueue) { ++drains; return VK_SUCCESS; }
    VkResult CreateSwapchainKHR(VkDevice,const VkSwapchainCreateInfoKHR* info,const VkAllocationCallbacks*,VkSwapchainKHR* chain) {
        assert((info->imageUsage & supported)==info->imageUsage);
        assert(info->imageFormat==VK_FORMAT_R8G8B8A8_UNORM && info->imageColorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
        assert(!info->oldSwapchain || chains.count(info->oldSwapchain));
        assert(!retired.count(info->oldSwapchain));
        if (info->oldSwapchain) retired.insert(info->oldSwapchain);
        usages.push_back(info->imageUsage); predecessors.push_back(info->oldSwapchain);
        if (failCreate) { failCreate=false; return VK_ERROR_FORMAT_NOT_SUPPORTED; }
        if (failCopy && info->imageUsage==VK_IMAGE_USAGE_TRANSFER_DST_BIT) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        *chain=handle<VkSwapchainKHR>(serial++); assert(chains.insert(*chain).second); created.push_back(*chain);
        return VK_SUCCESS;
    }
    VkResult GetSwapchainImagesKHR(VkDevice,VkSwapchainKHR,uint32_t* count,VkImage* images) {
        *count=3;
        if (images) for (unsigned i=0;i<3;++i) images[i]=handle<VkImage>(serial++);
        return VK_SUCCESS;
    }
    VkResult CreateSemaphore(VkDevice,const VkSemaphoreCreateInfo*,const VkAllocationCallbacks*,VkSemaphore* sem) {
        if (++semaphoreAttempts==2 && failSemaphore) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        *sem=handle<VkSemaphore>(serial++); assert(semaphores.insert(*sem).second); return VK_SUCCESS;
    }
    void DestroySemaphore(VkDevice,VkSemaphore sem,const VkAllocationCallbacks*) { assert(semaphores.erase(sem)==1); }
    VkResult CreateImageView(VkDevice,const VkImageViewCreateInfo* info,const VkAllocationCallbacks*,VkImageView* view) {
        assert(info->format==VK_FORMAT_R8G8B8A8_UNORM && info->viewType==VK_IMAGE_VIEW_TYPE_2D);
        const auto* usage=static_cast<const VkImageViewUsageCreateInfo*>(info->pNext);
        assert(usage && usage->usage==VK_IMAGE_USAGE_STORAGE_BIT);
        if (++viewAttempts==2 && failView) return VK_ERROR_FORMAT_NOT_SUPPORTED;
        *view=handle<VkImageView>(serial++); assert(views.insert(*view).second); return VK_SUCCESS;
    }
    void DestroyImageView(VkDevice,VkImageView view,const VkAllocationCallbacks*) { assert(views.erase(view)==1); }
    void DestroySwapchainKHR(VkDevice,VkSwapchainKHR chain,const VkAllocationCallbacks*) { assert(chains.erase(chain)==1); retired.erase(chain); }
    void CmdPipelineBarrier(VkCommandBuffer,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags,
        uint32_t,const VkMemoryBarrier*,uint32_t,const VkBufferMemoryBarrier*,uint32_t count,const VkImageMemoryBarrier* images) {
        srcStages.push_back(src); dstStages.push_back(dst); barriers.insert(barriers.end(),images,images+count);
    }
    void CmdCopyImage(VkCommandBuffer,VkImage,VkImageLayout src,VkImage,VkImageLayout dst,uint32_t count,const VkImageCopy* copy) {
        assert(src==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && dst==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        assert(count==1 && copy->extent.width==64 && copy->extent.height==48 && copy->extent.depth==1);
        ++copies;
    }
};
struct Device {
    MockVulkan vk;
    VkDevice device() const { return VK_NULL_HANDLE; }
    VkPhysicalDevice physDev() const { return VK_NULL_HANDLE; }
    VkQueue queue() const { return VK_NULL_HANDLE; }
    void waitIdle() {}
} g_device;
class CVulkanTexture {
public:
    struct createFlags { bool bStorage=false,bSampled=false,bTransferSrc=false; };
    VkImage m_vkImage=VK_NULL_HANDLE;
    VkImageView m_srgbView=VK_NULL_HANDLE;
    uint32_t m_width=0,m_height=0,m_contentWidth=0,m_contentHeight=0,m_depth=0,m_drmFormat=0;
    VkFormat m_format=VK_FORMAT_UNDEFINED;
    bool m_bOutputImage=false,m_bInitialized=false;
    bool BInitOhosStorageOutput(VkImage,uint32_t,uint32_t);
    bool BInit(uint32_t,uint32_t,uint32_t,uint32_t,createFlags flags) {
        assert(flags.bStorage && flags.bSampled && flags.bTransferSrc); ++g_device.vk.localTargets; return true;
    }
    ~CVulkanTexture() { if (m_srgbView) g_device.vk.DestroyImageView(VK_NULL_HANDLE,m_srgbView,nullptr); }
};
template<class T> struct Owner : std::shared_ptr<T> {
    Owner& operator=(T* value) { this->reset(value); return *this; }
};
class COhosBackend {
public:
    struct Output {
        bool storage=false;
        VkSwapchainKHR swapchain=VK_NULL_HANDLE;
        std::vector<VkImage> images;
        std::vector<Owner<CVulkanTexture>> targets;
        std::vector<VkSemaphore> rendered;
    } m_output;
    void* m_window=nullptr;
    VkSurfaceKHR m_surface=VK_NULL_HANDLE;
    bool m_resize=true,m_failed=false;
    struct Connector { struct Mode { uint32_t uWidth=0,uHeight=0; } mode; } m_connector;
    bool error(const char*,VkResult) { m_failed=true; return false; }
#include "gamescope_output_cleanup.h"
    bool recreateOutput();
    ~COhosBackend() { destroyOutput(m_output); }
};
#include "gamescope_output_methods.h"

static void clean() {
    assert(g_device.vk.chains.empty() && g_device.vk.retired.empty() && g_device.vk.views.empty() && g_device.vk.semaphores.empty());
    g_device.vk=MockVulkan{};
}
int main() {
    {
        COhosBackend backend; assert(backend.recreateOutput());
        assert(backend.m_output.storage && g_device.vk.localTargets==0 && g_device.vk.views.size()==3);
        for (auto& target:backend.m_output.targets) assert(target->m_bOutputImage && target->m_bInitialized);
        const auto first=backend.m_output.swapchain;
        assert(backend.recreateOutput());
        assert(g_device.vk.predecessors.back()==first && g_device.vk.chains.size()==1 && g_device.vk.drains==1);
    } clean();
    {
        COhosBackend backend; assert(backend.recreateOutput());
        g_device.vk.failCreate=true;
        assert(backend.recreateOutput() && !backend.m_output.storage);
        assert(!g_device.vk.predecessors.back()); // previous chain was retired by the failed call
    } clean();
    {
        g_device.vk.supported=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        COhosBackend backend; assert(backend.recreateOutput());
        assert(!backend.m_output.storage && g_device.vk.localTargets==3 && g_device.vk.views.empty());
    } clean();
    for (unsigned failure=0;failure<3;++failure) {
        g_device.vk.failCreate=failure==0; g_device.vk.failView=failure==1; g_device.vk.failSemaphore=failure==2;
        {
            COhosBackend backend; assert(backend.recreateOutput());
            assert(!backend.m_output.storage && !backend.m_failed && g_device.vk.usages.size()==2);
            assert(g_device.vk.usages[0]==VK_IMAGE_USAGE_STORAGE_BIT && g_device.vk.usages[1]==VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            if (failure==0) assert(!g_device.vk.predecessors[1]);
            else assert(g_device.vk.predecessors[1]==g_device.vk.created[0]);
            assert(g_device.vk.chains.size()==1 && g_device.vk.views.empty() && g_device.vk.semaphores.size()==3);
        } clean();
    }
    {
        g_device.vk.failView=g_device.vk.failCopy=true;
        COhosBackend backend; assert(!backend.recreateOutput() && backend.m_failed);
    } clean();
    {
        g_device.vk.supported=0;
        COhosBackend backend; assert(!backend.recreateOutput() && g_device.vk.usages.empty());
    } clean();
    {
        g_device.vk.supported=VK_IMAGE_USAGE_STORAGE_BIT;
        COhosBackend backend; assert(backend.recreateOutput() && backend.m_output.storage);
    } clean();
    assert(VppGamescopeOutputWaitStage(true)==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    assert(VppGamescopeOutputWaitStage(false)==VK_PIPELINE_STAGE_TRANSFER_BIT);
    const auto output=handle<VkImage>(1), target=handle<VkImage>(2);
    VppGamescopeRecordOutput(g_device.vk,VK_NULL_HANDLE,output,output,64,48,true);
    assert(g_device.vk.copies==0 && g_device.vk.barriers.size()==1);
    auto& direct=g_device.vk.barriers[0];
    assert(direct.image==output && direct.oldLayout==VK_IMAGE_LAYOUT_GENERAL && direct.newLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    assert(direct.srcAccessMask==VK_ACCESS_SHADER_WRITE_BIT && direct.dstAccessMask==0);
    assert(g_device.vk.srcStages[0]==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    clean();
    VppGamescopeRecordOutput(g_device.vk,VK_NULL_HANDLE,target,output,64,48,false);
    assert(g_device.vk.copies==1 && g_device.vk.barriers.size()==4);
    const auto& copied=g_device.vk.barriers;
    assert(copied[0].image==target && copied[0].newLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    assert(copied[1].image==output && copied[1].newLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    assert(copied[2].newLayout==VK_IMAGE_LAYOUT_GENERAL && copied[3].newLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    for (const auto& b:copied) assert(b.srcQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED && b.dstQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED);
    clean();
    std::puts("Gamescope actual output: storage, copy, resize, three fallback failures, terminal failure, no-capability, borrowed views and output barriers: PASS");
}
