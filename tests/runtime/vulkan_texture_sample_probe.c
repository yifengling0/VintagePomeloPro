/* Offscreen Vulkan texture qualification through the active Wine Vulkan route.
 * This exercises real allocation, upload, UNORM/sRGB views, mip sampling,
 * filtering, partial updates, GPU completion and CPU verification. The ASTC
 * input is a legal constant-color void-extent fixture, not a BC transcoder.
 * No DXVK DLL, game file, feature-level setting or system policy is changed.
 */
#define WIN32_LEAN_AND_MEAN
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan_core.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "vulkan_texture_sample_spv.h"

#define INSTANCE_FUNCTIONS(X) \
  X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) \
  X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures) \
  X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
  X(vkGetPhysicalDeviceImageFormatProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define DEVICE_FUNCTIONS(X) \
  X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) \
  X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) \
  X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) \
  X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
  X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) \
  X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
  X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) \
  X(vkUpdateDescriptorSets) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
  X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateComputePipelines) \
  X(vkDestroyPipeline) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
  X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
  X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) \
  X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdDispatch) \
  X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkQueueSubmit) X(vkWaitForFences)
#define DECLARE(name) static PFN_##name name;
INSTANCE_FUNCTIONS(DECLARE)
DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE

static FILE *out;
static VkDevice device;
static VkPhysicalDevice physical;
static VkPhysicalDeviceMemoryProperties memory_props;
static VkQueue queue;
static uint32_t queue_family;
static const uint8_t colors[6][4] = {
  {224, 32, 96, 192}, {64, 192, 128, 128},
  {16, 96, 240, 64}, {128, 160, 48, 255},
  {80, 144, 208, 160}, {200, 80, 40, 96}
};

static int memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
  for (uint32_t i = 0; i < memory_props.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (memory_props.memoryTypes[i].propertyFlags & flags) == flags)
      return (int)i;
  return -1;
}

static VkResult allocate(const VkMemoryRequirements *req, VkMemoryPropertyFlags flags,
                         VkDeviceMemory *memory) {
  int type = memory_type(req->memoryTypeBits, flags);
  if (type < 0) return VK_ERROR_FEATURE_NOT_PRESENT;
  VkMemoryAllocateInfo info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  info.allocationSize = req->size;
  info.memoryTypeIndex = (uint32_t)type;
  return vkAllocateMemory(device, &info, NULL, memory);
}

static void block(uint8_t *dst, const uint8_t *rgba, int astc) {
  if (astc) {
    /* LDR void extent, all-ones extent; channels are little-endian UNORM16. */
    static const uint8_t header[8] = {0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    memcpy(dst, header, 8);
    for (int c = 0; c < 4; ++c) dst[8 + c * 2] = dst[9 + c * 2] = rgba[c];
  } else {
    for (int i = 0; i < 16; ++i) memcpy(dst + i * 4, rgba, 4);
  }
}

static void fill(uint8_t *dst, int astc, int partial) {
  if (partial) { block(dst, colors[5], astc); return; }
  if (astc) {
    for (int i = 0; i < 5; ++i) block(dst + i * 16, colors[i], 1);
  } else {
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
      memcpy(dst + (y * 8 + x) * 4, colors[(y >= 4 ? 2 : 0) + (x >= 4)], 4);
    block(dst + 256, colors[4], 0);
  }
}

static float channel(uint8_t value, int srgb) {
  float v = value / 255.0f;
  return srgb ? (v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f)) : v;
}

static int verify(const float *pixels, int srgb, int pass, float *max_error) {
  int failures = 0;
  *max_error = 0;
  for (int i = 0; i < 66; ++i) for (int c = 0; c < 4; ++c) {
    float expected;
    if (i == 64) {
      expected = 0;
      for (int b = 0; b < 4; ++b)
        expected += channel(colors[pass && b == 3 ? 5 : b][c], srgb && c < 3) * 0.25f;
    } else {
      int b = i == 65 ? 4 : ((i / 8 >= 4 ? 2 : 0) + (i % 8 >= 4));
      if (pass && b == 3) b = 5;
      expected = channel(colors[b][c], srgb && c < 3);
    }
    float error = fabsf(pixels[i * 4 + c] - expected);
    if (!isfinite(pixels[i * 4 + c]) || error > 0.012f) {
      if (failures < 4) fprintf(out, "mismatch pixel=%d channel=%d actual=%g expected=%g\n",
                               i, c, pixels[i * 4 + c], expected);
      ++failures;
    }
    if (error > *max_error) *max_error = error;
  }
  return failures;
}

static int run_case(VkFormat format, VkFormat view_format, int astc, int srgb) {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  VkDeviceMemory image_mem = VK_NULL_HANDLE, upload_mem = VK_NULL_HANDLE, result_mem = VK_NULL_HANDLE;
  VkBuffer upload = VK_NULL_HANDLE, result = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkShaderModule shader = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  void *upload_ptr = NULL, *result_ptr = NULL;
  int failures = 1;
  VkResult status;
#define CHECK(call) do { status = (call); if (status != VK_SUCCESS) { \
  fprintf(out, "error operation=%s result=%d\n", #call, status); goto cleanup; } } while (0)
  fprintf(out, "case image_format=%u view_format=%u astc=%d srgb=%d\n", format, view_format, astc, srgb);
  VkImageFormatProperties caps;
  CHECK(vkGetPhysicalDeviceImageFormatProperties(physical, format, VK_IMAGE_TYPE_2D,
    VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, &caps));
  VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  ici.imageType = VK_IMAGE_TYPE_2D; ici.format = format; ici.extent = (VkExtent3D){8, 8, 1};
  ici.mipLevels = 2; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  CHECK(vkCreateImage(device, &ici, NULL, &image));
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device, image, &req);
  fprintf(out, "image_allocation_bytes=%llu fixture_payload_bytes=%u\n",
          (unsigned long long)req.size, astc ? 80u : 320u);
  CHECK(allocate(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &image_mem));
  CHECK(vkBindImageMemory(device, image, image_mem, 0));
  VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = image; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = view_format;
  vci.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, 0, 1};
  CHECK(vkCreateImageView(device, &vci, NULL, &view));
  VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
  sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.maxLod = 1;
  CHECK(vkCreateSampler(device, &sci, NULL, &sampler));
  VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = 320; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  CHECK(vkCreateBuffer(device, &bci, NULL, &upload));
  vkGetBufferMemoryRequirements(device, upload, &req);
  CHECK(allocate(&req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &upload_mem));
  CHECK(vkBindBufferMemory(device, upload, upload_mem, 0));
  CHECK(vkMapMemory(device, upload_mem, 0, VK_WHOLE_SIZE, 0, &upload_ptr));
  bci.size = 66 * 4 * sizeof(float); bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  CHECK(vkCreateBuffer(device, &bci, NULL, &result));
  vkGetBufferMemoryRequirements(device, result, &req);
  CHECK(allocate(&req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &result_mem));
  CHECK(vkBindBufferMemory(device, result, result_mem, 0));
  CHECK(vkMapMemory(device, result_mem, 0, VK_WHOLE_SIZE, 0, &result_ptr));
  VkDescriptorSetLayoutBinding bindings[2] = {
    {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
    {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}
  };
  VkDescriptorSetLayoutCreateInfo dlci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dlci.bindingCount = 2; dlci.pBindings = bindings;
  CHECK(vkCreateDescriptorSetLayout(device, &dlci, NULL, &set_layout));
  VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
  VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = 1; dpci.poolSizeCount = 2; dpci.pPoolSizes = sizes;
  CHECK(vkCreateDescriptorPool(device, &dpci, NULL, &descriptor_pool));
  VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = descriptor_pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &set_layout;
  VkDescriptorSet set;
  CHECK(vkAllocateDescriptorSets(device, &dai, &set));
  VkDescriptorImageInfo dii = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkDescriptorBufferInfo dbi = {result, 0, 66 * 4 * sizeof(float)};
  VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
  writes[0].dstSet = set; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo = &dii;
  writes[1].dstSet = set; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo = &dbi;
  vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
  VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1; plci.pSetLayouts = &set_layout;
  CHECK(vkCreatePipelineLayout(device, &plci, NULL, &pipeline_layout));
  VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smci.codeSize = sizeof(texture_sample_spv); smci.pCode = texture_sample_spv;
  CHECK(vkCreateShaderModule(device, &smci, NULL, &shader));
  VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = shader; cpci.stage.pName = "main";
  cpci.layout = pipeline_layout;
  CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline));
  VkCommandPoolCreateInfo cmci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cmci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cmci.queueFamilyIndex = queue_family;
  CHECK(vkCreateCommandPool(device, &cmci, NULL, &command_pool));
  VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = command_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
  VkCommandBuffer cmd;
  CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));
  VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  CHECK(vkCreateFence(device, &fci, NULL, &fence));
  failures = 0;
  for (int pass = 0; pass < 2; ++pass) {
    fill(upload_ptr, astc, pass);
    memset(result_ptr, 0xff, 66 * 4 * sizeof(float));
    CHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(vkBeginCommandBuffer(cmd, &begin));
    VkMemoryBarrier host = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &host, 0, NULL, 0, NULL);
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = pass ? VK_ACCESS_SHADER_READ_BIT : 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = pass ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, pass ? 1u : 2u, 0, 1};
    vkCmdPipelineBarrier(cmd, pass ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    VkBufferImageCopy copies[2] = {0};
    copies[0].imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copies[0].imageExtent = (VkExtent3D){pass ? 4u : 8u, pass ? 4u : 8u, 1};
    if (pass) copies[0].imageOffset = (VkOffset3D){4, 4, 0};
    copies[1].bufferOffset = astc ? 64 : 256;
    copies[1].imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1};
    copies[1].imageExtent = (VkExtent3D){4, 4, 1};
    vkCmdCopyBufferToImage(cmd, upload, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, pass ? 1 : 2, copies);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      0, 0, NULL, 0, NULL, 1, &barrier);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cmd, 1, 1, 1);
    VkMemoryBarrier readback = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    readback.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; readback.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
      0, 1, &readback, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cmd));
    CHECK(vkResetFences(device, 1, &fence));
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
    CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    status = vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull);
    if (status != VK_SUCCESS) {
      fprintf(out, "gpu_wait_failed=%d probe_complete=0\n", status);
      fclose(out); ExitProcess(10); /* Do not destroy pending GPU resources. */
    }
    float max_error;
    int bad = verify(result_ptr, srgb, pass, &max_error);
    failures += bad != 0;
    fprintf(out, "pass=%d mismatched_channels=%d max_error=%g\n", pass, bad, max_error);
  }
cleanup:
  /* CHECK must not report success after an earlier verified pass. */
  if (status != VK_SUCCESS) failures = failures ? failures : 1;
  if (result_ptr) vkUnmapMemory(device, result_mem);
  if (upload_ptr) vkUnmapMemory(device, upload_mem);
  if (fence) vkDestroyFence(device, fence, NULL);
  if (command_pool) vkDestroyCommandPool(device, command_pool, NULL);
  if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
  if (shader) vkDestroyShaderModule(device, shader, NULL);
  if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, NULL);
  if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, NULL);
  if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, NULL);
  if (sampler) vkDestroySampler(device, sampler, NULL);
  if (view) vkDestroyImageView(device, view, NULL);
  if (image) vkDestroyImage(device, image, NULL);
  if (result) vkDestroyBuffer(device, result, NULL);
  if (upload) vkDestroyBuffer(device, upload, NULL);
  if (image_mem) vkFreeMemory(device, image_mem, NULL);
  if (result_mem) vkFreeMemory(device, result_mem, NULL);
  if (upload_mem) vkFreeMemory(device, upload_mem, NULL);
  fprintf(out, "case_complete=1 failures=%d\n", failures);
  return failures != 0;
#undef CHECK
}

int main(int argc, char **argv) {
  out = fopen(argc > 1 ? argv[1] : "C:\\windows\\temp\\vp-texture-sample.tsv", "w");
  if (!out) return 2;
  setvbuf(out, NULL, _IONBF, 0);
  fprintf(out, "texture_sample_probe=1 pid=%lu\n", GetCurrentProcessId());
  HMODULE module = LoadLibraryA("vulkan-1.dll");
  PFN_vkGetInstanceProcAddr gipa = module ? (PFN_vkGetInstanceProcAddr)GetProcAddress(module, "vkGetInstanceProcAddr") : NULL;
  if (!gipa) { fprintf(out, "loader_failed=%lu\n", GetLastError()); fclose(out); return 3; }
  PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
  if (!create) { fclose(out); return 3; }
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "VP offscreen texture sample probe"; app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VkInstance instance;
  VkResult status = create(&ici, NULL, &instance);
  if (status) { fprintf(out, "instance_failed=%d\n", status); fclose(out); return 4; }
#define LOAD(name) name = (PFN_##name)gipa(instance, #name); \
  if (!name) { fprintf(out, "missing=%s\n", #name); fclose(out); return 5; }
  INSTANCE_FUNCTIONS(LOAD)
#undef LOAD
  uint32_t count = 1;
  status = vkEnumeratePhysicalDevices(instance, &count, &physical);
  if ((status != VK_SUCCESS && status != VK_INCOMPLETE) || !count) {
    fprintf(out, "physical_device_failed=%d\n", status); vkDestroyInstance(instance, NULL); fclose(out); return 6;
  }
  VkPhysicalDeviceProperties props;
  VkPhysicalDeviceFeatures features, enabled = {0};
  vkGetPhysicalDeviceProperties(physical, &props);
  vkGetPhysicalDeviceFeatures(physical, &features);
  vkGetPhysicalDeviceMemoryProperties(physical, &memory_props);
  fprintf(out, "device=%s bc=%u astcLdr=%u etc2=%u max2d=%u\n", props.deviceName,
    features.textureCompressionBC, features.textureCompressionASTC_LDR, features.textureCompressionETC2,
    props.limits.maxImageDimension2D);
  VkQueueFamilyProperties queues[32]; count = 32;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues);
  for (queue_family = 0; queue_family < count; ++queue_family)
    if (queues[queue_family].queueCount && (queues[queue_family].queueFlags & VK_QUEUE_COMPUTE_BIT)) break;
  if (queue_family == count) { vkDestroyInstance(instance, NULL); fclose(out); return 7; }
  float priority = 1;
  VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = queue_family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
  enabled.textureCompressionASTC_LDR = features.textureCompressionASTC_LDR;
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci; dci.pEnabledFeatures = &enabled;
  status = vkCreateDevice(physical, &dci, NULL, &device);
  if (status) { fprintf(out, "device_failed=%d\n", status); vkDestroyInstance(instance, NULL); fclose(out); return 8; }
#define LOAD(name) name = (PFN_##name)vkGetDeviceProcAddr(device, #name); \
  if (!name) { fprintf(out, "missing=%s\n", #name); fclose(out); return 9; }
  DEVICE_FUNCTIONS(LOAD)
#undef LOAD
  vkGetDeviceQueue(device, queue_family, 0, &queue);
  int failures = run_case(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, 0, 0);
  failures += run_case(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, 0, 1);
  if (features.textureCompressionASTC_LDR) {
    failures += run_case(VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, 1, 0);
    failures += run_case(VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_4x4_SRGB_BLOCK, 1, 1);
  } else {
    fprintf(out, "astc_skipped=feature_unavailable\n"); ++failures;
  }
  vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL); FreeLibrary(module);
  fprintf(out, "probe_complete=1 failures=%d\n", failures); fclose(out);
  return failures ? 1 : 0;
}
