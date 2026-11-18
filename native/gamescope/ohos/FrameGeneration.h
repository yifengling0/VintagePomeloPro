// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "winfg/framegen.hpp"
#include <algorithm>
#include <array>
#include <vector>

// One compute owner per embedded compositor. Descriptor/UBO updates occur only
// after the preceding generation fence; a busy GPU bypasses FG without a wait.
class VppWinfgEngine {
public:
    ~VppWinfgEngine() { Destroy(); }
    bool Init(VkInstance instance, PFN_vkGetInstanceProcAddr get, VkPhysicalDevice physical,
              VkDevice device, uint32_t family, VkQueue queue) {
        if (device_) return ready_;
        device_=device; queue_=queue;
        auto gdpa=reinterpret_cast<PFN_vkGetDeviceProcAddr>(get(instance,"vkGetDeviceProcAddr"));
        if (!gdpa) return false;
#define LOAD(name) dd_.name=reinterpret_cast<PFN_vk##name>(gdpa(device,"vk" #name)); if (!dd_.name) return false
        LOAD(CreateImage); LOAD(DestroyImage); LOAD(GetImageMemoryRequirements);
        LOAD(CreateImageView); LOAD(DestroyImageView); LOAD(AllocateMemory); LOAD(FreeMemory);
        LOAD(BindImageMemory); LOAD(CreateBuffer); LOAD(DestroyBuffer); LOAD(GetBufferMemoryRequirements);
        LOAD(BindBufferMemory); LOAD(MapMemory); LOAD(UnmapMemory); LOAD(CreateSampler); LOAD(DestroySampler);
        LOAD(CreateShaderModule); LOAD(DestroyShaderModule); LOAD(CreateDescriptorSetLayout); LOAD(DestroyDescriptorSetLayout);
        LOAD(CreatePipelineLayout); LOAD(DestroyPipelineLayout); LOAD(CreateComputePipelines); LOAD(DestroyPipeline);
        LOAD(CreateDescriptorPool); LOAD(DestroyDescriptorPool); LOAD(ResetDescriptorPool);
        LOAD(AllocateDescriptorSets); LOAD(UpdateDescriptorSets); LOAD(CmdBindPipeline); LOAD(CmdBindDescriptorSets);
        LOAD(CmdDispatch); LOAD(CmdPipelineBarrier); LOAD(CmdClearColorImage); LOAD(CmdCopyImage); LOAD(DeviceWaitIdle);
        LOAD(CreateCommandPool); LOAD(DestroyCommandPool); LOAD(AllocateCommandBuffers); LOAD(BeginCommandBuffer);
        LOAD(EndCommandBuffer); LOAD(QueueSubmit); LOAD(CreateFence); LOAD(DestroyFence); LOAD(ResetFences);
#undef LOAD
        resetCommand_=reinterpret_cast<PFN_vkResetCommandBuffer>(gdpa(device,"vkResetCommandBuffer"));
        fenceStatus_=reinterpret_cast<PFN_vkGetFenceStatus>(gdpa(device,"vkGetFenceStatus"));
        id_.GetPhysicalDeviceMemoryProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get(instance,"vkGetPhysicalDeviceMemoryProperties"));
        auto properties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get(instance,"vkGetPhysicalDeviceProperties"));
        auto formats=reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(get(instance,"vkGetPhysicalDeviceFormatProperties"));
        auto families=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(get(instance,"vkGetPhysicalDeviceQueueFamilyProperties"));
        if (!resetCommand_ || !fenceStatus_ || !properties || !formats || !families || !id_.GetPhysicalDeviceMemoryProperties) return false;
        VkPhysicalDeviceProperties caps{}; properties(physical,&caps);
        if (caps.limits.maxComputeWorkGroupInvocations<256 || caps.limits.maxComputeWorkGroupSize[0]<16 || caps.limits.maxComputeWorkGroupSize[1]<16) return false;
        for (auto format:{VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R32_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT}) {
            VkFormatProperties p{}; formats(physical,format,&p);
            const auto need=VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            if ((p.optimalTilingFeatures&need)!=need) return false;
        }
        uint32_t count=0; families(physical,&count,nullptr);
        std::vector<VkQueueFamilyProperties> queues(count); families(physical,&count,queues.data());
        if (family>=count || !(queues[family].queueFlags&VK_QUEUE_COMPUTE_BIT)) return false;
        timestampPeriod_=caps.limits.timestampPeriod;
        timestampBits_=queues[family].timestampValidBits;
        createQuery_=reinterpret_cast<PFN_vkCreateQueryPool>(gdpa(device,"vkCreateQueryPool"));
        destroyQuery_=reinterpret_cast<PFN_vkDestroyQueryPool>(gdpa(device,"vkDestroyQueryPool"));
        resetQuery_=reinterpret_cast<PFN_vkCmdResetQueryPool>(gdpa(device,"vkCmdResetQueryPool"));
        writeTimestamp_=reinterpret_cast<PFN_vkCmdWriteTimestamp>(gdpa(device,"vkCmdWriteTimestamp"));
        getQuery_=reinterpret_cast<PFN_vkGetQueryPoolResults>(gdpa(device,"vkGetQueryPoolResults"));
        if (timestampBits_ && createQuery_ && destroyQuery_ && resetQuery_ && writeTimestamp_ && getQuery_) {
            VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; query.queryType=VK_QUERY_TYPE_TIMESTAMP; query.queryCount=2;
            if (createQuery_(device,&query,nullptr,&query_)!=VK_SUCCESS) query_=VK_NULL_HANDLE;
        }
        winfg::Config config; config.enabled=true; config.model=3; config.perfPreset=1;
        config.multiplier=2; config.gmMode=2; config.capture=false;
        fg_.configure(config);
        if (!fg_.init(&dd_,&id_,physical,device,family,queue)) return false;
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pool.queueFamilyIndex=family;
        if (dd_.CreateCommandPool(device,&pool,nullptr,&pool_)!=VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command.commandPool=pool_; command.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; command.commandBufferCount=1;
        if (dd_.AllocateCommandBuffers(device,&command,&command_)!=VK_SUCCESS) return false;
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fence.flags=VK_FENCE_CREATE_SIGNALED_BIT;
        if (dd_.CreateFence(device,&fence,nullptr,&fence_)!=VK_SUCCESS) return false;
        ready_=true; return true;
    }
    bool Idle() {
        if (!ready_ || fenceStatus_(device_,fence_)!=VK_SUCCESS) return false;
        if (inFlight_) { inFlight_=false; ++completed_; sceneCut_=fg_.sceneCut(); }
        if (queryPending_ && query_) {
            uint64_t times[2]{};
            if (getQuery_(device_,query_,0,2,sizeof(times),times,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS) {
                const auto mask=timestampBits_==64?UINT64_MAX:(1ull<<timestampBits_)-1;
                gpuNs_=uint64_t(double((times[1]-times[0])&mask)*timestampPeriod_);
                queryPending_=false;
            }
        }
        return true;
    }
    uint64_t GpuNs() const { return gpuNs_; }
    uint64_t Completed() const { return completed_; }
    bool SceneCut() const { return sceneCut_; }
    bool Resize(VkExtent2D size) {
        if (!Idle()) return false;
        return fg_.onResize(size,VK_FORMAT_R8G8B8A8_UNORM);
    }
    bool Generate(VkImage prev, VkImageView prevView, VkImage curr, VkImageView currView,
                  VkImage out, VkImageView outView) {
        if (!Idle() || !fg_.valid()) return false;
        if (resetCommand_(command_,0)!=VK_SUCCESS) return false;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (dd_.BeginCommandBuffer(command_,&begin)!=VK_SUCCESS) return false;
        if (query_) { resetQuery_(command_,query_,0,2); writeTimestamp_(command_,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,query_,0); }
        // Gamescope internal textures stay GENERAL. Win-FG samples the same
        // encoded UNORM values in READ_ONLY, then returns ownership/layout.
        Barrier(prev,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Barrier(curr,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Barrier(out,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
        const bool recorded=fg_.record(command_,prevView,currView,outView,0.5f);
        Barrier(prev,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
        Barrier(curr,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
        Barrier(out,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
        if (query_) writeTimestamp_(command_,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,query_,1);
        if (dd_.EndCommandBuffer(command_)!=VK_SUCCESS || !recorded) return false;
        if (dd_.ResetFences(device_,1,&fence_)!=VK_SUCCESS) return false;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&command_;
        if (dd_.QueueSubmit(queue_,1,&submit,fence_)!=VK_SUCCESS) { ready_=false; return false; }
        queryPending_=query_!=VK_NULL_HANDLE;
        inFlight_=true;
        return true;
    }
    void Destroy() {
        // Only session/resize/disable boundaries drain the device.
        if (!device_) return;
        if (dd_.DeviceWaitIdle) dd_.DeviceWaitIdle(device_);
        fg_.destroy();
        if (query_) destroyQuery_(device_,query_,nullptr);
        if (fence_) dd_.DestroyFence(device_,fence_,nullptr);
        if (pool_) dd_.DestroyCommandPool(device_,pool_,nullptr);
        fence_=VK_NULL_HANDLE; pool_=VK_NULL_HANDLE; query_=VK_NULL_HANDLE; device_=VK_NULL_HANDLE; ready_=false;
        gpuNs_=completed_=0; queryPending_=false;
        inFlight_=sceneCut_=false;
    }
private:
    void Barrier(VkImage image,VkImageLayout from,VkImageLayout to) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask=from==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT; b.oldLayout=from; b.newLayout=to;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.image=image; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        dd_.CmdPipelineBarrier(command_,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
    }
    winfg::DeviceDispatch dd_{}; winfg::InstanceDispatch id_{}; winfg::FrameGen fg_;
    PFN_vkResetCommandBuffer resetCommand_=nullptr; PFN_vkGetFenceStatus fenceStatus_=nullptr;
    VkDevice device_=VK_NULL_HANDLE; VkQueue queue_=VK_NULL_HANDLE;
    VkCommandPool pool_=VK_NULL_HANDLE; VkCommandBuffer command_=VK_NULL_HANDLE; VkFence fence_=VK_NULL_HANDLE;
    bool ready_=false;
    PFN_vkCreateQueryPool createQuery_=nullptr; PFN_vkDestroyQueryPool destroyQuery_=nullptr;
    PFN_vkCmdResetQueryPool resetQuery_=nullptr; PFN_vkCmdWriteTimestamp writeTimestamp_=nullptr;
    PFN_vkGetQueryPoolResults getQuery_=nullptr;
    VkQueryPool query_=VK_NULL_HANDLE;
    uint32_t timestampBits_=0; float timestampPeriod_=0;
    uint64_t gpuNs_=0,completed_=0; bool queryPending_=false;
    bool inFlight_=false,sceneCut_=false;
};
