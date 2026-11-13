#include <stdio.h>
#include <stdarg.h>
#include "vulkan_shadow_transfer_core.c"
static void write_log(const char *format,...) {
  va_list args;va_start(args,format);vprintf(format,args);va_end(args);putchar('\n');fflush(stdout);
}
int main(int argc,char **argv) {
  if(argc<2)return 2;
  if(!freopen(argv[1],"w",stdout))return 3;
  setvbuf(stdout,NULL,_IOLBF,0);
  VkInstance instance=VK_NULL_HANDLE;
  VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="vp-vulkan-transfer-isolation",.apiVersion=VK_API_VERSION_1_3};
  VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
  VkResult r=vkCreateInstance(&ici,NULL,&instance);if(r){write_log("INSTANCE FAIL %d",r);return 4;}
  uint32_t n=1;VkPhysicalDevice physical;
  r=vkEnumeratePhysicalDevices(instance,&n,&physical);if(r){write_log("ENUM FAIL %d",r);return 5;}
  VkPhysicalDeviceProperties prop;vkGetPhysicalDeviceProperties(physical,&prop);
  write_log("DEVICE %s driver=%u",prop.deviceName,prop.driverVersion);
  float priority=1;VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=0,.queueCount=1,.pQueuePriorities=&priority};
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,.timelineSemaphore=VK_TRUE};
  VkPhysicalDeviceSynchronization2Features sync2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,.synchronization2=VK_TRUE};
  timeline.pNext=&sync2;
  VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&timeline,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
  VkDevice dev;r=vkCreateDevice(physical,&dci,NULL,&dev);if(r){write_log("DEVICE FAIL %d",r);return 6;}
  vp_probe_rounds=1;vp_probe_continue_failures=1;vp_probe_dedicated=0;
  if(argc>4)vp_probe_rounds=strtoul(argv[4],NULL,10);
  if(argc>5)vp_probe_map_before_bind=atoi(argv[5]);
  if(!vp_probe_rounds||vp_probe_rounds>32)return 2;
  vp_probe_submit2=argc>3&&atoi(argv[3]);
  if(argc>2){unsigned mb=strtoul(argv[2],NULL,10);if(mb&&mb<=128)vp_probe_bytes=(VkDeviceSize)mb*1024u*1024u;}
  vp_native_probe(physical,dev,0,write_log);
  vkDestroyDevice(dev,NULL);vkDestroyInstance(instance,NULL);return vp_probe_failed?1:0;
}
