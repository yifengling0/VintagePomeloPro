// SPDX-License-Identifier: LGPL-2.1-or-later
// Executes the imported compute graph. CPU upload/readback exists ONLY here.
#include "FrameGeneration.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#define CHECK(call) do { const auto r=(call); if(r!=VK_SUCCESS) { fprintf(stderr,"%s: %d\n",#call,int(r)); std::abort(); } } while(0)
struct Image { VkImage image{}; VkImageView view{}; VkDeviceMemory memory{}; };
static VKAPI_ATTR VkResult VKAPI_CALL FailDescriptor(VkDevice,const VkDescriptorSetAllocateInfo*,VkDescriptorSet*) {
    return VK_ERROR_OUT_OF_POOL_MEMORY;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FailureDeviceProc(VkDevice device,const char* name) {
    return !std::strcmp(name,"vkAllocateDescriptorSets")?reinterpret_cast<PFN_vkVoidFunction>(FailDescriptor):vkGetDeviceProcAddr(device,name);
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FailureInstanceProc(VkInstance instance,const char* name) {
    return !std::strcmp(name,"vkGetDeviceProcAddr")?reinterpret_cast<PFN_vkVoidFunction>(FailureDeviceProc):vkGetInstanceProcAddr(instance,name);
}
int main() {
    setvbuf(stdout,nullptr,_IONBF,0);
    alarm(90);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; instanceInfo.pApplicationInfo=&app;
    VkInstance instance; CHECK(vkCreateInstance(&instanceInfo,nullptr,&instance));
    uint32_t count=0; CHECK(vkEnumeratePhysicalDevices(instance,&count,nullptr)); assert(count);
    std::vector<VkPhysicalDevice> devices(count); CHECK(vkEnumeratePhysicalDevices(instance,&count,devices.data()));
    const auto physical=devices[0];
    VkPhysicalDeviceMemoryProperties memory{}; vkGetPhysicalDeviceMemoryProperties(physical,&memory);
    auto memType=[&](uint32_t bits,VkMemoryPropertyFlags flags) {
        for(uint32_t i=0;i<memory.memoryTypeCount;++i) if((bits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&flags)==flags) return i;
        std::abort();
    };
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);
    std::vector<VkQueueFamilyProperties> families(count); vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data());
    uint32_t family=0; while(family<count && !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT)) ++family; assert(family<count);
    float priority=1;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; queueInfo.queueFamilyIndex=family; queueInfo.queueCount=1; queueInfo.pQueuePriorities=&priority;
    VkPhysicalDeviceFeatures features{}; vkGetPhysicalDeviceFeatures(physical,&features);
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; deviceInfo.queueCreateInfoCount=1; deviceInfo.pQueueCreateInfos=&queueInfo; deviceInfo.pEnabledFeatures=&features;
    VkDevice device; CHECK(vkCreateDevice(physical,&deviceInfo,nullptr,&device));
    VkQueue queue; vkGetDeviceQueue(device,family,0,&queue);
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; poolInfo.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; poolInfo.queueFamilyIndex=family;
    VkCommandPool pool; CHECK(vkCreateCommandPool(device,&poolInfo,nullptr,&pool));
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; alloc.commandPool=pool; alloc.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; alloc.commandBufferCount=1;
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(device,&alloc,&cmd));
    auto start=[&] { CHECK(vkResetCommandBuffer(cmd,0)); VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd,&b)); };
    auto submit=[&] { CHECK(vkEndCommandBuffer(cmd)); VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount=1; s.pCommandBuffers=&cmd; CHECK(vkQueueSubmit(queue,1,&s,VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue)); };
    auto barrier=[&](VkImage image,VkImageLayout from,VkImageLayout to) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.oldLayout=from; b.newLayout=to;
        b.srcAccessMask=from==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;
        b.dstAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.image=image; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
    };
    for(int iteration=0;iteration<5;++iteration) {
        const uint32_t w=128+uint32_t(iteration%2)*2,h=96;
        const uint32_t bytes=w*h*4;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=bytes*3; bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer buffer; CHECK(vkCreateBuffer(device,&bi,nullptr,&buffer));
        VkMemoryRequirements mr{}; vkGetBufferMemoryRequirements(device,buffer,&mr);
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ma.allocationSize=mr.size; ma.memoryTypeIndex=memType(mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory host; CHECK(vkAllocateMemory(device,&ma,nullptr,&host)); CHECK(vkBindBufferMemory(device,buffer,host,0));
        uint8_t* pixels=nullptr; CHECK(vkMapMemory(device,host,0,bytes*3,0,reinterpret_cast<void**>(&pixels)));
        std::array<Image,3> images{};
        for(auto& image:images) {
            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.imageType=VK_IMAGE_TYPE_2D; ci.format=VK_FORMAT_R8G8B8A8_UNORM;
            ci.extent={w,h,1}; ci.mipLevels=ci.arrayLayers=1; ci.samples=VK_SAMPLE_COUNT_1_BIT; ci.tiling=VK_IMAGE_TILING_OPTIMAL;
            ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            CHECK(vkCreateImage(device,&ci,nullptr,&image.image)); vkGetImageMemoryRequirements(device,image.image,&mr);
            ma.allocationSize=mr.size; ma.memoryTypeIndex=memType(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            CHECK(vkAllocateMemory(device,&ma,nullptr,&image.memory)); CHECK(vkBindImageMemory(device,image.image,image.memory,0));
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=image.image; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=ci.format; vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            CHECK(vkCreateImageView(device,&vi,nullptr,&image.view));
        }
        VppWinfgEngine engine;
        assert(engine.Init(instance,vkGetInstanceProcAddr,physical,device,family,queue));
        assert(engine.Resize({w,h}));
        for(int mode=0;mode<3;++mode) {
            for(unsigned frame=0;frame<2;++frame) for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
                auto* p=pixels+frame*bytes+(y*w+x)*4;
                const bool block=x>=32+frame*8 && x<64+frame*8 && y>=24 && y<72;
                p[0]=mode?block?uint8_t(130+((x-frame*8)*13+y*7)%100):24:128;
                p[1]=mode?p[0]:96; p[2]=mode?p[0]:64; p[3]=255;
                if (mode==2) p[0]=p[1]=p[2]=frame?240:32;
            }
            start();
            for(unsigned frame=0;frame<2;++frame) {
                barrier(images[frame].image,mode?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkBufferImageCopy copy{}; copy.bufferOffset=frame*bytes; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={w,h,1};
                vkCmdCopyBufferToImage(cmd,buffer,images[frame].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
                barrier(images[frame].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
            }
            barrier(images[2].image,mode?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL); submit();
            assert(engine.Generate(images[0].image,images[0].view,images[1].image,images[1].view,images[2].image,images[2].view));
            CHECK(vkQueueWaitIdle(queue)); assert(engine.Idle());
            start(); barrier(images[2].image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy copy{}; copy.bufferOffset=2*bytes; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={w,h,1};
            vkCmdCopyImageToBuffer(cmd,images[2].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
            barrier(images[2].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL); submit();
            const auto* output=pixels+2*bytes;
            if(!mode) {
                for(unsigned i=0;i<bytes;i+=4) assert(std::abs(int(output[i])-128)<=1 && std::abs(int(output[i+1])-96)<=1 && std::abs(int(output[i+2])-64)<=1 && output[i+3]==255);
                if (iteration==0) {
                    VppWinfgEngine failing;
                    assert(failing.Init(instance,FailureInstanceProc,physical,device,family,queue));
                    assert(failing.Resize({w,h}));
                    assert(!failing.Generate(images[0].image,images[0].view,images[1].image,images[1].view,images[2].image,images[2].view));
                    failing.Destroy();
                    // The first engine must remain usable after a second
                    // instance's partial descriptor allocation and teardown.
                }
            } else if (mode==2) {
                assert(engine.SceneCut());
                for(unsigned i=0;i<bytes;i+=4) assert(output[i]==240 && output[i+1]==240 && output[i+2]==240 && output[i+3]==255);
            } else {
                assert(!engine.SceneCut());
                double total=0,weighted=0;
                for(unsigned y=24;y<72;++y) for(unsigned x=20;x<90;++x) { const double v=std::max(0,int(output[(y*w+x)*4])-24); total+=v; weighted+=x*v; }
                const double centroid=weighted/total;
                printf("motion midpoint centroid=%.3f expected=51.5\n",centroid);
                assert(std::abs(centroid-51.5)<2.0);
                // The generated image must differ from BOTH endpoints.
                unsigned difference[2]{};
                for(unsigned i=0;i<bytes;i+=4) for(unsigned j=0;j<2;++j) difference[j]+=std::abs(int(output[i])-pixels[j*bytes+i]);
                assert(difference[0]>10000 && difference[1]>10000);
            }
        }
        engine.Destroy();
        for(auto& image:images) { vkDestroyImageView(device,image.view,nullptr); vkDestroyImage(device,image.image,nullptr); vkFreeMemory(device,image.memory,nullptr); }
        vkUnmapMemory(device,host); vkDestroyBuffer(device,buffer,nullptr); vkFreeMemory(device,host,nullptr);
    }
    vkDestroyCommandPool(device,pool,nullptr); vkDestroyDevice(device,nullptr); vkDestroyInstance(instance,nullptr);
    puts("PASS Win-FG GPU: encoded color, actual motion midpoint, scene-cut fallback, odd sizes, five lifecycles, descriptor failure and independent instances");
}
