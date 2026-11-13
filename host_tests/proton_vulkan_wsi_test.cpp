// Exercise WSI synchronization/error contracts without a GPU or Wine runtime.
#include "../entry/src/main/cpp/linux_steam/gnu_fixture/proton_vulkan_wsi.cpp"
#include <cassert>
#include <atomic>
#include <thread>
namespace {
std::atomic<unsigned> inFlight{0}, concurrent{0}, signals{0};
std::atomic<VkResult> submitResult{VK_SUCCESS};
VkResult fakeSubmit(VkQueue,uint32_t count,const VkSubmitInfo* submits,VkFence) {
    if(inFlight.fetch_add(1))++concurrent;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    for(uint32_t i=0;i<count;i++)if(submits[i].signalSemaphoreCount)++signals;
    --inFlight;return submitResult.load();
}
VkResult fakeExtensions(const char*,uint32_t* count,VkExtensionProperties*) {*count=0;return VK_SUCCESS;}
PFN_vkVoidFunction fakeInstanceGet(VkInstance,const char* name) {
    return !strcmp(name,"vkEnumerateInstanceExtensionProperties")?reinterpret_cast<PFN_vkVoidFunction>(fakeExtensions):nullptr;
}
#ifdef STEAMOHOS_NATIVE_FEX_WSI
unsigned forwardedDraws=0;
unsigned memoryCalls=0,memoryFrees=0,memoryUnmaps=0;
std::vector<VkMappedMemoryRange> forwardedRanges;
VkResult memoryResult=VK_SUCCESS;
VkResult fakeRanges(VkDevice,uint32_t count,const VkMappedMemoryRange* ranges) {
    ++memoryCalls;forwardedRanges.assign(ranges,ranges+count);return memoryResult;
}
void fakeFreeMemory(VkDevice,VkDeviceMemory,const VkAllocationCallbacks*) {++memoryFrees;}
void fakeUnmapMemory(VkDevice,VkDeviceMemory) {++memoryUnmaps;}
VkResult fakeMapMemory(VkDevice,VkDeviceMemory,VkDeviceSize,VkDeviceSize,VkMemoryMapFlags,void** out) {*out=reinterpret_cast<void*>(0x12300);return VK_SUCCESS;}

VkResult pipelineResult=VK_SUCCESS;
VkResult fakeShader(VkDevice,const VkShaderModuleCreateInfo*,const VkAllocationCallbacks*,VkShaderModule* out) {*out=reinterpret_cast<VkShaderModule>(0x1234);return VK_SUCCESS;}
VkResult fakePipelines(VkDevice,VkPipelineCache,uint32_t,const VkGraphicsPipelineCreateInfo*,const VkAllocationCallbacks*,VkPipeline*) {return pipelineResult;}
VkResult fakeCommands(VkDevice,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* out) {for(unsigned i=0;i<info->commandBufferCount;++i)out[i]=reinterpret_cast<VkCommandBuffer>(0x12340+i*16);return VK_SUCCESS;}
void fakeFreeCommands(VkDevice,VkCommandPool,uint32_t,const VkCommandBuffer*) {}
void fakeDestroyPool(VkDevice,VkCommandPool,const VkAllocationCallbacks*) {}
void fakeDraw(VkCommandBuffer,uint32_t vertices,uint32_t instances,uint32_t firstVertex,uint32_t firstInstance) {assert(vertices==3&&instances==2&&firstVertex==1&&firstInstance==7);++forwardedDraws;}
#endif
PFN_vkVoidFunction fakeGet(VkDevice,const char* name) {
#ifdef STEAMOHOS_NATIVE_FEX_WSI
#define MOCK(n,f) if(!strcmp(name,"vk" #n))return reinterpret_cast<PFN_vkVoidFunction>(f)
    MOCK(FlushMappedMemoryRanges,fakeRanges);MOCK(InvalidateMappedMemoryRanges,fakeRanges);
    MOCK(FreeMemory,fakeFreeMemory);MOCK(UnmapMemory,fakeUnmapMemory);MOCK(MapMemory,fakeMapMemory);
    MOCK(CreateShaderModule,fakeShader);MOCK(CreateGraphicsPipelines,fakePipelines);
    MOCK(AllocateCommandBuffers,fakeCommands);MOCK(FreeCommandBuffers,fakeFreeCommands);
    MOCK(DestroyCommandPool,fakeDestroyPool);MOCK(CmdDraw,fakeDraw);
#undef MOCK
#endif
    return !strcmp(name,"vkQueueSubmit")?reinterpret_cast<PFN_vkVoidFunction>(fakeSubmit):nullptr;
}
}
int main() {
    assert(aliasRangeFits(4097,4096,1));
    assert(aliasRangeFits(4103,7,4096));
    assert(!aliasRangeFits(4097,4096,2));
    assert(!aliasRangeFits(4097,4098,1));
    assert(!aliasRangeFits(UINT64_MAX,UINT64_MAX-1,3));
    assert(!aliasRangeFits(4096,0,0));
    get=fakeInstanceGet;
    present=[](VkQueue,VkImage,uint32_t,uint32_t,VkFormat,VkImageLayout,uint32_t,uint32_t,uint32_t,uint64_t*){return 0;};
    uint32_t extensionCount=0;
    assert(vkEnumerateInstanceExtensionProperties(nullptr,&extensionCount,nullptr)==VK_SUCCESS);
    std::vector<VkExtensionProperties> extensionList(extensionCount);
    assert(vkEnumerateInstanceExtensionProperties(nullptr,&extensionCount,extensionList.data())==VK_SUCCESS);
    auto has=[&](const char* name){return std::any_of(extensionList.begin(),extensionList.end(),[&](const auto& v){return !strcmp(v.extensionName,name);});};
    assert(has("VK_KHR_surface")&&has("VK_KHR_wayland_surface"));
#ifdef STEAMOHOS_NATIVE_FEX_WSI
    assert(has("VK_KHR_xlib_surface")&&has("VK_KHR_xcb_surface"));
    assert(intercept("vkCreateXlibSurfaceKHR")&&intercept("vkCreateXcbSurfaceKHR"));
    VkXlibSurfaceCreateInfoKHR xlib{};VkSurfaceKHR rejected=VK_NULL_HANDLE;
    assert(vkCreateXlibSurfaceKHR(VK_NULL_HANDLE,&xlib,nullptr,&rejected)==VK_ERROR_INITIALIZATION_FAILED);
    VkXcbSurfaceCreateInfoKHR xcb{};
    assert(vkCreateXcbSurfaceKHR(VK_NULL_HANDLE,&xcb,nullptr,&rejected)==VK_ERROR_INITIALIZATION_FAILED);
#else
    assert(!has("VK_KHR_xlib_surface")&&!has("VK_KHR_xcb_surface"));
#endif

    VkDevice device=reinterpret_cast<VkDevice>(0x10000);
    VkQueue queue=reinterpret_cast<VkQueue>(0x20000);
    VkSemaphore semaphore=reinterpret_cast<VkSemaphore>(0x30000);
    devices[device]={VK_NULL_HANDLE,VK_NULL_HANDLE,queue,fakeGet};queues[queue]=device;
#ifdef STEAMOHOS_NATIVE_FEX_WSI
    // A freed native VkDeviceMemory in a later flush must not reach Venus.
    // Preserve good entries, report bad ones, and retain real driver errors.
    VkDeviceMemory live=reinterpret_cast<VkDeviceMemory>(0x70000);
    VkDeviceMemory freed=reinterpret_cast<VkDeviceMemory>(0x80000);
    mappings[live]={4096,nullptr,device,0,0,false};
    mappings[freed]={4096,nullptr,device,0,0,false};
    void* mapped=nullptr;
    assert(vkMapMemory(device,live,64,1024,0,&mapped)==VK_SUCCESS&&mapped);
    assert(vkMapMemory(device,live,0,VK_WHOLE_SIZE,0,&mapped)==VK_ERROR_MEMORY_MAP_FAILED);
    assert(vkMapMemory(device,freed,0,VK_WHOLE_SIZE,0,&mapped)==VK_SUCCESS);
    VkMappedMemoryRange ranges[2]={{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,nullptr,live,64,256},
                                 {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,nullptr,freed,0,VK_WHOLE_SIZE}};
    assert(vkFlushMappedMemoryRanges(device,2,ranges)==VK_SUCCESS&&forwardedRanges.size()==2);
    vkFreeMemory(device,freed,nullptr);assert(memoryFrees==1&&!mappings.count(freed));
    assert(vkFlushMappedMemoryRanges(device,2,ranges)==VK_ERROR_MEMORY_MAP_FAILED);
    assert(forwardedRanges.size()==1&&forwardedRanges[0].memory==live);
    unsigned before=memoryCalls;
    assert(vkFlushMappedMemoryRanges(device,1,ranges+1)==VK_ERROR_MEMORY_MAP_FAILED&&memoryCalls==before);
    assert(vkInvalidateMappedMemoryRanges(device,2,ranges)==VK_ERROR_MEMORY_MAP_FAILED&&forwardedRanges.size()==1);
    auto wrong=ranges[0];wrong.offset=1024;wrong.size=128;
    assert(vkFlushMappedMemoryRanges(device,1,&wrong)==VK_ERROR_MEMORY_MAP_FAILED);
    wrong=ranges[0];wrong.size=VK_WHOLE_SIZE;
    assert(vkFlushMappedMemoryRanges(device,1,&wrong)==VK_ERROR_MEMORY_MAP_FAILED);
    wrong=ranges[0];wrong.offset=UINT64_MAX-8;wrong.size=64;
    assert(vkFlushMappedMemoryRanges(device,1,&wrong)==VK_ERROR_MEMORY_MAP_FAILED);
    auto foreign=reinterpret_cast<VkDevice>(0x10100);devices[foreign]=devices[device];
    assert(vkFlushMappedMemoryRanges(foreign,1,ranges)==VK_ERROR_MEMORY_MAP_FAILED);
    devices.erase(foreign);
    memoryResult=VK_ERROR_DEVICE_LOST;
    assert(vkFlushMappedMemoryRanges(device,2,ranges)==VK_ERROR_DEVICE_LOST);
    memoryResult=VK_SUCCESS;
    vkUnmapMemory(device,live);assert(memoryUnmaps==1);
    assert(vkFlushMappedMemoryRanges(device,1,ranges)==VK_ERROR_MEMORY_MAP_FAILED);
    vkFreeMemory(device,live,nullptr);mappings.clear();
    assert(intercept("vkFlushMappedMemoryRanges")&&intercept("vkInvalidateMappedMemoryRanges"));
    // The diagnostic must retain real results and exact draw parameters.
    assert(!nativeGpuIntercept("vkCmdDraw"));
    VkShaderModule shader{};assert(nativeCreateShaderModule(device,nullptr,nullptr,&shader)==VK_SUCCESS&&shader);
    assert(nativeShaders==1);
    assert(nativeCreateGraphicsPipelines(device,VK_NULL_HANDLE,2,nullptr,nullptr,nullptr)==VK_SUCCESS&&nativePipelines==2);
    pipelineResult=VK_ERROR_DEVICE_LOST;
    assert(nativeCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,nullptr,nullptr,nullptr)==VK_ERROR_DEVICE_LOST);
    assert(nativePipelines==2&&nativePipelineErrors==1);
    VkCommandBufferAllocateInfo allocation{};allocation.commandBufferCount=2;
    allocation.commandPool=reinterpret_cast<VkCommandPool>(0x9898);VkCommandBuffer commands[2];
    assert(nativeAllocateCommandBuffers(device,&allocation,commands)==VK_SUCCESS&&nativeCommands.size()==2);
    nativeCmdDraw(commands[0],3,2,1,7);assert(nativeDraws==1&&forwardedDraws==1);
    nativeFreeCommandBuffers(device,allocation.commandPool,1,commands);assert(nativeCommands.size()==1);
    nativeDestroyCommandPool(device,allocation.commandPool,nullptr);assert(nativeCommands.empty());
#endif
    auto swap=std::make_shared<Swapchain>();swap->device=device;
    swap->images={reinterpret_cast<VkImage>(0x40000),reinterpret_cast<VkImage>(0x50000)};
    swap->acquired={true,true};VkSwapchainKHR handle=reinterpret_cast<VkSwapchainKHR>(swap.get());swaps[handle]=swap;
    uint32_t index=UINT32_MAX;
    assert(vkAcquireNextImageKHR(device,handle,0,semaphore,VK_NULL_HANDLE,&index)==VK_NOT_READY);
    assert(vkAcquireNextImageKHR(device,handle,1000000,semaphore,VK_NULL_HANDLE,&index)==VK_TIMEOUT);
    assert(signals==0&&index==UINT32_MAX);
    swap->retired=true;
    assert(vkAcquireNextImageKHR(device,handle,UINT64_MAX,semaphore,VK_NULL_HANDLE,&index)==VK_ERROR_OUT_OF_DATE_KHR);
    swap->retired=false;
    std::thread releaseThread([&]{std::this_thread::sleep_for(std::chrono::milliseconds(4));std::lock_guard<std::mutex> lock(swap->mutex);swap->acquired[1]=false;swap->available.notify_one();});
    assert(vkAcquireNextImageKHR(device,handle,1000000000,semaphore,VK_NULL_HANDLE,&index)==VK_SUCCESS);
    releaseThread.join();assert(index==1&&signals==1);
    uint32_t count=0;assert(vkGetSwapchainImagesKHR(device,handle,&count,nullptr)==VK_SUCCESS&&count==2);
    VkImage image{};count=1;assert(vkGetSwapchainImagesKHR(device,handle,&count,&image)==VK_INCOMPLETE&&image==swap->images[0]);
    swap->acquired[0]=false;submitResult=VK_ERROR_DEVICE_LOST;
    assert(vkAcquireNextImageKHR(device,handle,0,semaphore,VK_NULL_HANDLE,&index)==VK_ERROR_DEVICE_LOST&&!swap->acquired[0]);
    submitResult=VK_SUCCESS;
    std::thread renderThread([&]{VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;for(unsigned i=0;i<50;i++)assert(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)==VK_SUCCESS);});
    for(unsigned i=0;i<50;i++) {
        assert(vkAcquireNextImageKHR(device,handle,0,semaphore,VK_NULL_HANDLE,&index)==VK_SUCCESS);
        std::lock_guard<std::mutex> lock(swap->mutex);swap->acquired[index]=false;
    }
    renderThread.join();assert(concurrent==0);
    swaps.clear();devices.clear();queues.clear();queueLocks.clear();
    puts("PASS: acquire timeout, retirement, wakeup, enumeration, submit failure and queue exclusion");
}
