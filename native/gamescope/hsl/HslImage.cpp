// SPDX-License-Identifier: LGPL-2.1-or-later
// Private resource transport; never a DMA-BUF implementation. Native objects
// stay in the receiver ABI. Only tickets and GNU image proxies reach here.
#include "HslImage.h"
#include "hsl_venus_shared_image.h"
#include "hsl-image-v1-protocol.h"
#include "wlserver.hpp"
#include "main.hpp"
#include <drm_fourcc.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "wlr_begin.hpp"
#include <wlr/interfaces/wlr_buffer.h>
#include "wlr_end.hpp"
#include <array>
#include <memory>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <dlfcn.h>
#include <chrono>

namespace {
using Control = VkResult (*)(VkQueue,uint32_t*,uint32_t*,VkImage*,VkSemaphore);
Control control = nullptr;
PFN_vkDestroyFence destroyFence = nullptr;
PFN_vkGetFenceStatus getFenceStatus = nullptr;
// Aggregate initialization without hiding uninitialized fields/warnings.
template<class T> T vkinfo(VkStructureType type) { T value{};value.sType=type;return value; }
struct Pool {
    uint32_t words[HSL_IMAGE_WORDS]{};
    std::array<VkImage,HSL_IMAGE_MAX_SLOTS> images{};
    std::array<bool,HSL_IMAGE_MAX_SLOTS> created{};
    bool offered=false,imported=false,closed=false;
    VkResult call(uint32_t op,uint32_t index=0,VkImage* out=nullptr,VkSemaphore sem=VK_NULL_HANDLE) {
        uint32_t request[HSL_IMAGE_WORDS],reply[HSL_IMAGE_WORDS]{};
        memcpy(request,words,sizeof(request));request[HI_OP]=op;request[HI_INDEX]=index;
        auto result=control(g_device.queue(),request,reply,out,sem);
        if(op==HI_OFFER&&result==VK_SUCCESS)memcpy(words+HI_KEY,reply+HI_KEY,4*sizeof(uint32_t));
        return result;
    }
    // Invoked only on the render thread after all buffers/jobs release their
    // references. NOT_READY retains this generation for a later real retry.
    VkResult retire() {
        if(!offered)return VK_SUCCESS;
        if(imported) {
            for(auto& image:images)if(image){g_device.vk.DestroyImage(g_device.device(),image,nullptr);image=VK_NULL_HANDLE;}
            imported=false;
        }
        auto result=call(HI_DETACH);
        if(result==VK_SUCCESS||result==VK_ERROR_OUT_OF_DATE_KHR)offered=false;
        return result;
    }
};
struct Buffer {
    wlr_buffer base{}; // First member: wlr_buffer implementation identity.
    wl_resource* resource=nullptr;
    wl_listener release{};
    std::shared_ptr<Pool> pool;
    uint32_t index=0;
    bool awaiting=false;
};
struct Job {
    std::shared_ptr<Pool> pool;
    Buffer* buffer=nullptr; // wlr_buffer lock held until release notification.
    gamescope::OwningRc<CVulkanTexture> texture;
    VkCommandPool commands=VK_NULL_HANDLE;
    VkSemaphore ready=VK_NULL_HANDLE,done=VK_NULL_HANDLE;
    VkFence retired=VK_NULL_HANDLE;
    bool submitted=false,released=false,retireQueued=false,cancelled=false;
    std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
    ~Job() {
        if(ready)g_device.vk.DestroySemaphore(g_device.device(),ready,nullptr);
        if(done)g_device.vk.DestroySemaphore(g_device.device(),done,nullptr);
        if(retired)destroyFence(g_device.device(),retired,nullptr);
        if(commands)g_device.vk.DestroyCommandPool(g_device.device(),commands,nullptr);
    }
};
std::mutex poolsMutex;
std::vector<std::shared_ptr<Pool>> pools;
std::vector<std::unique_ptr<Job>> jobs; // Render thread only.
wl_global* global=nullptr;
void failure(const char* operation,VkResult result) {
    fprintf(stderr,"HSL_IMAGE_COMPOSITOR operation=%s result=%d\n",operation,result);
}
void destroyResource(wl_client*,wl_resource* resource){wl_resource_destroy(resource);}
const struct wl_buffer_interface bufferResourceImpl{destroyResource};
bool resourceIsBuffer(wl_resource* resource) {
    return wl_resource_instance_of(resource,&wl_buffer_interface,&bufferResourceImpl);
}
wlr_buffer* fromResource(wl_resource* resource) {
    auto* buffer=static_cast<Buffer*>(wl_resource_get_user_data(resource));
    if(!buffer)return nullptr;
    buffer->awaiting=true;
    return &buffer->base;
}
void destroyBuffer(wlr_buffer* base) {
    auto* buffer=reinterpret_cast<Buffer*>(base);
    wl_list_remove(&buffer->release.link);
    wlr_buffer_finish(base);
    if(buffer->resource)wl_resource_set_user_data(buffer->resource,nullptr);
    delete buffer;
}
const wlr_buffer_impl bufferImpl{.destroy=destroyBuffer};
void dropResource(wl_resource* resource) {
    auto* buffer=static_cast<Buffer*>(wl_resource_get_user_data(resource));
    if(!buffer)return;
    buffer->resource=nullptr;wlr_buffer_drop(&buffer->base);
}
void sendRelease(Buffer* buffer) {
    if(buffer->awaiting&&buffer->resource)wl_buffer_send_release(buffer->resource);
    buffer->awaiting=false;
}
void onRelease(wl_listener* listener,void*) {
    auto* buffer=reinterpret_cast<Buffer*>(reinterpret_cast<char*>(listener)-offsetof(Buffer,release));
    sendRelease(buffer);
}
void createBuffer(wl_client* client,wl_resource* resource,uint32_t id,uint32_t index) {
    auto pool=*static_cast<std::shared_ptr<Pool>*>(wl_resource_get_user_data(resource));
    if(!pool->offered||index>=pool->words[HI_COUNT]||pool->created[index]) {
        wl_resource_post_error(resource,0,"Invalid or duplicate HSL image slot");return;
    }
    auto* buffer=new(std::nothrow) Buffer;
    if(!buffer){wl_client_post_no_memory(client);return;}
    buffer->resource=wl_resource_create(client,&wl_buffer_interface,1,id);
    if(!buffer->resource){delete buffer;wl_client_post_no_memory(client);return;}
    buffer->pool=pool;buffer->index=index;pool->created[index]=true;
    wlr_buffer_init(&buffer->base,&bufferImpl,pool->words[HI_WIDTH],pool->words[HI_HEIGHT]);
    wl_resource_set_implementation(buffer->resource,&bufferResourceImpl,buffer,dropResource);
    buffer->release.notify=onRelease;wl_signal_add(&buffer->base.events.release,&buffer->release);
}
const struct hsl_image_pool_v1_interface poolImpl{destroyResource,createBuffer};
void createPool(wl_client* client,wl_resource*,uint32_t id,uint32_t width,uint32_t height,uint32_t format,uint32_t usage,uint32_t count) {
    auto* resource=wl_resource_create(client,&hsl_image_pool_v1_interface,1,id);
    if(!resource){wl_client_post_no_memory(client);return;}
    auto pool=std::make_shared<Pool>();
    pool->words[HI_VERSION]=HSL_IMAGE_VERSION;pool->words[HI_WIDTH]=width;pool->words[HI_HEIGHT]=height;
    pool->words[HI_FORMAT]=format;pool->words[HI_USAGE]=usage;pool->words[HI_COUNT]=count;
    auto* owner=new(std::nothrow) std::shared_ptr<Pool>(pool);
    if(!owner){wl_resource_destroy(resource);wl_client_post_no_memory(client);return;}
    wl_resource_set_implementation(resource,&poolImpl,owner,[](wl_resource* r){delete static_cast<std::shared_ptr<Pool>*>(wl_resource_get_user_data(r));});
    // Keep every generation until the render thread has retired its endpoint.
    {std::lock_guard<std::mutex> lock(poolsMutex);pools.push_back(pool);}
    auto result=pool->call(HI_OFFER);
    if(result!=VK_SUCCESS){hsl_image_pool_v1_send_failed(resource,result);return;}
    pool->offered=true;
    wl_array descriptor{};descriptor.size=sizeof(pool->words);descriptor.data=pool->words;
    hsl_image_pool_v1_send_offered(resource,&descriptor);
}
const struct hsl_image_manager_v1_interface managerImpl{destroyResource,createPool};
void bindManager(wl_client* client,void*,uint32_t,uint32_t id) {
    auto* resource=wl_resource_create(client,&hsl_image_manager_v1_interface,1,id);
    if(!resource){wl_client_post_no_memory(client);return;}
    wl_resource_set_implementation(resource,&managerImpl,nullptr,nullptr);
}
}

bool hsl_image_init(wl_display* display) {
    // The WSI wrapper serializes private ICD control and Vulkan submissions on
    // the same real queue. Never dlsym the unprotected ICD entry in this server.
    Dl_info identity{};
    void* library=dladdr(reinterpret_cast<void*>(g_device.vk.QueueSubmit),&identity)&&identity.dli_fname?
        dlopen(identity.dli_fname,RTLD_NOW|RTLD_NOLOAD):nullptr;
    control=library?reinterpret_cast<Control>(dlsym(library,"hsl_image_control")):nullptr;
    if(library)dlclose(library);
    destroyFence=reinterpret_cast<PFN_vkDestroyFence>(g_device.vk.GetDeviceProcAddr(g_device.device(),"vkDestroyFence"));
    getFenceStatus=reinterpret_cast<PFN_vkGetFenceStatus>(g_device.vk.GetDeviceProcAddr(g_device.device(),"vkGetFenceStatus"));
    if(!control||!destroyFence||!getFenceStatus)return false;
    static const wlr_buffer_resource_interface iface{"hsl_image_v1",resourceIsBuffer,fromResource};
    wlr_buffer_register_resource_interface(&iface);
    global=wl_global_create(display,&hsl_image_manager_v1_interface,1,nullptr,bindManager);
    return global!=nullptr;
}
bool hsl_image_is_buffer(wlr_buffer* buffer){return buffer&&buffer->impl==&bufferImpl;}

gamescope::OwningRc<CVulkanTexture> hsl_image_snapshot(wlr_buffer* base) {
    auto* buffer=reinterpret_cast<Buffer*>(base);
    auto pool=buffer->pool;
    hsl_image_collect();
    if(pool->closed)return nullptr;
    if(!pool->imported) {
        auto result=pool->call(HI_IMPORT,0,pool->images.data());
        if(result!=VK_SUCCESS){failure("import",result);return nullptr;}
        pool->imported=true;
    }
    auto job=std::make_unique<Job>();job->pool=pool;
    job->texture=new CVulkanTexture();
    CVulkanTexture::createFlags flags;flags.bSampled=true;flags.bTransferSrc=true;flags.bTransferDst=true;
    if(!job->texture->BInit(base->width,base->height,1,DRM_FORMAT_ABGR8888,flags))return nullptr;
    auto si=vkinfo<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
    auto result=g_device.vk.CreateSemaphore(g_device.device(),&si,nullptr,&job->ready);
    if(result==VK_SUCCESS)result=g_device.vk.CreateSemaphore(g_device.device(),&si,nullptr,&job->done);
    auto fi=vkinfo<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    if(result==VK_SUCCESS)result=g_device.vk.CreateFence(g_device.device(),&fi,nullptr,&job->retired);
    auto pi=vkinfo<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);pi.queueFamilyIndex=g_device.queueFamily();
    if(result==VK_SUCCESS)result=g_device.vk.CreateCommandPool(g_device.device(),&pi,nullptr,&job->commands);
    VkCommandBuffer command=VK_NULL_HANDLE;
    auto ai=vkinfo<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    ai.commandPool=job->commands;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;
    if(result==VK_SUCCESS)result=g_device.vk.AllocateCommandBuffers(g_device.device(),&ai,&command);
    auto begin=vkinfo<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(result==VK_SUCCESS)result=g_device.vk.BeginCommandBuffer(command,&begin);
    if(result!=VK_SUCCESS){failure("snapshot allocate",result);return nullptr;}
    std::array<VkImageMemoryBarrier,2> barriers{};
    for(auto& barrier:barriers){barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};}
    auto& source=barriers[0];auto& target=barriers[1];
    source.oldLayout=source.newLayout=VK_IMAGE_LAYOUT_GENERAL;source.srcQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;
    source.dstQueueFamilyIndex=g_device.queueFamily();source.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;source.image=pool->images[buffer->index];
    target.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;target.newLayout=VK_IMAGE_LAYOUT_GENERAL;
    target.srcQueueFamilyIndex=target.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;target.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;target.image=job->texture->vkImage();
    g_device.vk.CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,barriers.data());
    VkImageCopy copy{{VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},{0,0,0},{VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},{0,0,0},{uint32_t(base->width),uint32_t(base->height),1}};
    g_device.vk.CmdCopyImage(command,source.image,VK_IMAGE_LAYOUT_GENERAL,target.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
    source.srcQueueFamilyIndex=g_device.queueFamily();source.dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;
    source.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;source.dstAccessMask=0;
    target.oldLayout=VK_IMAGE_LAYOUT_GENERAL;target.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;target.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    g_device.vk.CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,2,barriers.data());
    result=g_device.vk.EndCommandBuffer(command);
    if(result==VK_SUCCESS)result=pool->call(HI_CONSUME,buffer->index,nullptr,job->ready);
    if(result!=VK_SUCCESS){
        failure("consume",result);
        // Swapchain destruction/resize retires this generation, not the
        // compositor. No copy was submitted, so discard this old commit.
        if(result==VK_ERROR_OUT_OF_DATE_KHR)pool->closed=true;
        return nullptr;
    }
    const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
    auto submit=vkinfo<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&job->ready;submit.pWaitDstStageMask=&stage;
    submit.commandBufferCount=1;submit.pCommandBuffers=&command;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&job->done;
    result=g_device.vk.QueueSubmit(g_device.queue(),1,&submit,VK_NULL_HANDLE);
    if(result!=VK_SUCCESS){failure("copy submit",result);g_device.vk.QueueWaitIdle(g_device.queue());return nullptr;}
    // This means a same-queue GPU dependency, not CPU/GPU completion. Later
    // Gamescope compute submissions are ordered after the copy and barrier.
    job->texture->queueFamily=g_device.queueFamily();job->texture->markLocalQueueDependency();
    job->submitted=true;
    wlserver_lock();wlr_buffer_lock(base);wlserver_unlock();job->buffer=buffer;
    auto texture=job->texture;
    jobs.push_back(std::move(job));
    static uint64_t count=0;
    if(++count==1||count%120==0)fprintf(stderr,"HSL_GAMESCOPE_GPU_INPUT frames=%llu cpuPixelCopies=0\n",(unsigned long long)count);
    hsl_image_collect();
    return texture;
}

void hsl_image_collect() {
    for(auto it=jobs.begin();it!=jobs.end();) {
        auto& job=**it;
        if(!job.released&&!job.cancelled) {
            auto result=job.pool->closed?VK_ERROR_OUT_OF_DATE_KHR:
                job.pool->call(HI_RELEASE,job.buffer->index,nullptr,job.done);
            if(result==VK_NOT_READY&&std::chrono::steady_clock::now()-job.started<std::chrono::seconds(5)){++it;continue;}
            if(result==VK_ERROR_OUT_OF_DATE_KHR) {
                failure("release retired generation",result);
                job.pool->closed=true;job.cancelled=true;
                // The producer has closed the generation. HI_RELEASE did NOT
                // succeed. Keep the local image/buffer until our GPU copy's
                // retirement fence completes; other generations keep running.
            } else {
                if(result!=VK_SUCCESS){failure("release",result==VK_NOT_READY?VK_TIMEOUT:result);g_bRun=false;++it;continue;}
                job.released=true;
                // HI_ACQUIRE still supplies the producer's GPU dependency.
                wlserver_lock();sendRelease(job.buffer);wlr_buffer_unlock(&job.buffer->base);wlserver_unlock();job.buffer=nullptr;
            }
        }
        if(!job.retireQueued) {
            auto submit=vkinfo<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
            auto result=g_device.vk.QueueSubmit(g_device.queue(),1,&submit,job.retired);
            if(result!=VK_SUCCESS){failure("retire submit",result);++it;continue;}
            job.retireQueued=true;
        }
        auto result=getFenceStatus(g_device.device(),job.retired);
        if(result==VK_SUCCESS) {
            if(job.buffer){wlserver_lock();sendRelease(job.buffer);wlr_buffer_unlock(&job.buffer->base);wlserver_unlock();job.buffer=nullptr;}
            it=jobs.erase(it);
        }
        else {if(result!=VK_NOT_READY)failure("retire fence",result);++it;}
    }
    std::lock_guard<std::mutex> lock(poolsMutex);
    for(auto it=pools.begin();it!=pools.end();) {
        if(it->use_count()!=1){++it;continue;}
        auto result=(*it)->retire();
        if(result==VK_SUCCESS||result==VK_ERROR_OUT_OF_DATE_KHR)it=pools.erase(it);
        else {if(result!=VK_NOT_READY)failure("detach",result);++it;}
    }
}
void hsl_image_shutdown() {
    if(!global)return;
    // Called after wl_display_destroy_clients, with the render thread stopped.
    // Fatal/closed peers still need their pending GNU resources safely drained.
    g_device.vk.QueueWaitIdle(g_device.queue());
    for(auto& job:jobs)if(job->buffer){wlr_buffer_unlock(&job->buffer->base);job->buffer=nullptr;}
    jobs.clear();
    for(auto& pool:pools){auto result=pool->retire();if(result==VK_NOT_READY){g_device.vk.QueueWaitIdle(g_device.queue());result=pool->retire();}if(result!=VK_SUCCESS&&result!=VK_ERROR_OUT_OF_DATE_KHR)failure("shutdown detach",result);}
    pools.clear();global=nullptr;
}
