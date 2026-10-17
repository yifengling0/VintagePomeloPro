#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define getpid() GetCurrentProcessId()
#define usleep(us) Sleep((us)/1000)
#define sleep(s) Sleep((s)*1000)
#else
#include <unistd.h>
#endif

typedef void (*log_fn)(const char *, ...);
static VkDevice device;
static VkPhysicalDeviceMemoryProperties props;
static log_fn report;
static unsigned vp_probe_rounds = 8;
static unsigned vp_probe_continue_failures;
static VkDeviceSize vp_probe_bytes=96u*1024u*1024u;
static unsigned vp_probe_submit2;
static unsigned vp_probe_dedicated;
static unsigned vp_probe_map_before_bind;
static unsigned vp_probe_failed;
struct buffer { VkBuffer handle; VkDeviceMemory memory; void *map; };
#define TRY(expr) do { VkResult result=(expr); if(result) { report("NATIVE-ISOLATION FAIL %s result=%d",#expr,result); goto cleanup; } } while(0)
static VkResult create_buffer(struct buffer *buffer, VkDeviceSize bytes, unsigned map_now) {
  VkBufferCreateInfo bci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
  VkResult r=vkCreateBuffer(device,&bci,NULL,&buffer->handle); if(r)return r;
  VkMemoryRequirements req; vkGetBufferMemoryRequirements(device,buffer->handle,&req);
  unsigned type=props.memoryTypeCount;
  for(unsigned i=0;i<props.memoryTypeCount;i++)if((req.memoryTypeBits&(1u<<i))&&(props.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)){type=i;break;}
  if(type==props.memoryTypeCount)return VK_ERROR_FEATURE_NOT_PRESENT;
  VkMemoryDedicatedAllocateInfo dedicated={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.buffer=buffer->handle};
  VkMemoryAllocateInfo mai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=vp_probe_dedicated?&dedicated:NULL,.allocationSize=req.size,.memoryTypeIndex=type};
  r=vkAllocateMemory(device,&mai,NULL,&buffer->memory);if(r)return r;
  if(map_now&&vp_probe_map_before_bind){r=vkMapMemory(device,buffer->memory,0,VK_WHOLE_SIZE,0,&buffer->map);if(r)return r;}
  r=vkBindBufferMemory(device,buffer->handle,buffer->memory,0);if(r)return r;
  return map_now&&!buffer->map ? vkMapMemory(device,buffer->memory,0,VK_WHOLE_SIZE,0,&buffer->map) : VK_SUCCESS;
}
static void free_buffer(struct buffer *buffer) {
  if(buffer->map)vkUnmapMemory(device,buffer->memory);
  if(buffer->handle)vkDestroyBuffer(device,buffer->handle,NULL);
  if(buffer->memory)vkFreeMemory(device,buffer->memory,NULL);
  memset(buffer,0,sizeof(*buffer));
}
__attribute__((visibility("default"))) void vp_native_probe(VkPhysicalDevice physical,VkDevice dev,uint32_t family,log_fn logger) {
  device=dev;report=logger;vkGetPhysicalDeviceMemoryProperties(physical,&props);
  VkCommandPool pool=VK_NULL_HANDLE;VkCommandBuffer command=VK_NULL_HANDLE;
  struct buffer src={0},dst={0};VkImage image=VK_NULL_HANDLE;VkDeviceMemory imageMemory=VK_NULL_HANDLE;
  unsigned success=0, total_failures=0;
  const VkDeviceSize bytes=vp_probe_bytes;
  const uint32_t width=4096,height=(uint32_t)(bytes/(4096u*8u));
  VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
  report("NATIVE-ISOLATION begin transfer pid=%d",getpid());
  VkCommandPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
  TRY(vkCreateCommandPool(device,&pci,NULL,&pool));
  VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
  TRY(vkAllocateCommandBuffers(device,&cai,&command));
  for(unsigned phase=0;phase<3;phase++)for(unsigned round=0;round<vp_probe_rounds;round++) {
    TRY(vkResetCommandPool(device,pool,VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT));
    TRY(create_buffer(&src,bytes,1));TRY(create_buffer(&dst,bytes,1));
    uint32_t pattern=0x12345600u+round+phase*16;
    for(unsigned i=0;i<bytes/4;i++)((uint32_t*)src.map)[i]=pattern;
    report("NATIVE-ISOLATION CPU-input phase=%u bytes=%llu first=%x last=%x expected=%x",phase,(unsigned long long)bytes,((volatile uint32_t*)src.map)[0],((volatile uint32_t*)src.map)[bytes/4-1],pattern);
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=src.memory,.size=bytes};
    TRY(vkFlushMappedMemoryRanges(device,1,&range));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    TRY(vkBeginCommandBuffer(command,&begin));
    if(phase==1) {
      uint32_t data[16384];for(unsigned i=0;i<16384;i++)data[i]=pattern;
      report("NATIVE-ISOLATION CPU-update-payload first=%x expected=%x",((volatile uint32_t*)data)[0],pattern);
      for(VkDeviceSize offset=0;offset<bytes;offset+=sizeof(data))vkCmdUpdateBuffer(command,src.handle,offset,sizeof(data),data);
      VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT};
      vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,NULL,0,NULL);
    }
    if(phase==2) {
      VkImageCreateInfo ici={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R16G16B16A16_UNORM,.extent={width,height,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT};
      TRY(vkCreateImage(device,&ici,NULL,&image));
      VkMemoryRequirements req;vkGetImageMemoryRequirements(device,image,&req);
      unsigned type=0;while(type<props.memoryTypeCount&&!(req.memoryTypeBits&(1u<<type)))type++;
      VkMemoryAllocateInfo mai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
      TRY(vkAllocateMemory(device,&mai,NULL,&imageMemory));TRY(vkBindImageMemory(device,image,imageMemory,0));
      VkImageMemoryBarrier ib={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
      vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&ib);
      VkBufferImageCopy region={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={width,height,1}};
      vkCmdCopyBufferToImage(command,src.handle,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
      ib.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;ib.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;ib.oldLayout=ib.newLayout;ib.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&ib);
      vkCmdCopyImageToBuffer(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dst.handle,1,&region);
    } else {
      VkBufferCopy region={.size=bytes};vkCmdCopyBuffer(command,src.handle,dst.handle,1,&region);
    }
    VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);
    TRY(vkEndCommandBuffer(command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    if(vp_probe_submit2) {
      VkCommandBufferSubmitInfo ci={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,.commandBuffer=command,.deviceMask=1};
      VkSubmitInfo2 si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO_2,.commandBufferInfoCount=1,.pCommandBufferInfos=&ci};
      TRY(vkQueueSubmit2(queue,1,&si,VK_NULL_HANDLE));
    } else {TRY(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));}
    TRY(vkQueueWaitIdle(queue));
    range.memory=dst.memory;TRY(vkInvalidateMappedMemoryRanges(device,1,&range));
    unsigned failures=0;for(unsigned i=0;i<bytes/4;i++)if(((uint32_t*)dst.map)[i]!=pattern)failures++;
    report("NATIVE-ISOLATION phase=%u round=%u failures=%u first=%x expected=%x",phase,round+1,failures,((uint32_t*)dst.map)[0],pattern);
    if(vp_probe_continue_failures) {
      range.memory=src.memory;TRY(vkInvalidateMappedMemoryRanges(device,1,&range));
      report("NATIVE-ISOLATION source-after-invalidate phase=%u first=%x",phase,((uint32_t*)src.map)[0]);
      for(unsigned i=0;i<bytes/4;i++)if(((uint32_t*)src.map)[i]!=pattern)failures++;
    }
    if(image){vkDestroyImage(device,image,NULL);image=VK_NULL_HANDLE;vkFreeMemory(device,imageMemory,NULL);imageMemory=VK_NULL_HANDLE;}
    free_buffer(&src);free_buffer(&dst);
    usleep(400000);
    total_failures+=failures;
    if(failures&&!vp_probe_continue_failures)goto cleanup;
    if(round+1==vp_probe_rounds){report("NATIVE-ISOLATION phase=%u idle",phase);sleep(3);}
  }
  success=total_failures==0;
cleanup:
  vkDeviceWaitIdle(device);
  if(pool)vkDestroyCommandPool(device,pool,NULL);
  free_buffer(&src);free_buffer(&dst);
  if(image)vkDestroyImage(device,image,NULL);if(imageMemory)vkFreeMemory(device,imageMemory,NULL);
  sleep(3);
  report("NATIVE-ISOLATION FINAL %s",success?"success":"failed");
  vp_probe_failed=!success;
}
