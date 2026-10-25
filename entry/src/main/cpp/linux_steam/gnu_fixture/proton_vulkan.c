/* GNU Venus capability/queue test. Uses the original OHOS Vulkan receiver;
 * does not load an OHOS driver into glibc or imply a working DXVK swapchain. */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
struct window {
    struct wl_surface *surface;
    struct xdg_surface *xdg;
    struct xdg_toplevel *toplevel;
    struct wl_buffer *placeholder;
    int width, height;
    bool configured, closed;
    unsigned frames;
};
static void ping(void *data,struct xdg_wm_base *base,uint32_t serial) { (void)data;xdg_wm_base_pong(base,serial); }
static const struct xdg_wm_base_listener wm_listener={.ping=ping};
static void global(void *data,struct wl_registry *registry,uint32_t name,const char *iface,uint32_t version) {
    (void)data;
    if(!strcmp(iface,"wl_compositor")) compositor=wl_registry_bind(registry,name,&wl_compositor_interface,version<4?version:4);
    else if(!strcmp(iface,"wl_shm")) shm=wl_registry_bind(registry,name,&wl_shm_interface,1);
    else if(!strcmp(iface,"xdg_wm_base")) {wm=wl_registry_bind(registry,name,&xdg_wm_base_interface,1);xdg_wm_base_add_listener(wm,&wm_listener,NULL);}
}
static void removed(void *data,struct wl_registry *r,uint32_t name) {(void)data;(void)r;(void)name;}
static const struct wl_registry_listener registry_listener={global,removed};
static void configured(void *data,struct xdg_surface *surface,uint32_t serial) {
    struct window *w=data;xdg_surface_ack_configure(surface,serial);w->configured=true;
}
static const struct xdg_surface_listener surface_listener={configured};
static void resized(void *data,struct xdg_toplevel *top,int32_t width,int32_t height,struct wl_array *states) {
    (void)data;(void)top;(void)width;(void)height;(void)states;
}
static void closed(void *data,struct xdg_toplevel *top) { (void)top;((struct window*)data)->closed=true; }
static const struct xdg_toplevel_listener top_listener={.configure=resized,.close=closed};
static bool placeholder(struct window *w,int width,int height) {
    char path[4096];const char *tmp=getenv("TMPDIR");
    if(!tmp||snprintf(path,sizeof(path),"%s/gnu-present-XXXXXX",tmp)>=(int)sizeof(path))return false;
    int fd=mkstemp(path);if(fd<0)return false;unlink(path);
    const int bytes=width*height*4;
    if(ftruncate(fd,bytes)){close(fd);return false;}
    struct wl_shm_pool *pool=wl_shm_create_pool(shm,fd,bytes);
    close(fd);if(!pool)return false;
    struct wl_buffer *buffer=wl_shm_pool_create_buffer(pool,0,width,height,width*4,WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);if(!buffer)return false;
    wl_surface_attach(w->surface,buffer,0,0);wl_surface_damage(w->surface,0,0,width,height);
    xdg_surface_set_window_geometry(w->xdg,0,0,width,height);wl_surface_commit(w->surface);
    if(w->placeholder)wl_buffer_destroy(w->placeholder);
    w->placeholder=buffer;w->width=width;w->height=height;
    return wl_display_roundtrip(display)>=0;
}
static bool create_window(struct window *w,const char *title,int width,int height) {
    w->surface=wl_compositor_create_surface(compositor);if(!w->surface)return false;
    w->xdg=xdg_wm_base_get_xdg_surface(wm,w->surface);if(!w->xdg)return false;
    xdg_surface_add_listener(w->xdg,&surface_listener,w);
    w->toplevel=xdg_surface_get_toplevel(w->xdg);if(!w->toplevel)return false;
    xdg_toplevel_add_listener(w->toplevel,&top_listener,w);xdg_toplevel_set_title(w->toplevel,title);
    xdg_toplevel_set_app_id(w->toplevel,"hmosdeck.gnu.shared-present");wl_surface_commit(w->surface);
    if(wl_display_roundtrip(display)<0||!w->configured)return false;
    return placeholder(w,width,height);
}
static bool receiver_ready(struct window *w) {
    char path[256];uint64_t key=((uint64_t)(uint32_t)getpid()<<32)|wl_proxy_get_id((struct wl_proxy*)w->surface);
    snprintf(path,sizeof(path),"/data/storage/el2/base/cache/winehua_zc_surface_%"PRIu64".ready",key);
    return access(path,F_OK)==0;
}


#define LOAD(name) PFN_##name name=(PFN_##name)get(instance,#name); if(!name){printf("GNU_VULKAN missing=%s\n",#name);return 3;}
#define CHECK(expr) do { VkResult result=(expr); if(result!=VK_SUCCESS){printf("GNU_VULKAN failure=%s result=%d\n",#expr,result);return 4;} } while(0)
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    const char *root=getenv("STEAMOHOS_CODE_ROOT");
    if(!root||strcmp(root,"/data/storage/el2/base/files/steamohos-native/proton-probe"))return 2;
    setenv("VN_DEBUG","vtest",1);
    /* Match the proven OHOS Wine Venus synchronization contract. The host
     * does not expose the shared feedback pages used by upstream defaults. */
    setenv("VN_PERF","no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring",1);
    setenv("VN_WINEHUA_STRONG_RING_BARRIER","1",1);
    setenv("VN_WINEHUA_REMOTE_MEMORY_SYNC","1",1);
    setenv("VN_WINEHUA_PERSISTENT_MAP_SYNC","1",1);
    setenv("VN_WINEHUA_DIRECT_FENCE_WAIT","1",1);
    char path[4096];snprintf(path,sizeof(path),"%s/graphics/libvulkan_virtio.so",root);
    void *library=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if(!library){printf("GNU_VULKAN load_failed=%s\n",dlerror());return 2;}
    PFN_vkGetInstanceProcAddr get=(PFN_vkGetInstanceProcAddr)dlsym(library,"vk_icdGetInstanceProcAddr");
    if(!get)return 3;
    VkInstance instance=VK_NULL_HANDLE;
    LOAD(vkCreateInstance);
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="GNU Proton Vulkan probe",.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo create={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    CHECK(vkCreateInstance(&create,NULL,&instance));
    LOAD(vkEnumeratePhysicalDevices);LOAD(vkGetPhysicalDeviceProperties);
    LOAD(vkGetPhysicalDeviceQueueFamilyProperties);LOAD(vkCreateDevice);
    LOAD(vkGetDeviceQueue);LOAD(vkDeviceWaitIdle);LOAD(vkDestroyDevice);LOAD(vkDestroyInstance);
    uint32_t count=0;CHECK(vkEnumeratePhysicalDevices(instance,&count,NULL));
    if(!count||count>16){printf("GNU_VULKAN devices=%u\n",count);return 5;}
    VkPhysicalDevice devices[16];CHECK(vkEnumeratePhysicalDevices(instance,&count,devices));
    VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(devices[0],&properties);
    printf("GNU_VULKAN_DEVICE name=%s vendor=%x type=%u api=%u.%u.%u maxTexture=%u\n",properties.deviceName,properties.vendorID,properties.deviceType,
        VK_VERSION_MAJOR(properties.apiVersion),VK_VERSION_MINOR(properties.apiVersion),VK_VERSION_PATCH(properties.apiVersion),properties.limits.maxImageDimension2D);
    if(properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU)return 6;
    uint32_t families=0;vkGetPhysicalDeviceQueueFamilyProperties(devices[0],&families,NULL);
    if(!families||families>64)return 7;
    VkQueueFamilyProperties queues[64];vkGetPhysicalDeviceQueueFamilyProperties(devices[0],&families,queues);
    uint32_t family=0;while(family<families&&!(queues[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))family++;
    if(family==families)return 8;
    float priority=1;
    VkDeviceQueueCreateInfo queue={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&queue};
    VkDevice device;CHECK(vkCreateDevice(devices[0],&dc,NULL,&device));
    VkQueue q;vkGetDeviceQueue(device,family,0,&q);if(!q)return 9;
    CHECK(vkDeviceWaitIdle(device));

    puts("GNU_VULKAN_QUEUE_OK");
    char render_path[4096];snprintf(render_path,sizeof(render_path),"%s/vulkan-render",root);
    if(access(render_path,F_OK)==0) {
        typedef int (*Present)(VkQueue,VkImage,uint32_t,uint32_t,VkFormat,VkImageLayout,uint32_t,uint32_t,uint32_t,uint64_t*);
        Present present=(Present)dlsym(library,"vn_winehua_present");if(!present)return 10;
        LOAD(vkCreateImage);LOAD(vkGetImageMemoryRequirements);LOAD(vkGetPhysicalDeviceMemoryProperties);
        LOAD(vkAllocateMemory);LOAD(vkBindImageMemory);LOAD(vkDestroyImage);LOAD(vkFreeMemory);
        LOAD(vkCreateCommandPool);LOAD(vkAllocateCommandBuffers);LOAD(vkResetCommandBuffer);
        LOAD(vkBeginCommandBuffer);LOAD(vkEndCommandBuffer);LOAD(vkCmdPipelineBarrier);LOAD(vkCmdClearColorImage);
        LOAD(vkQueueSubmit);LOAD(vkCreateFence);LOAD(vkWaitForFences);LOAD(vkResetFences);
        LOAD(vkDestroyFence);LOAD(vkDestroyCommandPool);
        display=wl_display_connect(NULL);if(!display)return 11;
        struct wl_registry *registry=wl_display_get_registry(display);wl_registry_add_listener(registry,&registry_listener,NULL);
        if(wl_display_roundtrip(display)<0||!compositor||!shm||!wm)return 11;
        struct window window={0};if(!create_window(&window,"GNU Vulkan NativeImage",640,400))return 12;
        uint32_t surface_id=wl_proxy_get_id((struct wl_proxy*)window.surface);
        printf("GNU_VULKAN_BEGIN pid=%u surface=%u\n",(unsigned)getpid(),surface_id);
        VkCommandPoolCreateInfo pool_info={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};
        VkCommandPool pool;CHECK(vkCreateCommandPool(device,&pool_info,NULL,&pool));
        VkCommandBufferAllocateInfo cb_info={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
        VkCommandBuffer cb;CHECK(vkAllocateCommandBuffers(device,&cb_info,&cb));
        VkFenceCreateInfo fence_info={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence;CHECK(vkCreateFence(device,&fence_info,NULL,&fence));
        VkImage image=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;
        VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkImageLayout old_layout=VK_IMAGE_LAYOUT_UNDEFINED;
        unsigned presents=0, geometry=1;
        for(unsigned frame=0;frame<360;frame++) {
            if(wl_display_roundtrip(display)<0||window.closed)return 13;
            if(frame==0||frame==180) {
                if(frame) {
                    CHECK(vkDeviceWaitIdle(device));vkDestroyImage(device,image,NULL);vkFreeMemory(device,memory,NULL);
                    if(!placeholder(&window,480,300))return 14;
                    ++geometry;puts("GNU_VULKAN_RESIZE width=480 height=300");
                }
                VkImageCreateInfo image_info={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
                    .format=VK_FORMAT_B8G8R8A8_UNORM,.extent={(uint32_t)window.width,(uint32_t)window.height,1},
                    .mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
                    .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
                CHECK(vkCreateImage(device,&image_info,NULL,&image));
                VkMemoryRequirements requirements;vkGetImageMemoryRequirements(device,image,&requirements);
                VkPhysicalDeviceMemoryProperties memories;vkGetPhysicalDeviceMemoryProperties(devices[0],&memories);
                uint32_t type=0;
                while(type<memories.memoryTypeCount&&(!(requirements.memoryTypeBits&(1u<<type))||
                    !(memories.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))++type;
                if(type==memories.memoryTypeCount)return 15;
                VkMemoryAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                    .allocationSize=requirements.size,.memoryTypeIndex=type};
                CHECK(vkAllocateMemory(device,&alloc,NULL,&memory));CHECK(vkBindImageMemory(device,image,memory,0));
                old_layout=VK_IMAGE_LAYOUT_UNDEFINED;
            }
            CHECK(vkResetCommandBuffer(cb,0));
            VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            CHECK(vkBeginCommandBuffer(cb,&begin));
            VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask=old_layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_READ_BIT,
                .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=old_layout,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange=range};
            vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
            VkClearColorValue color={.float32={0.1f+(frame%120)/150.f,0.2f,0.75f-(frame%120)/200.f,1.0f}};
            vkCmdClearColorImage(cb,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&range);
            barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT;
            barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
            vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&barrier);
            CHECK(vkEndCommandBuffer(cb));
            VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb};
            CHECK(vkQueueSubmit(q,1,&submit,fence));
            CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,5000000000ull));CHECK(vkResetFences(device,1,&fence));
            uint64_t deadline=0;
            int rc=present(q,image,window.width,window.height,VK_FORMAT_B8G8R8A8_UNORM,VK_IMAGE_LAYOUT_GENERAL,
                (uint32_t)getpid(),surface_id,frame+1,&deadline);
            if(rc!=0&&rc!=1){printf("GNU_VULKAN_PRESENT_FAILED frame=%u result=%d\n",frame,rc);return 16;}
            old_layout=VK_IMAGE_LAYOUT_GENERAL;++presents;
            wl_surface_commit(window.surface);wl_display_flush(display);
            if(frame%120==60)printf("GNU_VULKAN_PROGRESS frame=%u ready=%d\n",frame,receiver_ready(&window));
            struct timespec delay={0,16000000};nanosleep(&delay,NULL);
        }
        CHECK(vkDeviceWaitIdle(device));
        const bool ready=receiver_ready(&window);
        printf("GNU_VULKAN_RESULT frames=%u receiver_ready=%d pixel_readbacks=0 per_frame_shm_uploads=0 geometry_buffers=%u\n",presents,ready,geometry);
        vkDestroyImage(device,image,NULL);vkFreeMemory(device,memory,NULL);
        vkDestroyFence(device,fence,NULL);vkDestroyCommandPool(device,pool,NULL);
        wl_buffer_destroy(window.placeholder);xdg_toplevel_destroy(window.toplevel);
        xdg_surface_destroy(window.xdg);wl_surface_destroy(window.surface);wl_display_roundtrip(display);
        wl_registry_destroy(registry);xdg_wm_base_destroy(wm);wl_shm_destroy(shm);wl_compositor_destroy(compositor);wl_display_disconnect(display);
        if(!ready)return 17;
        puts("GNU_VULKAN_PRESENT_OK");
    }
    vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);
    puts("GNU_VULKAN_QUEUE_OK");dlclose(library);return 0;
}
