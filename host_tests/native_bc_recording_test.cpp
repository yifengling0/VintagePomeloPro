// Exercise production BC recording without a GPU. Vulkan calls remain stubs;
// replay order, retained layouts, allocation churn and cross-recorder locking
// are checked independently of rendering performance.
#include <atomic>
#include <cstdlib>
#include <new>
namespace {
thread_local bool countAllocations=false;
thread_local size_t allocations=0;
}
__attribute__((noinline)) void* operator new(size_t n) {
    if(countAllocations)++allocations;
    if(void* p=std::malloc(n?n:1))return p;
    throw std::bad_alloc();
}
__attribute__((noinline)) void* operator new[](size_t n) {return ::operator new(n);}
__attribute__((noinline)) void operator delete(void* p) noexcept {std::free(p);}
__attribute__((noinline)) void operator delete[](void* p) noexcept {std::free(p);}
__attribute__((noinline)) void operator delete(void* p,size_t) noexcept {std::free(p);}
__attribute__((noinline)) void operator delete[](void* p,size_t) noexcept {std::free(p);}

#include "../entry/src/main/cpp/linux_steam/gnu_fixture/native_vulkan_wsi.cpp"
#include <array>
#include <cassert>
#include <thread>

namespace {
std::atomic<unsigned> procLookups{0},pushCalls{0},bindCalls{0},destroyCalls{0};
uintptr_t nextCommand=0x100000,nextLayout=0x200000;
bool checkUnlocked=false,checkParallel=false;
std::mutex rendezvous;
std::condition_variable arrived;
unsigned recorders=0;
void verifyUnlocked() {
    if(!checkUnlocked)return;
    bool unlocked=false;
    std::thread probe([&] {unlocked=stateMutex.try_lock();if(unlocked)stateMutex.unlock();});
    probe.join();assert(unlocked);
}
void fakeBind(VkCommandBuffer,VkPipelineBindPoint,VkPipeline) {
    ++bindCalls;verifyUnlocked();
    if(checkParallel) {
        std::unique_lock<std::mutex> lock(rendezvous);
        ++recorders;arrived.notify_all();
        assert(arrived.wait_for(lock,std::chrono::seconds(2),[]{return recorders==2;}));
    }
}
void fakePush(VkCommandBuffer,VkPipelineLayout,VkShaderStageFlags,uint32_t,uint32_t,const void*) {
    ++pushCalls;verifyUnlocked();
}
VkResult fakeAllocate(VkDevice,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* out) {
    for(unsigned i=0;i<info->commandBufferCount;i++){out[i]=reinterpret_cast<VkCommandBuffer>(nextCommand);nextCommand+=16;}
    return VK_SUCCESS;
}
void fakeFree(VkDevice,VkCommandPool,uint32_t,const VkCommandBuffer*) {}
VkResult fakeLayout(VkDevice,const VkPipelineLayoutCreateInfo*,const VkAllocationCallbacks*,VkPipelineLayout* out) {
    *out=reinterpret_cast<VkPipelineLayout>(nextLayout);nextLayout+=16;return VK_SUCCESS;
}
void fakeDestroy(VkDevice,VkPipelineLayout,const VkAllocationCallbacks*) {++destroyCalls;}
PFN_vkVoidFunction fakeGet(VkDevice,const char* name) {
    ++procLookups;
#define MOCK(n,f) if(!strcmp(name,"vk" #n))return reinterpret_cast<PFN_vkVoidFunction>(f)
    MOCK(CmdBindPipeline,fakeBind);MOCK(CmdPushConstants,fakePush);
    MOCK(AllocateCommandBuffers,fakeAllocate);MOCK(FreeCommandBuffers,fakeFree);
    MOCK(CreatePipelineLayout,fakeLayout);MOCK(DestroyPipelineLayout,fakeDestroy);
#undef MOCK
    return nullptr;
}
using StageBytes=std::array<std::array<uint8_t,32>,2>;
void apply(StageBytes& state,VkShaderStageFlags stages,unsigned offset,unsigned size,const void* bytes) {
    const auto* p=static_cast<const uint8_t*>(bytes);
    for(unsigned stage=0;stage<2;stage++)if(stages&(stage?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT))
        std::copy_n(p,size,state[stage].begin()+offset);
}
}
int main() {
    const auto device=reinterpret_cast<VkDevice>(0x10000);
    const auto physical=reinterpret_cast<VkPhysicalDevice>(0x20000);
    const auto pool=reinterpret_cast<VkCommandPool>(0x30000);
    devices[device]={VK_NULL_HANDLE,physical,VK_NULL_HANDLE,fakeGet};
    nativebc::supported[physical]=true;
    VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    info.commandPool=pool;info.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;info.commandBufferCount=2;
    VkCommandBuffer commands[2];
    assert(nativebc::allocateCommands(device,&info,commands)==VK_SUCCESS);
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,32};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.pushConstantRangeCount=1;layoutInfo.pPushConstantRanges=&range;
    VkPipelineLayout layout;
    assert(nativebc::createLayout(device,&layoutInfo,nullptr,&layout)==VK_SUCCESS);
    std::weak_ptr<nativebc::ReplayLayout> retained=nativebc::replayLayouts.at(layout);
    StageBytes reference{};
    const uint8_t a[16]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    const uint8_t b[16]={17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32};
    const uint8_t c[4]={71,72,73,74};
    auto update=[&](VkShaderStageFlags stages,unsigned offset,unsigned size,const void* data) {
        apply(reference,stages,offset,size,data);
        nativebc::push(commands[0],layout,stages,offset,size,data);
    };
    update(VK_SHADER_STAGE_VERTEX_BIT,0,16,a);
    update(VK_SHADER_STAGE_FRAGMENT_BIT,0,16,b);
    update(VK_SHADER_STAGE_VERTEX_BIT,4,4,c);
    update(VK_SHADER_STAGE_VERTEX_BIT,0,16,b);
    update(VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,8,4,c);
    update(VK_SHADER_STAGE_FRAGMENT_BIT,0,16,a);
    StageBytes replay{};
    for(const auto& p:nativebc::commands.at(commands[0]).pushes)
        apply(replay,p.stages,p.offset,unsigned(p.bytes.size()),p.bytes.data());
    assert(replay==reference);
    assert(nativebc::commands.at(commands[0]).pushes.size()==4);

    // Alternating exact keys also exercises rotation rather than just a hit
    // on the last entry. After warmup, forwarding needs no proc lookup or heap.
    const unsigned lookups=procLookups;
    allocations=0;countAllocations=true;
    for(unsigned i=0;i<10000;i++) {
        nativebc::push(commands[0],layout,(i&1)?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT,0,16,(i&1)?a:b);
        nativebc::bindPipeline(commands[0],VK_PIPELINE_BIND_POINT_GRAPHICS,VK_NULL_HANDLE);
    }
    countAllocations=false;
    assert(allocations==0&&procLookups==lookups);
    checkUnlocked=true;
    nativebc::push(commands[0],layout,VK_SHADER_STAGE_VERTEX_BIT,0,16,a);
    nativebc::bindPipeline(commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,VK_NULL_HANDLE);
    checkUnlocked=false;
    checkParallel=true;
    std::thread first([&]{nativebc::bindPipeline(commands[0],VK_PIPELINE_BIND_POINT_GRAPHICS,VK_NULL_HANDLE);});
    std::thread second([&]{nativebc::bindPipeline(commands[1],VK_PIPELINE_BIND_POINT_GRAPHICS,VK_NULL_HANDLE);});
    first.join();second.join();checkParallel=false;
    assert(recorders==2);

    nativebc::destroyLayout(device,layout,nullptr);
    assert(!retained.expired()&&destroyCalls==1);
    nativebc::clear(nativebc::commands.at(commands[0]));
    assert(retained.expired()&&destroyCalls==2);
    // A new command buffer has its own cached capability and entry points;
    // it must still reject a missing replay layout, retaining the first error.
    unsigned calls=pushCalls;
    nativebc::push(commands[1],layout,VK_SHADER_STAGE_VERTEX_BIT,0,16,a);
    assert(pushCalls==calls&&nativebc::commands.at(commands[1]).error==VK_ERROR_VALIDATION_FAILED_EXT);
    nativebc::freeCommands(device,pool,2,commands);
    assert(nativebc::commands.empty()&&nativeCommands.empty());
    devices.clear();nativebc::supported.clear();
    puts("PASS BC recording: overlapping/stage replay, layout lifetime, 10000 allocation-free updates, cached dispatch and parallel forwarding");
}
