#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>
#include <unistd.h>
#ifndef PROBE_BYTES
#define PROBE_BYTES 65536u
#endif
_Static_assert(PROBE_BYTES >= 4 && PROBE_BYTES <= 67108864 && PROBE_BYTES % 4 == 0, "bounded word-aligned probe size");
__asm__(".symver dlopen,dlopen@GLIBC_2.2.5\n.symver dlsym,dlsym@GLIBC_2.2.5\n.symver dlerror,dlerror@GLIBC_2.2.5");
#define GLOBALS(X) X(vkCreateInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceFeatures2) X(vkCreateDevice) X(vkDestroyInstance) X(vkGetDeviceProcAddr)
#define DEVICES(X) X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkFlushMappedMemoryRanges) X(vkInvalidateMappedMemoryRanges) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) X(vkCmdFillBuffer) X(vkEndCommandBuffer) X(vkCreateFence) X(vkQueueSubmit) X(vkWaitForFences) X(vkDestroyFence) X(vkUnmapMemory) X(vkDestroyBuffer) X(vkFreeMemory) X(vkDestroyCommandPool) X(vkDestroyDevice)
#define DECLARE(n) static PFN_##n n;
GLOBALS(DECLARE) DEVICES(DECLARE)
#define CHECK(call) do { VkResult r=(call);if(r!=VK_SUCCESS){fprintf(stderr,"VK_COHERENCE error line=%d result=%d call=%s\n",__LINE__,r,#call);return 2;} } while(0)
static uint32_t pattern(uint32_t i, uint32_t seed) {return (i*2654435761u)^seed;}
#ifdef PROBE_IMAGES
#include "native_vk_image_probe.h"
#endif
#ifdef PROBE_TEXTURES
#include "native_texture_probe.h"
#endif
#ifdef PROBE_BC
#include "native_bc_probe.h"
#endif
static int run_probe(unsigned requested_type) {
 setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);
 fprintf(stderr,"VK_COHERENCE begin requested_type=%u bytes=%u display_readbacks=0\n",requested_type,(unsigned)PROBE_BYTES);
 void *lib=dlopen("libvulkan.so.1",RTLD_NOW|RTLD_LOCAL);
 if(!lib){fprintf(stderr,"VK_COHERENCE dlopen=%s\n",dlerror());return 2;}
 PFN_vkGetInstanceProcAddr get=(PFN_vkGetInstanceProcAddr)dlsym(lib,"vkGetInstanceProcAddr");
 if(!get)return 2;
 vkCreateInstance=(PFN_vkCreateInstance)get(VK_NULL_HANDLE,"vkCreateInstance");
 VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Coherent buffer contract probe",
#ifdef PROBE_API11
 .apiVersion=VK_API_VERSION_1_1
#else
 .apiVersion=VK_API_VERSION_1_2
#endif
 };
 VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
 VkInstance instance;CHECK(vkCreateInstance(&ii,NULL,&instance));
#define LOAD_INSTANCE(n) n=(PFN_##n)get(instance,#n);if(!n)return 2;
 GLOBALS(LOAD_INSTANCE)
 uint32_t count=1;VkPhysicalDevice physical;CHECK(vkEnumeratePhysicalDevices(instance,&count,&physical));
 VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(physical,&props);
 VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(physical,&memory);
 fprintf(stderr,"VK_COHERENCE renderer=%s types=%u\n",props.deviceName,memory.memoryTypeCount);
 for(unsigned i=0;i<memory.memoryTypeCount;i++)fprintf(stderr,"VK_COHERENCE memory_type=%u flags=%x\n",i,memory.memoryTypes[i].propertyFlags);
 if(requested_type>=memory.memoryTypeCount || !(memory.memoryTypes[requested_type].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
  fprintf(stderr,"VK_COHERENCE skipped=memory_type_not_host_visible type=%u\n",requested_type);
  vkDestroyInstance(instance,NULL); return 3;
 }
 if(props.apiVersion<VK_API_VERSION_1_2) {
  fprintf(stderr,"VK_COHERENCE skipped=physical_api_below_1_2 api=%u\n",props.apiVersion);
  vkDestroyInstance(instance,NULL); return 3;
 }
 VkQueueFamilyProperties families[32];count=32;vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families);
 uint32_t family=0;while(family<count&&!(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))family++;
 if(family==count)return 2;
 float priority=1;VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
 VkPhysicalDeviceBufferDeviceAddressFeatures address={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
 VkPhysicalDeviceTimelineSemaphoreFeatures timeline={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,.pNext=&address};
 VkPhysicalDeviceFeatures2 features={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&timeline};
 vkGetPhysicalDeviceFeatures2(physical,&features);
 fprintf(stderr,"VK_COHERENCE timeline_supported=%u\n",timeline.timelineSemaphore);
 fprintf(stderr,"VK_COHERENCE buffer_device_address=%u etc2=%u astc=%u bc=%u queue_compute=%u\n",
  address.bufferDeviceAddress,features.features.textureCompressionETC2,features.features.textureCompressionASTC_LDR,
  features.features.textureCompressionBC,!!(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT));
 timeline.pNext=NULL; /* This diagnostic does not use or require device addresses. */
#ifdef PROBE_BC
 if(!address.bufferDeviceAddress||!features.features.textureCompressionBC)return 3;
 timeline.pNext=&address;
#endif
 VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&timeline,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
#ifdef PROBE_API11
 di.pNext=NULL; /* Match Unity: no 1.2 core features requested by the app. */
#endif
#ifdef PROBE_TEXTURES
 if(!features.features.textureCompressionETC2||!(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT))return 3;
 VkPhysicalDeviceFeatures enabled={.textureCompressionETC2=VK_TRUE};di.pEnabledFeatures=&enabled;
#endif
#ifdef PROBE_BC
 VkPhysicalDeviceFeatures enabled={.textureCompressionBC=VK_TRUE};di.pEnabledFeatures=&enabled;
#endif
 VkDevice device;CHECK(vkCreateDevice(physical,&di,NULL,&device));
#define LOAD_DEVICE(n) n=(PFN_##n)vkGetDeviceProcAddr(device,#n);if(!n){fprintf(stderr,"VK_COHERENCE missing=%s\n",#n);return 2;}
 DEVICES(LOAD_DEVICE)
 VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
 VkBuffer buffers[2];VkDeviceMemory allocations[2];uint32_t *maps[2];
 const uint32_t words=PROBE_BYTES/4;VkDeviceSize bytes=words*4;
 for(unsigned i=0;i<2;i++) {
  VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
  CHECK(vkCreateBuffer(device,&bi,NULL,&buffers[i]));
  VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,buffers[i],&req);
  uint32_t type=requested_type;
  if(type>=memory.memoryTypeCount||!(req.memoryTypeBits&(1u<<type)))return 2;
  fprintf(stderr,"VK_COHERENCE reported_visible=%u req_bits=%x diagnostic_type=%u\n",!!(memory.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT),req.memoryTypeBits,type);
  VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
  CHECK(vkAllocateMemory(device,&ai,NULL,&allocations[i]));CHECK(vkBindBufferMemory(device,buffers[i],allocations[i],0));
  CHECK(vkMapMemory(device,allocations[i],0,VK_WHOLE_SIZE,0,(void**)&maps[i]));
  fprintf(stderr,"VK_COHERENCE buffer=%u type=%u bytes=%llu\n",i,type,(unsigned long long)req.size);
 }
 VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
 VkCommandPool pool;CHECK(vkCreateCommandPool(device,&pi,NULL,&pool));
 VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=2};
 VkCommandBuffer commands[2];CHECK(vkAllocateCommandBuffers(device,&ca,commands));
 for(unsigned c=0;c<2;c++) {
 VkCommandBuffer command=commands[c];
 VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(vkBeginCommandBuffer(command,&begin));
 VkBufferMemoryBarrier before={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=buffers[0],.offset=0,.size=VK_WHOLE_SIZE};
 vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,1,&before,0,NULL);
 VkBufferCopy copy={.size=bytes};
 if(c==0)vkCmdCopyBuffer(command,buffers[0],buffers[1],1,&copy);
 else vkCmdFillBuffer(command,buffers[1],0,bytes,0xabcdef01);
 VkBufferMemoryBarrier after={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=buffers[1],.offset=0,.size=VK_WHOLE_SIZE};
 vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&after,0,NULL);CHECK(vkEndCommandBuffer(command));
 }
 unsigned failed=0;
 const unsigned coherent=!!(memory.memoryTypes[requested_type].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
 for(unsigned phase=0;phase<4;phase++) {
  if(!coherent && (phase==0 || phase==2)) {
   fprintf(stderr,"VK_COHERENCE phase=%u skipped=noncoherent_requires_explicit_flush\n",phase); continue;
  }
  uint32_t seed=0x12345678u+phase;
  if(phase!=3)for(uint32_t i=0;i<words;i++)maps[0][i]=pattern(i,seed);
  VkMappedMemoryRange src={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=allocations[0],.offset=0,.size=VK_WHOLE_SIZE};
  VkMappedMemoryRange dst={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=allocations[1],.offset=0,.size=VK_WHOLE_SIZE};
  fprintf(stderr,"VK_COHERENCE phase=%u src_before_flush=%08x\n",phase,maps[0][0]);
  if(phase==1)CHECK(vkFlushMappedMemoryRanges(device,1,&src));
  fprintf(stderr,"VK_COHERENCE phase=%u src_after_flush=%08x\n",phase,maps[0][0]);
  VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence;CHECK(vkCreateFence(device,&fi,NULL,&fence));
  VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&commands[phase==3]};CHECK(vkQueueSubmit(queue,1,&submit,fence));
  CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,5000000000ull));
  fprintf(stderr,"VK_COHERENCE phase=%u src_after_wait=%08x\n",phase,maps[0][0]);
  unsigned wrong=0;for(uint32_t i=0;i<words;i++)if(maps[1][i]!=(phase==3?0xabcdef01:pattern(i,seed)))wrong++;
  const uint32_t expected=phase==3?0xabcdef01:seed;
  fprintf(stderr,"VK_COHERENCE phase=%u explicit_flush=%u before_invalidate_wrong=%u first=%08x expected=%08x\n",phase,phase==1,wrong,maps[1][0],expected);
  CHECK(vkInvalidateMappedMemoryRanges(device,1,&dst));
  unsigned afterWrong=0;for(uint32_t i=0;i<words;i++)if(maps[1][i]!=(phase==3?0xabcdef01:pattern(i,seed)))afterWrong++;
  fprintf(stderr,"VK_COHERENCE phase=%u after_invalidate_wrong=%u first=%08x expected=%08x\n",phase,afterWrong,maps[1][0],expected);
  failed+=coherent?(wrong!=0 || afterWrong!=0):afterWrong!=0;vkDestroyFence(device,fence,NULL);
 }
#ifdef PROBE_IMAGES
 if(!failed)failed+=run_image_probe(device,queue,family,requested_type,&memory,&props)!=0;
#endif
#ifdef PROBE_TEXTURES
 if(!failed)failed+=run_texture_probe(physical,device,queue,family,requested_type,&memory,get,instance)!=0;
#endif
#ifdef PROBE_BC
 if(!failed)failed+=run_bc_probe(physical,device,queue,family,requested_type,&memory,get,instance)!=0;
#endif
 vkDestroyCommandPool(device,pool,NULL);
 for(unsigned i=0;i<2;i++){vkUnmapMemory(device,allocations[i]);vkDestroyBuffer(device,buffers[i],NULL);vkFreeMemory(device,allocations[i],NULL);}
 vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);
 fprintf(stderr,"VK_COHERENCE case_done type=%u failed_phases=%u\n",requested_type,failed);return failed?1:0;
}
static void deadline(int n) { (void)n; _Exit(3); }
#ifndef PROBE_MEMORY_TYPE
#define PROBE_MEMORY_TYPE 0
#endif
int main(int argc,char **argv) {
 signal(SIGALRM,deadline); alarm(180);
 unsigned type=PROBE_MEMORY_TYPE;
#ifdef PROBE_STEAM_LAUNCH
 /* Steam may append game launch arguments; diagnostic selection is compiled in. */
 (void)argc; (void)argv;
#else
 if(argc>2)return 2;
 if(argc==2){char *end=NULL;unsigned long value=strtoul(argv[1],&end,10);if(!argv[1][0]||*end||value>=VK_MAX_MEMORY_TYPES)return 2;type=(unsigned)value;}
#endif
 int a=run_probe(type);fprintf(stderr,"VK_COHERENCE complete type=%u result=%d\n",type,a);return a;
}
extern int start_old(int (*)(int,char**,char**),int,char**,void(*)(void),void(*)(void),void(*)(void),void*);
__asm__(".symver start_old,__libc_start_main@GLIBC_2.2.5");
int __wrap___libc_start_main(int(*fn)(int,char**,char**),int n,char**v,void(*init)(void),void(*fini)(void),void(*rf)(void),void*stack){return start_old(fn,n,v,init,fini,rf,stack);}
