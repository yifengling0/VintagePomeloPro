// Opt-in GNU Wine WSI adapter for the existing OHOS Venus presenter.
// Rendering stays in the original GNU Venus ICD. Only window/swapchain
// ownership is implemented here, following OHOS Wine's private WSI contract.
// No host pointer crosses the libc boundary; no rendered pixels enter SHM.
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <dirent.h>
#include <cerrno>
#include <cinttypes>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace {
std::recursive_mutex stateMutex;
void* icd;
PFN_vkGetInstanceProcAddr get;
using Present = int(*)(VkQueue,VkImage,uint32_t,uint32_t,VkFormat,VkImageLayout,uint32_t,uint32_t,uint32_t,uint64_t*);
Present present;
struct Device { VkInstance instance; VkPhysicalDevice physical; VkQueue queue; PFN_vkGetDeviceProcAddr get; };
std::map<VkPhysicalDevice,VkInstance> physicals;
std::map<VkDevice,Device> devices;
std::map<VkQueue,VkDevice> queues;
std::map<VkQueue,std::shared_ptr<std::recursive_mutex>> queueLocks;
std::shared_ptr<std::recursive_mutex> queueLock(VkQueue queue) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    auto& value=queueLocks[queue];if(!value)value=std::make_shared<std::recursive_mutex>();return value;
}
struct Mapping { VkDeviceSize bytes=0; void* alias=nullptr; };
std::map<VkDeviceMemory,Mapping> mappings;
using AllocateVirtual = int32_t(*)(void*,void**,uintptr_t,size_t*,uint32_t,uint32_t);
using FreeVirtual = int32_t(*)(void*,void**,size_t*,uint32_t);
FreeVirtual freeVirtual;
bool alias32(VkDeviceMemory memory,VkDeviceSize memoryOffset,size_t bytes,void** base,void** out) {
    const char* root=getenv("STEAMOHOS_CODE_ROOT");if(!root||!bytes)return false;
    char path[4096];snprintf(path,sizeof(path),"%s/package/files/lib/wine/aarch64-unix/ntdll.so",root);
    void* ntdll=dlopen(path,RTLD_NOW|RTLD_NOLOAD);if(!ntdll){fprintf(stderr,"GNU_WINE_VULKAN alias_ntdll_failed %s\n",dlerror());return false;}
    auto alloc=reinterpret_cast<AllocateVirtual>(dlsym(ntdll,"NtAllocateVirtualMemory"));
    freeVirtual=reinterpret_cast<FreeVirtual>(dlsym(ntdll,"NtFreeVirtualMemory"));dlclose(ntdll);
    if(!alloc||!freeVirtual){fprintf(stderr,"GNU_WINE_VULKAN alias_nt_exports_missing\n");return false;}
    const size_t page=static_cast<size_t>(sysconf(_SC_PAGESIZE));
    const size_t delta=memoryOffset&(page-1);
    if(bytes>SIZE_MAX-delta-page)return false;
    size_t length=(bytes+delta+page-1)&~(page-1);
    using MappedFd = int(*)(VkDeviceMemory,uint64_t*);
    auto mappedFd=reinterpret_cast<MappedFd>(dlsym(icd,"vn_winehua_mapped_memory_fd"));
    uint64_t backingSize=0;int fd=mappedFd?mappedFd(memory,&backingSize):-1;
    if(fd<0){fprintf(stderr,"GNU_WINE_VULKAN alias_fd_unavailable\n");return false;}
    const uint64_t fileOffset=memoryOffset-delta;
    if(fileOffset>backingSize||length>backingSize-fileOffset){close(fd);return false;}
    void* placed=nullptr;size_t reserved=length;
    const int32_t status=alloc(reinterpret_cast<void*>(~uintptr_t(0)),&placed,0x7fffffff,&reserved,0x3000,0x04);
    if(status||!placed){close(fd);fprintf(stderr,"GNU_WINE_VULKAN alias_reserve_failed status=%x bytes=%zu\n",status,length);return false;}
    void* mapped=mmap(placed,length,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,fileOffset);close(fd);
    if(mapped!=placed) {fprintf(stderr,"GNU_WINE_VULKAN alias_mmap_failed errno=%d\n",errno);size_t zero=0;freeVirtual(reinterpret_cast<void*>(~uintptr_t(0)),&placed,&zero,0x8000);return false;}
    *base=placed;*out=static_cast<char*>(placed)+delta;
    fprintf(stderr,"GNU_WINE_VULKAN shared_alias bytes=%zu pixel_copies=0 low_address=1\n",length);return true;
}
void releaseAlias(Mapping& mapping) {
    if(mapping.alias&&freeVirtual){size_t zero=0;freeVirtual(reinterpret_cast<void*>(~uintptr_t(0)),&mapping.alias,&zero,0x8000);mapping.alias=nullptr;}
}
struct Surface {
    wl_display* display=nullptr;wl_surface* window=nullptr;wl_registry* registry=nullptr;
    wl_event_queue* queue=nullptr;wl_shm* shm=nullptr;wl_buffer* blank=nullptr;
    ~Surface() {
        if(blank)wl_buffer_destroy(blank);if(shm)wl_shm_destroy(shm);
        if(registry)wl_registry_destroy(registry);if(queue)wl_event_queue_destroy(queue);
    }
};
std::map<VkSurfaceKHR,std::shared_ptr<Surface>> surfaces;
struct Swapchain {
    VkDevice device;std::shared_ptr<Surface> surface;VkFormat format;VkExtent2D extent;
    std::vector<VkImage> images;std::vector<VkDeviceMemory> memories;std::vector<bool> acquired;
    std::mutex mutex;std::condition_variable available;bool retired=false;uint32_t serial=0;
};
std::map<VkSwapchainKHR,std::shared_ptr<Swapchain>> swaps;
void startupTrace(const char* stage, const char* detail="") {
    static const bool enabled=access("/data/storage/el2/base/files/steamohos-native/proton-probe/vulkan-init-debug",R_OK)==0;
    static std::atomic<unsigned> count{0};
    if(enabled && count.fetch_add(1)<64)
        fprintf(stderr,"GNU_VULKAN_STARTUP pid=%d stage=%s detail=%s\n",getpid(),stage,detail);
}
bool init() {
    startupTrace("init_enter");
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if(get)return true;
    const char* root=getenv("STEAMOHOS_CODE_ROOT");
    if(!root||strcmp(root,"/data/storage/el2/base/files/steamohos-native/proton-probe"))return false;
    char path[4096];snprintf(path,sizeof(path),"%s/graphics/libvulkan_virtio.so",root);
    startupTrace("icd_open");
    icd=dlopen(path,RTLD_NOW|RTLD_LOCAL);startupTrace("icd_opened",icd?"ok":"failed");if(!icd)return false;
    get=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(icd,"vk_icdGetInstanceProcAddr"));
    present=reinterpret_cast<Present>(dlsym(icd,"vn_winehua_present"));
    startupTrace("icd_ready",get&&present?"ok":"missing");
    return get&&present;
}
template<class T> T instanceProc(VkInstance instance,const char* name) {return reinterpret_cast<T>(get(instance,name));}
template<class T> T deviceProc(VkDevice device,const char* name) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    auto it=devices.find(device);return it==devices.end()?nullptr:reinterpret_cast<T>(it->second.get(device,name));
}
VkInstance physicalInstance(VkPhysicalDevice physical) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);auto it=physicals.find(physical);return it==physicals.end()?VK_NULL_HANDLE:it->second;
}
template<class K,class V> std::shared_ptr<V> lookup(std::map<K,std::shared_ptr<V>>& map,K key) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);auto it=map.find(key);return it==map.end()?nullptr:it->second;
}
template<class T> VkResult enumerate(const std::vector<T>& values,uint32_t* count,T* out) {
    if(!out){*count=values.size();return VK_SUCCESS;}
    uint32_t n=std::min(*count,static_cast<uint32_t>(values.size()));std::copy_n(values.begin(),n,out);*count=n;
    return n==values.size()?VK_SUCCESS:VK_INCOMPLETE;
}
bool surfaceExtension(const char* s) {return !strcmp(s,VK_KHR_SURFACE_EXTENSION_NAME)||!strcmp(s,VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME)||!strcmp(s,VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);}
void global(void* data,wl_registry* registry,uint32_t id,const char* name,uint32_t) {
    auto s=static_cast<Surface*>(data);if(!strcmp(name,"wl_shm"))s->shm=static_cast<wl_shm*>(wl_registry_bind(registry,id,&wl_shm_interface,1));
}
void removed(void*,wl_registry*,uint32_t){}
const wl_registry_listener registryListener={global,removed};
bool geometry(Surface& s,VkExtent2D size) {
    if(!s.queue) {
        s.queue=wl_display_create_queue(s.display);if(!s.queue)return false;
        auto wrapper=static_cast<wl_display*>(wl_proxy_create_wrapper(s.display));if(!wrapper)return false;
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper),s.queue);
        s.registry=wl_display_get_registry(wrapper);wl_proxy_wrapper_destroy(wrapper);if(!s.registry)return false;
        wl_registry_add_listener(s.registry,&registryListener,&s);
        if(wl_display_roundtrip_queue(s.display,s.queue)<0||!s.shm)return false;
    }
    const uint64_t bytes=uint64_t(size.width)*size.height*4;
    if(!size.width||!size.height||bytes>128*1024*1024)return false;
    char path[4096];const char* tmp=getenv("TMPDIR");
    if(!tmp||snprintf(path,sizeof(path),"%s/vulkan-geometry-XXXXXX",tmp)>=(int)sizeof(path))return false;
    int fd=mkstemp(path);if(fd<0)return false;unlink(path);
    if(ftruncate(fd,bytes)){close(fd);return false;}
    auto pool=wl_shm_create_pool(s.shm,fd,bytes);close(fd);if(!pool)return false;
    auto blank=wl_shm_pool_create_buffer(pool,0,size.width,size.height,size.width*4,WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);if(!blank)return false;
    wl_surface_attach(s.window,blank,0,0);wl_surface_damage(s.window,0,0,size.width,size.height);
    wl_surface_commit(s.window);wl_display_flush(s.display);
    if(s.blank)wl_buffer_destroy(s.blank);s.blank=blank;return true;
}
void release(Swapchain& swap) {
    auto destroy=deviceProc<PFN_vkDestroyImage>(swap.device,"vkDestroyImage");
    auto freeMemory=deviceProc<PFN_vkFreeMemory>(swap.device,"vkFreeMemory");
    for(auto image:swap.images)if(image)destroy(swap.device,image,nullptr);
    for(auto memory:swap.memories)if(memory)freeMemory(swap.device,memory,nullptr);
}
}
extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t* version) {
    if(!init())return VK_ERROR_INITIALIZATION_FAILED;
    auto f=instanceProc<PFN_vkEnumerateInstanceVersion>(VK_NULL_HANDLE,"vkEnumerateInstanceVersion");
    if(f)return f(version);*version=VK_API_VERSION_1_0;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char* layer,uint32_t* count,VkExtensionProperties* out) {
    if(!init())return VK_ERROR_INITIALIZATION_FAILED;
    auto f=instanceProc<PFN_vkEnumerateInstanceExtensionProperties>(VK_NULL_HANDLE,"vkEnumerateInstanceExtensionProperties");
    if(layer)return f(layer,count,out);
    uint32_t n=0;VkResult rc=f(nullptr,&n,nullptr);if(rc!=VK_SUCCESS)return rc;
    std::vector<VkExtensionProperties> values(n);rc=f(nullptr,&n,values.data());if(rc!=VK_SUCCESS)return rc;values.resize(n);
    for(const char* name:{VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME})
        if(std::none_of(values.begin(),values.end(),[&](auto& p){return !strcmp(p.extensionName,name);})) {
            VkExtensionProperties p{};strcpy(p.extensionName,name);p.specVersion=!strcmp(name,VK_KHR_SURFACE_EXTENSION_NAME)?25:!strcmp(name,VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME)?6:1;values.push_back(p);
        }
    return enumerate(values,count,out);
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* count,VkLayerProperties* out) {
    return enumerate(std::vector<VkLayerProperties>{},count,out);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* info,const VkAllocationCallbacks* alloc,VkInstance* instance) {
    if(!init())return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<const char*> extensions;
    for(uint32_t i=0;i<info->enabledExtensionCount;i++)if(!surfaceExtension(info->ppEnabledExtensionNames[i]))extensions.push_back(info->ppEnabledExtensionNames[i]);
    auto copy=*info;copy.enabledExtensionCount=extensions.size();copy.ppEnabledExtensionNames=extensions.data();
    startupTrace("create_instance");
    return instanceProc<PFN_vkCreateInstance>(VK_NULL_HANDLE,"vkCreateInstance")(&copy,alloc,instance);
}
VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance,const VkAllocationCallbacks* alloc) {
    {std::lock_guard<std::recursive_mutex> lock(stateMutex);
    for(auto it=physicals.begin();it!=physicals.end();)if(it->second==instance)it=physicals.erase(it);else ++it;}
    instanceProc<PFN_vkDestroyInstance>(instance,"vkDestroyInstance")(instance,alloc);
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance,uint32_t* count,VkPhysicalDevice* out) {
    startupTrace("enum_physical");
    auto rc=instanceProc<PFN_vkEnumeratePhysicalDevices>(instance,"vkEnumeratePhysicalDevices")(instance,count,out);
    if(out&&(rc==VK_SUCCESS||rc==VK_INCOMPLETE)) {std::lock_guard<std::recursive_mutex> lock(stateMutex);for(uint32_t i=0;i<*count;i++)physicals[out[i]]=instance;}
    return rc;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physical,const char* layer,uint32_t* count,VkExtensionProperties* out) {
    auto f=instanceProc<PFN_vkEnumerateDeviceExtensionProperties>(physicalInstance(physical),"vkEnumerateDeviceExtensionProperties");
    if(!f)return VK_ERROR_INITIALIZATION_FAILED;if(layer)return f(physical,layer,count,out);
    uint32_t n=0;auto rc=f(physical,nullptr,&n,nullptr);if(rc!=VK_SUCCESS)return rc;
    std::vector<VkExtensionProperties> values(n);rc=f(physical,nullptr,&n,values.data());if(rc!=VK_SUCCESS)return rc;values.resize(n);
    if(std::none_of(values.begin(),values.end(),[](auto& p){return !strcmp(p.extensionName,VK_KHR_SWAPCHAIN_EXTENSION_NAME);})) {
        VkExtensionProperties p{};strcpy(p.extensionName,VK_KHR_SWAPCHAIN_EXTENSION_NAME);p.specVersion=70;values.push_back(p);
    }
    return enumerate(values,count,out);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo* info,const VkAllocationCallbacks* alloc,VkDevice* device) {
    auto instance=physicalInstance(physical);if(!instance)return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<const char*> extensions;
    for(uint32_t i=0;i<info->enabledExtensionCount;i++)if(strcmp(info->ppEnabledExtensionNames[i],VK_KHR_SWAPCHAIN_EXTENSION_NAME))extensions.push_back(info->ppEnabledExtensionNames[i]);
    auto copy=*info;copy.enabledExtensionCount=extensions.size();copy.ppEnabledExtensionNames=extensions.data();
    auto rc=instanceProc<PFN_vkCreateDevice>(instance,"vkCreateDevice")(physical,&copy,alloc,device);if(rc!=VK_SUCCESS)return rc;
    Device d{instance,physical,VK_NULL_HANDLE,instanceProc<PFN_vkGetDeviceProcAddr>(instance,"vkGetDeviceProcAddr")};
    // Acquire signals are submitted on the first graphics queue requested by the application.
    uint32_t n=0;auto families=instanceProc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(instance,"vkGetPhysicalDeviceQueueFamilyProperties");
    families(physical,&n,nullptr);std::vector<VkQueueFamilyProperties> props(n);families(physical,&n,props.data());
    for(uint32_t i=0;i<info->queueCreateInfoCount;i++) {
        const auto& q=info->pQueueCreateInfos[i];if(q.queueFamilyIndex<n&&(props[q.queueFamilyIndex].queueFlags&VK_QUEUE_GRAPHICS_BIT)&&!q.flags) {
            reinterpret_cast<PFN_vkGetDeviceQueue>(d.get(*device,"vkGetDeviceQueue"))(*device,q.queueFamilyIndex,0,&d.queue);break;
        }
    }
    if(!d.queue){reinterpret_cast<PFN_vkDestroyDevice>(d.get(*device,"vkDestroyDevice"))(*device,alloc);return VK_ERROR_FEATURE_NOT_PRESENT;}
    std::lock_guard<std::recursive_mutex> lock(stateMutex);devices[*device]=d;queues[d.queue]=*device;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device,const VkAllocationCallbacks* alloc) {
    auto destroy=deviceProc<PFN_vkDestroyDevice>(device,"vkDestroyDevice");
    {std::lock_guard<std::recursive_mutex> lock(stateMutex);devices.erase(device);for(auto it=queues.begin();it!=queues.end();)if(it->second==device){queueLocks.erase(it->first);it=queues.erase(it);}else ++it;}
    if(destroy)destroy(device,alloc);
}
VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device,uint32_t family,uint32_t index,VkQueue* queue) {
    deviceProc<PFN_vkGetDeviceQueue>(device,"vkGetDeviceQueue")(device,family,index,queue);
    std::lock_guard<std::recursive_mutex> lock(stateMutex);queues[*queue]=device;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateWaylandSurfaceKHR(VkInstance,const VkWaylandSurfaceCreateInfoKHR* info,const VkAllocationCallbacks*,VkSurfaceKHR* handle) {
    if(!info->display||!info->surface||info->flags)return VK_ERROR_INITIALIZATION_FAILED;
    auto s=std::make_shared<Surface>();s->display=info->display;s->window=info->surface;
    *handle=reinterpret_cast<VkSurfaceKHR>(s.get());std::lock_guard<std::recursive_mutex> lock(stateMutex);surfaces[*handle]=s;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(VkInstance,VkSurfaceKHR surface,const VkAllocationCallbacks*) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);surfaces.erase(surface);
}
VKAPI_ATTR VkBool32 VKAPI_CALL vkGetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice physical,uint32_t family,wl_display*) {
    auto f=instanceProc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(physicalInstance(physical),"vkGetPhysicalDeviceQueueFamilyProperties");
    uint32_t n=0;f(physical,&n,nullptr);std::vector<VkQueueFamilyProperties> props(n);f(physical,&n,props.data());
    return family<n&&(props[family].queueFlags&VK_QUEUE_GRAPHICS_BIT)?VK_TRUE:VK_FALSE;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physical,uint32_t family,VkSurfaceKHR surface,VkBool32* out) {
    if(!lookup(surfaces,surface))return VK_ERROR_SURFACE_LOST_KHR;
    *out=vkGetPhysicalDeviceWaylandPresentationSupportKHR(physical,family,nullptr);return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physical,VkSurfaceKHR surface,VkSurfaceCapabilitiesKHR* caps) {
    if(!lookup(surfaces,surface))return VK_ERROR_SURFACE_LOST_KHR;
    VkPhysicalDeviceProperties p;instanceProc<PFN_vkGetPhysicalDeviceProperties>(physicalInstance(physical),"vkGetPhysicalDeviceProperties")(physical,&p);
    *caps={};caps->minImageCount=2;caps->maxImageCount=3;caps->currentExtent={UINT32_MAX,UINT32_MAX};caps->minImageExtent={1,1};
    const uint32_t limit=std::min(p.limits.maxImageDimension2D,4096u); // geometry allocation cap, validated below
    caps->maxImageExtent={limit,limit};caps->maxImageArrayLayers=1;
    caps->supportedTransforms=caps->currentTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->supportedCompositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    caps->supportedUsageFlags=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physical,VkSurfaceKHR surface,uint32_t* count,VkSurfaceFormatKHR* out) {
    if(!lookup(surfaces,surface))return VK_ERROR_SURFACE_LOST_KHR;
    std::vector<VkSurfaceFormatKHR> values;auto f=instanceProc<PFN_vkGetPhysicalDeviceFormatProperties>(physicalInstance(physical),"vkGetPhysicalDeviceFormatProperties");
    for(auto format:{VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_B8G8R8A8_SRGB,VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_SRGB}) {
        VkFormatProperties props;f(physical,format,&props);if(props.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)values.push_back({format,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR});
    }
    return enumerate(values,count,out);
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice,VkSurfaceKHR surface,uint32_t* count,VkPresentModeKHR* out) {
    if(!lookup(surfaces,surface))return VK_ERROR_SURFACE_LOST_KHR;
    return enumerate(std::vector<VkPresentModeKHR>{VK_PRESENT_MODE_FIFO_KHR},count,out);
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physical,const VkPhysicalDeviceSurfaceInfo2KHR* info,VkSurfaceCapabilities2KHR* caps) {
    return vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,info->surface,&caps->surfaceCapabilities);
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice physical,const VkPhysicalDeviceSurfaceInfo2KHR* info,uint32_t* count,VkSurfaceFormat2KHR* out) {
    if(!out)return vkGetPhysicalDeviceSurfaceFormatsKHR(physical,info->surface,count,nullptr);
    std::vector<VkSurfaceFormatKHR> values(*count);auto result=vkGetPhysicalDeviceSurfaceFormatsKHR(physical,info->surface,count,values.data());
    if(result==VK_SUCCESS||result==VK_INCOMPLETE)for(uint32_t i=0;i<*count;i++)out[i].surfaceFormat=values[i];return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device,const VkSwapchainCreateInfoKHR* info,const VkAllocationCallbacks*,VkSwapchainKHR* handle) {
    auto surface=lookup(surfaces,info->surface);if(!surface)return VK_ERROR_SURFACE_LOST_KHR;
    if(info->flags||info->imageArrayLayers!=1||info->imageSharingMode!=VK_SHARING_MODE_EXCLUSIVE||info->minImageCount>3||info->presentMode!=VK_PRESENT_MODE_FIFO_KHR||info->pNext)return VK_ERROR_FEATURE_NOT_PRESENT;
    auto swap=std::make_shared<Swapchain>();swap->device=device;swap->surface=surface;swap->format=info->imageFormat;swap->extent=info->imageExtent;
    if(!geometry(*surface,swap->extent))return VK_ERROR_INITIALIZATION_FAILED;
    Device d;{std::lock_guard<std::recursive_mutex> lock(stateMutex);d=devices.at(device);}
    VkPhysicalDeviceMemoryProperties props;instanceProc<PFN_vkGetPhysicalDeviceMemoryProperties>(d.instance,"vkGetPhysicalDeviceMemoryProperties")(d.physical,&props);
    const uint32_t count=std::max(2u,info->minImageCount);swap->images.resize(count);swap->memories.resize(count);swap->acquired.resize(count,false);
    VkImageCreateInfo imageInfo{};imageInfo.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;imageInfo.imageType=VK_IMAGE_TYPE_2D;imageInfo.format=info->imageFormat;
    imageInfo.extent={info->imageExtent.width,info->imageExtent.height,1};imageInfo.mipLevels=1;imageInfo.arrayLayers=1;imageInfo.samples=VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling=VK_IMAGE_TILING_OPTIMAL;imageInfo.usage=info->imageUsage|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;imageInfo.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    for(uint32_t i=0;i<count;i++) {
        auto rc=deviceProc<PFN_vkCreateImage>(device,"vkCreateImage")(device,&imageInfo,nullptr,&swap->images[i]);if(rc!=VK_SUCCESS){release(*swap);return rc;}
        VkMemoryRequirements req;deviceProc<PFN_vkGetImageMemoryRequirements>(device,"vkGetImageMemoryRequirements")(device,swap->images[i],&req);
        uint32_t type=0;while(type<props.memoryTypeCount&&(!(req.memoryTypeBits&(1u<<type))||!(props.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))++type;
        if(type==props.memoryTypeCount){release(*swap);return VK_ERROR_FEATURE_NOT_PRESENT;}
        VkMemoryAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ai.allocationSize=req.size;ai.memoryTypeIndex=type;
        rc=deviceProc<PFN_vkAllocateMemory>(device,"vkAllocateMemory")(device,&ai,nullptr,&swap->memories[i]);if(rc!=VK_SUCCESS){release(*swap);return rc;}
        rc=deviceProc<PFN_vkBindImageMemory>(device,"vkBindImageMemory")(device,swap->images[i],swap->memories[i],0);if(rc!=VK_SUCCESS){release(*swap);return rc;}
    }
    *handle=reinterpret_cast<VkSwapchainKHR>(swap.get());
    {std::lock_guard<std::recursive_mutex> lock(stateMutex);swaps[*handle]=swap;}
    if(auto old=lookup(swaps,info->oldSwapchain)){std::lock_guard<std::mutex> lock(old->mutex);old->retired=true;old->available.notify_all();}
    fprintf(stderr,"GNU_WINE_VULKAN create pid=%d surface=%u size=%ux%u images=%u\n",getpid(),wl_proxy_get_id(reinterpret_cast<wl_proxy*>(surface->window)),swap->extent.width,swap->extent.height,count);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice,VkSwapchainKHR handle,const VkAllocationCallbacks*) {
    auto swap=lookup(swaps,handle);if(!swap)return;
    {std::lock_guard<std::mutex> lock(swap->mutex);swap->retired=true;swap->available.notify_all();}
    release(*swap);std::lock_guard<std::recursive_mutex> lock(stateMutex);swaps.erase(handle);
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice,VkSwapchainKHR handle,uint32_t* count,VkImage* out) {
    auto swap=lookup(swaps,handle);return swap?enumerate(swap->images,count,out):VK_ERROR_OUT_OF_DATE_KHR;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice device,VkSwapchainKHR handle,uint64_t timeout,VkSemaphore sem,VkFence fence,uint32_t* index) {
    auto swap=lookup(swaps,handle);if(!swap)return VK_ERROR_OUT_OF_DATE_KHR;
    std::unique_lock<std::mutex> lock(swap->mutex);
    auto ready=[&]{return swap->retired||std::find(swap->acquired.begin(),swap->acquired.end(),false)!=swap->acquired.end();};
    if(!ready()) {
        if(!timeout)return VK_NOT_READY;
        if(timeout==UINT64_MAX)swap->available.wait(lock,ready);
        else if(!swap->available.wait_for(lock,std::chrono::nanoseconds(std::min(timeout,uint64_t(INT64_MAX))),ready))return VK_TIMEOUT;
    }
    if(swap->retired)return VK_ERROR_OUT_OF_DATE_KHR;
    *index=std::find(swap->acquired.begin(),swap->acquired.end(),false)-swap->acquired.begin();
    VkQueue queue;{std::lock_guard<std::recursive_mutex> guard(stateMutex);queue=devices.at(device).queue;}
    VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.signalSemaphoreCount=sem?1:0;submit.pSignalSemaphores=sem?&sem:nullptr;
    auto guard=queueLock(queue);std::lock_guard<std::recursive_mutex> queueGuard(*guard);
    auto rc=deviceProc<PFN_vkQueueSubmit>(device,"vkQueueSubmit")(queue,1,&submit,fence);if(rc==VK_SUCCESS)swap->acquired[*index]=true;return rc;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImage2KHR(VkDevice device,const VkAcquireNextImageInfoKHR* info,uint32_t* index) {
    if(info->deviceMask!=1)return VK_ERROR_FEATURE_NOT_PRESENT;
    return vkAcquireNextImageKHR(device,info->swapchain,info->timeout,info->semaphore,info->fence,index);
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue,const VkPresentInfoKHR* info) {
    VkDevice device;{std::lock_guard<std::recursive_mutex> lock(stateMutex);auto it=queues.find(queue);if(it==queues.end())return VK_ERROR_INITIALIZATION_FAILED;device=it->second;}
    std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.waitSemaphoreCount=info->waitSemaphoreCount;submit.pWaitSemaphores=info->pWaitSemaphores;submit.pWaitDstStageMask=stages.data();
    VkResult result=VK_SUCCESS;
    if(info->waitSemaphoreCount) {
        auto guard=queueLock(queue);std::lock_guard<std::recursive_mutex> lock(*guard);
        result=deviceProc<PFN_vkQueueSubmit>(device,"vkQueueSubmit")(queue,1,&submit,VK_NULL_HANDLE);
    }
    if(result!=VK_SUCCESS)return result;
    for(uint32_t i=0;i<info->swapchainCount;i++) {
        auto swap=lookup(swaps,info->pSwapchains[i]);VkResult rc=VK_ERROR_OUT_OF_DATE_KHR;
        if(swap&&info->pImageIndices[i]<swap->images.size()) {
            std::lock_guard<std::mutex> lock(swap->mutex);uint32_t index=info->pImageIndices[i];
            if(!swap->acquired[index])rc=VK_ERROR_INITIALIZATION_FAILED;
            else {
                uint64_t deadline=0;const uint32_t surface=wl_proxy_get_id(reinterpret_cast<wl_proxy*>(swap->surface->window));
                auto guard=queueLock(queue);std::lock_guard<std::recursive_mutex> queueGuard(*guard);
                int status=present(queue,swap->images[index],swap->extent.width,swap->extent.height,swap->format,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,getpid(),surface,++swap->serial,&deadline);
                rc=(status==0||status==1)?VK_SUCCESS:VK_ERROR_DEVICE_LOST;
                if(rc!=VK_SUCCESS)fprintf(stderr,"GNU_WINE_VULKAN present_failed=%d serial=%u\n",status,swap->serial);
                wl_surface_commit(swap->surface->window);wl_display_flush(swap->surface->display);
                swap->acquired[index]=false;swap->available.notify_one();
                if(swap->serial%120==0)fprintf(stderr,"GNU_WINE_VULKAN frames=%u surface=%u pixel_readbacks=0\n",swap->serial,surface);
            }
        }
        if(info->pResults)info->pResults[i]=rc;if(result==VK_SUCCESS)result=rc;
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device,const VkMemoryAllocateInfo* info,const VkAllocationCallbacks* alloc,VkDeviceMemory* memory) {
    auto result=deviceProc<PFN_vkAllocateMemory>(device,"vkAllocateMemory")(device,info,alloc,memory);
    if(result==VK_SUCCESS){std::lock_guard<std::recursive_mutex> lock(stateMutex);mappings[*memory]={info->allocationSize,nullptr};}return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice device,VkDeviceMemory memory,VkDeviceSize offset,VkDeviceSize size,VkMemoryMapFlags flags,void** out) {
    auto map=deviceProc<PFN_vkMapMemory>(device,"vkMapMemory");auto rc=map(device,memory,offset,size,flags,out);
    if(rc!=VK_SUCCESS||!getenv("STEAMOHOS_PROTON_VULKAN_MAP32")||reinterpret_cast<uintptr_t>(*out)<=UINT32_MAX)return rc;
    std::lock_guard<std::recursive_mutex> lock(stateMutex);auto it=mappings.find(memory);
    const VkDeviceSize bytes=it==mappings.end()||offset>it->second.bytes?0:size==VK_WHOLE_SIZE?it->second.bytes-offset:size;
    void* alias=nullptr;
    if(bytes&&bytes<=SIZE_MAX&&alias32(memory,offset,bytes,&alias,out)) {it->second.alias=alias;return VK_SUCCESS;}
    deviceProc<PFN_vkUnmapMemory>(device,"vkUnmapMemory")(device,memory);*out=nullptr;
    fprintf(stderr,"GNU_WINE_VULKAN shared_alias_failed bytes=%" PRIu64 " errno=%d\n",bytes,errno);return VK_ERROR_MEMORY_MAP_FAILED;
}
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory2KHR(VkDevice device,const VkMemoryMapInfoKHR* info,void** out) {
    if(info->pNext)return VK_ERROR_FEATURE_NOT_PRESENT;return vkMapMemory(device,info->memory,info->offset,info->size,info->flags,out);
}
VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice device,VkDeviceMemory memory) {
    deviceProc<PFN_vkUnmapMemory>(device,"vkUnmapMemory")(device,memory);
    std::lock_guard<std::recursive_mutex> lock(stateMutex);auto it=mappings.find(memory);if(it!=mappings.end())releaseAlias(it->second);
}
VKAPI_ATTR VkResult VKAPI_CALL vkUnmapMemory2KHR(VkDevice device,const VkMemoryUnmapInfoKHR* info) {
    if(info->pNext||info->flags)return VK_ERROR_FEATURE_NOT_PRESENT;vkUnmapMemory(device,info->memory);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device,VkDeviceMemory memory,const VkAllocationCallbacks* alloc) {
    {std::lock_guard<std::recursive_mutex> lock(stateMutex);auto it=mappings.find(memory);if(it!=mappings.end()){releaseAlias(it->second);mappings.erase(it);}}
    deviceProc<PFN_vkFreeMemory>(device,"vkFreeMemory")(device,memory,alloc);
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue,uint32_t count,const VkSubmitInfo* info,VkFence fence) {
    VkDevice d;{std::lock_guard<std::recursive_mutex> lock(stateMutex);auto it=queues.find(queue);if(it==queues.end())return VK_ERROR_INITIALIZATION_FAILED;d=it->second;}
    auto guard=queueLock(queue);std::lock_guard<std::recursive_mutex> lock(*guard);
    return deviceProc<PFN_vkQueueSubmit>(d,"vkQueueSubmit")(queue,count,info,fence);
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue queue,uint32_t count,const VkSubmitInfo2* info,VkFence fence) {
    VkDevice d;{std::lock_guard<std::recursive_mutex> lock(stateMutex);auto it=queues.find(queue);if(it==queues.end())return VK_ERROR_INITIALIZATION_FAILED;d=it->second;}
    auto guard=queueLock(queue);std::lock_guard<std::recursive_mutex> lock(*guard);
    auto f=deviceProc<PFN_vkQueueSubmit2>(d,"vkQueueSubmit2");return f?f(queue,count,info,fence):VK_ERROR_EXTENSION_NOT_PRESENT;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue) {
    VkDevice d;{std::lock_guard<std::recursive_mutex> lock(stateMutex);auto it=queues.find(queue);if(it==queues.end())return VK_ERROR_INITIALIZATION_FAILED;d=it->second;}
    auto guard=queueLock(queue);std::lock_guard<std::recursive_mutex> lock(*guard);
    return deviceProc<PFN_vkQueueWaitIdle>(d,"vkQueueWaitIdle")(queue);
}
static PFN_vkVoidFunction intercept(const char* name) {
#define PROC(n) if(!strcmp(name,#n))return reinterpret_cast<PFN_vkVoidFunction>(n)
    PROC(vkAllocateMemory);PROC(vkMapMemory);PROC(vkMapMemory2KHR);PROC(vkUnmapMemory);PROC(vkUnmapMemory2KHR);PROC(vkFreeMemory);
    PROC(vkQueueSubmit);PROC(vkQueueSubmit2);PROC(vkQueueWaitIdle);
    if(!strcmp(name,"vkQueueSubmit2KHR"))return reinterpret_cast<PFN_vkVoidFunction>(vkQueueSubmit2);
    PROC(vkEnumerateInstanceVersion);PROC(vkEnumerateInstanceExtensionProperties);PROC(vkEnumerateInstanceLayerProperties);
    PROC(vkCreateInstance);PROC(vkDestroyInstance);PROC(vkEnumeratePhysicalDevices);
    PROC(vkEnumerateDeviceExtensionProperties);PROC(vkCreateDevice);PROC(vkDestroyDevice);PROC(vkGetDeviceQueue);
    PROC(vkCreateWaylandSurfaceKHR);PROC(vkDestroySurfaceKHR);PROC(vkGetPhysicalDeviceWaylandPresentationSupportKHR);
    PROC(vkGetPhysicalDeviceSurfaceSupportKHR);PROC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);PROC(vkGetPhysicalDeviceSurfaceFormatsKHR);PROC(vkGetPhysicalDeviceSurfacePresentModesKHR);
    PROC(vkGetPhysicalDeviceSurfaceCapabilities2KHR);PROC(vkGetPhysicalDeviceSurfaceFormats2KHR);
    PROC(vkCreateSwapchainKHR);PROC(vkDestroySwapchainKHR);PROC(vkGetSwapchainImagesKHR);PROC(vkAcquireNextImageKHR);PROC(vkAcquireNextImage2KHR);PROC(vkQueuePresentKHR);
#undef PROC
    return nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,const char* name) {
    if(!name)return nullptr;if(auto p=intercept(name))return p;return deviceProc<PFN_vkVoidFunction>(device,name);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,const char* name) {
    startupTrace("get_proc",name?name:"null");
    if(!name||!init())return nullptr;
    if(!strcmp(name,"vkGetInstanceProcAddr"))return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
    if(!strcmp(name,"vkGetDeviceProcAddr"))return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    if(auto p=intercept(name))return p;return get(instance,name);
}
}
