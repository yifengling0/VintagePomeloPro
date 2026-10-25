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
PFN_vkVoidFunction fakeGet(VkDevice,const char* name) {
    return !strcmp(name,"vkQueueSubmit")?reinterpret_cast<PFN_vkVoidFunction>(fakeSubmit):nullptr;
}
}
int main() {
    VkDevice device=reinterpret_cast<VkDevice>(0x10000);
    VkQueue queue=reinterpret_cast<VkQueue>(0x20000);
    VkSemaphore semaphore=reinterpret_cast<VkSemaphore>(0x30000);
    devices[device]={VK_NULL_HANDLE,VK_NULL_HANDLE,queue,fakeGet};queues[queue]=device;
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
