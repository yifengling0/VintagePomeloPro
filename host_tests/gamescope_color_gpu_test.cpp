// SPDX-License-Identifier: LGPL-2.1-or-later
// Independent Vulkan consumer of the actual Gamescope color shader.
// Derived from HSL native/gnu/probes/gamescope_color.cpp; see migration-sources.json.
// Host-only GPU readback: four masks of the small output in each positive case.
#define VK_NO_PROTOTYPES 1
#include "../native/gamescope/hsl/vulkan_descriptor_fallback.h"
#include "cs_composite_blit.h"
#include "cs_easu.h"
#include "cs_easu_fp16.h"
#include "cs_composite_rcas.h"
#include "cs_composite_rcas_1px.h"
#include <cmath>
#include <cstdlib>
#define A_CPU
#include "ffx_a.h"
#include "ffx_fsr1.h"
#define gamescope_color_spv cs_composite_blit
#include <dlfcn.h>
#include <cstdio>
#include <vector>
#include <unistd.h>

#define GLOBAL(X) X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceFeatures2) X(GetPhysicalDeviceProperties) \
 X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceFormatProperties) X(GetPhysicalDeviceMemoryProperties) \
 X(GetPhysicalDeviceImageFormatProperties2) X(CreateDevice) X(GetDeviceProcAddr)
#define DEVICE(X) X(DestroyDevice) X(GetDeviceQueue) X(CreateSamplerYcbcrConversion) X(DestroySamplerYcbcrConversion) \
 X(CreateSampler) X(DestroySampler) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) \
 X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(CreateCommandPool) X(DestroyCommandPool) \
 X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer) X(CmdPipelineBarrier) \
 X(CreateFence) X(DestroyFence) X(GetFenceStatus) X(WaitForFences) X(ResetFences) X(QueueSubmit) X(CreateShaderModule) X(DestroyShaderModule) \
 X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool) \
 X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
 X(CreateComputePipelines) X(DestroyPipeline) X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdDispatch)
#define OUTPUT_DEVICE(X) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements2) X(BindImageMemory) \
 X(CreateImageView) X(DestroyImageView) X(CmdCopyImageToBuffer)
#define DECLARE(name) static PFN_vk##name name;
GLOBAL(DECLARE) DEVICE(DECLARE) OUTPUT_DEVICE(DECLARE)
#undef DECLARE
static void fail(const char* stage, int result) {
    std::fprintf(stderr, "VPP_GAMESCOPE_COLOR_FAIL stage=%s result=%d\n", stage, result);
    std::exit(1);
}
#define CHECK(call) do { std::printf("VPP_GAMESCOPE_COLOR_STEP %s\n", #call); VkResult r = (call); if (r != VK_SUCCESS) fail(#call, r); } while (0)
int main(int argc, char** argv) {
    const bool uploadOnly = false, pipelineOnly = false;
    const bool raw = argc == 2 && std::strcmp(argv[1], "--raw") == 0;
    const bool missingEncode = argc == 2 && std::strcmp(argv[1], "--missing-encode") == 0;
    const bool doubleEncode = argc == 2 && std::strcmp(argv[1], "--double-encode") == 0;
    const bool gray = argc == 2 && std::strcmp(argv[1], "--gray") == 0;
    const bool fp16 = argc == 2 && std::strcmp(argv[1], "--easu-fp16") == 0;
    const bool easu = fp16 || (argc == 2 && std::strcmp(argv[1], "--easu-fp32") == 0);
    const bool rcas1px = argc == 2 && std::strcmp(argv[1], "--rcas-1px") == 0;
    const bool rcas = rcas1px || (argc == 2 && std::strcmp(argv[1], "--rcas-quad") == 0);
    const bool legacyBounds = argc == 2 && std::strcmp(argv[1], "--legacy-window-bounds") == 0;
    const bool windowBounds = legacyBounds || (argc == 2 && std::strcmp(argv[1], "--window-bounds") == 0);
    if (argc > 2 || (argc == 2 && !raw && !missingEncode && !doubleEncode && !gray && !easu && !rcas && !windowBounds)) return 64;
    const unsigned side = easu ? 17u : windowBounds ? 8u : 1u; // odd extent exercises all EASU edge guards
    constexpr unsigned bufferBytes = 4096;
    setvbuf(stdout, nullptr, _IONBF, 0); alarm(25);
    void* loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    auto get = loader ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr")) : nullptr;
    if (!get) fail("GNU Vulkan loader", -1);
    auto create = reinterpret_cast<PFN_vkCreateInstance>(get(VK_NULL_HANDLE, "vkCreateInstance"));
    VkApplicationInfo app = {}; app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instanceInfo = {}; instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; instanceInfo.pApplicationInfo = &app;
    VkInstance instance; CHECK(create(&instanceInfo, nullptr, &instance));
#define LOAD(name) name = reinterpret_cast<PFN_vk##name>(get(instance, "vk" #name)); if (!name) fail("vk" #name, -1);
    GLOBAL(LOAD)
#undef LOAD
    uint32_t count = 0; CHECK(EnumeratePhysicalDevices(instance, &count, nullptr));
    if (!count || count > 16) fail("physical device count", -1);
    std::vector<VkPhysicalDevice> physicals(count); CHECK(EnumeratePhysicalDevices(instance, &count, physicals.data()));
    auto physical = physicals[0];
    VkPhysicalDeviceProperties deviceProperties;
    GetPhysicalDeviceProperties(physical, &deviceProperties);
    std::printf("VPP_GAMESCOPE_LIMITS samplers=%u sampledImages=%u storageImages=%u uniformRange=%u\n",
        deviceProperties.limits.maxPerStageDescriptorSamplers, deviceProperties.limits.maxPerStageDescriptorSampledImages,
        deviceProperties.limits.maxPerStageDescriptorStorageImages, deviceProperties.limits.maxUniformBufferRange);
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr = {}; ycbcr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    VkPhysicalDeviceVulkan12Features features12 = {}; features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES; features12.pNext = &ycbcr;
    VkPhysicalDeviceFeatures2 features = {}; features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2; features.pNext = &features12;
    GetPhysicalDeviceFeatures2(physical, &features);
    if (fp16 && (!features12.shaderFloat16 || !features.features.shaderInt16))
        fail("real half precision features", VK_ERROR_FEATURE_NOT_PRESENT);
    std::printf("VPP_GAMESCOPE_COLOR_FEATURES ycbcr=%u scalar=%u timeline=%u storageWrite=%u sampledArrayDynamic=%u\n",
        ycbcr.samplerYcbcrConversion, features12.scalarBlockLayout,
        features12.timelineSemaphore, features.features.shaderStorageImageWriteWithoutFormat,
        features.features.shaderSampledImageArrayDynamicIndexing);
    if (!ycbcr.samplerYcbcrConversion || !features12.scalarBlockLayout ||
        !features12.timelineSemaphore || !features.features.shaderStorageImageWriteWithoutFormat)
        fail("required real Gamescope shader features", VK_ERROR_FEATURE_NOT_PRESENT);
    GetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    if (!count || count > 64) fail("queue count", -1);
    std::vector<VkQueueFamilyProperties> families(count); GetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    uint32_t family = 0; while (family < count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    if (family == count) fail("compute queue", -1);
    float priority = 1;
    VkDeviceQueueCreateInfo queueInfo = {}; queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = family; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
    ycbcr = {}; ycbcr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES; ycbcr.samplerYcbcrConversion = VK_TRUE;
    features12 = {}; features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES; features12.pNext = &ycbcr; features12.scalarBlockLayout = VK_TRUE; features12.timelineSemaphore = VK_TRUE;
    features12.shaderFloat16 = fp16;
    VkPhysicalDeviceFeatures enabled = {}; enabled.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    enabled.shaderInt16 = fp16;
    VkDeviceCreateInfo deviceInfo = {}; deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; deviceInfo.pNext = &features12;
    deviceInfo.pEnabledFeatures = &enabled;
    deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
    VkDevice device; CHECK(CreateDevice(physical, &deviceInfo, nullptr, &device));
#define LOAD(name) name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(device, "vk" #name)); if (!name) fail("vk" #name, -1);
    DEVICE(LOAD) OUTPUT_DEVICE(LOAD)
#undef LOAD
    VkQueue queue; GetDeviceQueue(device, family, 0, &queue);
    VkFormatProperties nv12 = {}; GetPhysicalDeviceFormatProperties(physical, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, &nv12);
    VkSamplerYcbcrConversionCreateInfo conversionInfo = {}; conversionInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
    conversionInfo.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM; conversionInfo.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    conversionInfo.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
    conversionInfo.xChromaOffset = conversionInfo.yChromaOffset = nv12.optimalTilingFeatures & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT ? VK_CHROMA_LOCATION_COSITED_EVEN : VK_CHROMA_LOCATION_MIDPOINT;
    conversionInfo.chromaFilter = VK_FILTER_NEAREST;
    VkSamplerYcbcrConversion conversion; CHECK(CreateSamplerYcbcrConversion(device, &conversionInfo, nullptr, &conversion));
    VkPhysicalDeviceMemoryProperties properties; GetPhysicalDeviceMemoryProperties(physical, &properties);
    {
        hsl::VulkanDescriptorFallback fallback;
        CHECK(fallback.init(physical, device, GetDeviceProcAddr, GetPhysicalDeviceImageFormatProperties2, properties, conversion));
        VkBufferCreateInfo bufferInfo = {}; bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bufferInfo.size = bufferBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer buffer; CHECK(CreateBuffer(device, &bufferInfo, nullptr, &buffer));
        VkMemoryRequirements memory; GetBufferMemoryRequirements(device, buffer, &memory);
        uint32_t index = 0;
        const auto hostFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        while (index < properties.memoryTypeCount && (!(memory.memoryTypeBits & (1u << index)) || (properties.memoryTypes[index].propertyFlags & hostFlags) != hostFlags)) ++index;
        if (index == properties.memoryTypeCount) fail("host coherent diagnostic buffer", VK_ERROR_FEATURE_NOT_PRESENT);
        VkMemoryAllocateInfo allocation = {}; allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; allocation.allocationSize = memory.size; allocation.memoryTypeIndex = index;
        VkDeviceMemory backing; CHECK(AllocateMemory(device, &allocation, nullptr, &backing)); CHECK(BindBufferMemory(device, buffer, backing, 0));
        void* mapped; CHECK(MapMemory(device, backing, 0, memory.size, 0, &mapped));
        std::memset(mapped, 0, bufferBytes); std::memcpy(mapped, fallback.Upload.data(), fallback.UploadBytes);
        // A nonzero sampled input and a separate storage output distinguish
        // real compositing from a no-op shader or aliased read/write image.
        // Midtones, unlike white/black, expose a missing or duplicate EOTF.
        const unsigned char input[4] = {uint8_t(gray ? 128 : 64),128,uint8_t(gray ? 128 : 192),uint8_t(windowBounds ? 0 : 255)};
        std::memcpy(mapped, input, sizeof(input));
        float* uniform = reinterpret_cast<float*>(static_cast<unsigned char*>(mapped) + 512);
        for (unsigned i = 0; i < 8; ++i) {
            uniform[2*i] = uniform[2*i+1] = uniform[32+i] = 1.0f;
            uniform[40+12*i] = uniform[45+12*i] = uniform[50+12*i] = 1.0f;
        }
        const uint32_t nearest = 1;
        std::memcpy(uniform+139, &nearest, sizeof(nearest));
        uniform[141] = uniform[142] = uniform[143] = uniform[144] = 1.0f;
        if (windowBounds) {
            // A full desktop behind a raised 4x4 opaque taskbar/popup. Its
            // producer leaves alpha zero; the input sampling view supplies
            // opacity only inside the actual window rectangle.
            uniform[0]=uniform[1]=1.0f/side;
            uniform[2]=uniform[3]=0.25f;
            uniform[18]=-2; uniform[19]=-4;
            const uint32_t border=legacyBounds ? 2u : 0u;
            const uint32_t alphaMode=legacyBounds ? (2u<<2) : 0u;
            std::memcpy(uniform+136,&border,sizeof(border));
            std::memcpy(uniform+140,&alphaMode,sizeof(alphaMode));
        }
        if (easu) {
            FsrEasuCon(reinterpret_cast<AU1*>(uniform),reinterpret_cast<AU1*>(uniform+4),
                reinterpret_cast<AU1*>(uniform+8),reinterpret_cast<AU1*>(uniform+12),
                1,1,1,1,float(side),float(side));
        } else if (rcas) {
            std::memset(uniform,0,1024);
            uniform[30]=1; // eight opacities follow layer0Offset + 7 scales + 7 offsets
            uniform[38]=uniform[43]=uniform[48]=1; // identity CTM
            AU1 sharpen[4]; FsrRcasCon(sharpen,1.0f);
            std::memcpy(uniform+136,sharpen,sizeof(uint32_t));
        }
        VkImageCreateInfo outputInfo = {}; outputInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        outputInfo.imageType = VK_IMAGE_TYPE_2D; outputInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        outputInfo.extent = {side,side,1}; outputInfo.mipLevels = outputInfo.arrayLayers = 1;
        outputInfo.samples = VK_SAMPLE_COUNT_1_BIT; outputInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        outputInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkImage outputImage; CHECK(CreateImage(device, &outputInfo, nullptr, &outputImage));
        VkMemoryDedicatedRequirements outputDedicated = {}; outputDedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
        VkMemoryRequirements2 outputMemory = {}; outputMemory.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2; outputMemory.pNext = &outputDedicated;
        VkImageMemoryRequirementsInfo2 outputQuery = {}; outputQuery.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2; outputQuery.image = outputImage;
        GetImageMemoryRequirements2(device, &outputQuery, &outputMemory);
        uint32_t outputIndex = 0;
        while (outputIndex < properties.memoryTypeCount && !(outputMemory.memoryRequirements.memoryTypeBits & (1u << outputIndex))) ++outputIndex;
        if (outputIndex == properties.memoryTypeCount) fail("output memory type", VK_ERROR_FEATURE_NOT_PRESENT);
        VkMemoryDedicatedAllocateInfo outputImport = {}; outputImport.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO; outputImport.image = outputImage;
        allocation.allocationSize = outputMemory.memoryRequirements.size; allocation.memoryTypeIndex = outputIndex;
        allocation.pNext = outputDedicated.requiresDedicatedAllocation || outputDedicated.prefersDedicatedAllocation ? &outputImport : nullptr;
        VkDeviceMemory outputBacking; CHECK(AllocateMemory(device, &allocation, nullptr, &outputBacking));
        CHECK(BindImageMemory(device, outputImage, outputBacking, 0));
        VkImageViewCreateInfo outputViewInfo = {}; outputViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        outputViewInfo.image = outputImage; outputViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D; outputViewInfo.format = outputInfo.format;
        outputViewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkImageView outputView; CHECK(CreateImageView(device, &outputViewInfo, nullptr, &outputView));
        // Venus may defer creation calls. Complete the uploads before creating
        // a shader/pipeline, so an upload-only failure cannot be compilation.
        VkCommandPoolCreateInfo commandPoolInfo = {}; commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; commandPoolInfo.queueFamilyIndex = family; commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VkCommandPool pool; CHECK(CreateCommandPool(device, &commandPoolInfo, nullptr, &pool));
        VkCommandBufferAllocateInfo commandInfo = {}; commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; commandInfo.commandPool = pool; commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; commandInfo.commandBufferCount = 1;
        VkCommandBuffer command; CHECK(AllocateCommandBuffers(device, &commandInfo, &command));
        VkFenceCreateInfo fenceInfo = {}; fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence; CHECK(CreateFence(device, &fenceInfo, nullptr, &fence));
        for (const unsigned slot : {0u, 2u, 3u, 1u}) {
            std::printf("VPP_GAMESCOPE_TYPED_INITIALIZATION_BEGIN slot=%u\n", slot);
            CHECK(ResetCommandBuffer(command, 0));
            CHECK(ResetFences(device, 1, &fence));
            VkCommandBufferBeginInfo begin = {}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            CHECK(BeginCommandBuffer(command, &begin));
            fallback.record(command, buffer, 0, 1u << slot);
            CHECK(EndCommandBuffer(command));
            VkSubmitInfo submit = {}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
            CHECK(QueueSubmit(queue, 1, &submit, fence));
            CHECK(WaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
            std::printf("VPP_GAMESCOPE_TYPED_INITIALIZATION_PASS slot=%u\n", slot);
        }
        if (!uploadOnly) {
        VkSamplerCreateInfo samplerInfo = {}; samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VkSampler sampler; CHECK(CreateSampler(device, &samplerInfo, nullptr, &sampler));
        VkSamplerYcbcrConversionInfo converted = {}; converted.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO; converted.conversion = conversion;
        samplerInfo.pNext = &converted;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        VkSampler convertedSampler; CHECK(CreateSampler(device, &samplerInfo, nullptr, &convertedSampler));
        // Vulkan accounts for the actual NV12 conversion descriptor footprint.
        VkSamplerYcbcrConversionImageFormatProperties conversionProperties = {}; conversionProperties.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES;
        VkImageFormatProperties2 imageProperties = {}; imageProperties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2; imageProperties.pNext = &conversionProperties;
        VkPhysicalDeviceImageFormatInfo2 imageQuery = {}; imageQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
        imageQuery.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM; imageQuery.type = VK_IMAGE_TYPE_2D; imageQuery.tiling = VK_IMAGE_TILING_OPTIMAL; imageQuery.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        CHECK(GetPhysicalDeviceImageFormatProperties2(physical, &imageQuery, &imageProperties));
        std::array<VkSampler, 16> immutable; immutable.fill(convertedSampler);
        std::array<VkDescriptorSetLayoutBinding, 8> bindings = {};
        const VkDescriptorType types[8] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
        const uint32_t counts[8] = { 1,1,1,16,16,2,2,1 };
        for (unsigned i = 0; i < 8; ++i) bindings[i] = { i, types[i], counts[i], VK_SHADER_STAGE_COMPUTE_BIT, i == 4 ? immutable.data() : nullptr };
        VkDescriptorSetLayoutCreateInfo layoutInfo = {}; layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; layoutInfo.bindingCount = bindings.size(); layoutInfo.pBindings = bindings.data();
        VkDescriptorSetLayout layout; CHECK(CreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout));
        CHECK(GetFenceStatus(device, fence));
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,20 + 16 * conversionProperties.combinedImageSamplerDescriptorCount}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
        VkDescriptorPoolCreateInfo poolInfo = {}; poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; poolInfo.maxSets = 1; poolInfo.poolSizeCount = 4; poolInfo.pPoolSizes = sizes;
        VkDescriptorPool descriptorPool; CHECK(CreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool));
        VkDescriptorSetAllocateInfo setInfo = {}; setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; setInfo.descriptorPool = descriptorPool; setInfo.descriptorSetCount = 1; setInfo.pSetLayouts = &layout;
        VkDescriptorSet set; CHECK(AllocateDescriptorSets(device, &setInfo, &set));
        CHECK(GetFenceStatus(device, fence));
        std::array<std::array<VkDescriptorImageInfo, 16>, 7> descriptors = {};
        VkDescriptorBufferInfo buffers[2] = {{buffer,512,1024}, {buffer,256,32}};
        std::array<VkWriteDescriptorSet, 8> writes = {};
        for (unsigned i = 0; i < 8; ++i) {
            auto& write = writes[i]; write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; write.dstSet = set; write.dstBinding = i; write.descriptorCount = counts[i]; write.descriptorType = types[i];
            if (i == 0 || i == 7) { write.pBufferInfo = &buffers[i == 7]; continue; }
            const auto slot = i == 4 ? fallback.Ycbcr : i == 5 ? fallback.Shaper : i == 6 ? fallback.Lut3d : fallback.Rgba;
            for (unsigned j = 0; j < counts[i]; ++j) descriptors[i][j] = {sampler, fallback.view(slot), i >= 5 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL};
            write.pImageInfo = descriptors[i].data();
        }
        descriptors[1][0].imageView = descriptors[2][0].imageView = outputView;
        VkImageView opaqueView=VK_NULL_HANDLE;
        if (windowBounds) {
            VkImageViewCreateInfo opaqueInfo={}; opaqueInfo.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            opaqueInfo.image=fallback.image(fallback.Rgba); opaqueInfo.viewType=VK_IMAGE_VIEW_TYPE_2D;
            opaqueInfo.format=VK_FORMAT_R8G8B8A8_UNORM;
            opaqueInfo.components.a=VK_COMPONENT_SWIZZLE_ONE;
            opaqueInfo.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            CHECK(CreateImageView(device,&opaqueInfo,nullptr,&opaqueView));
            descriptors[3][0].imageView=descriptors[3][1].imageView=opaqueView;
        }
        UpdateDescriptorSets(device, writes.size(), writes.data(), 0, nullptr);
        CHECK(GetFenceStatus(device, fence));
        VkPushConstantRange colorState = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
        VkPipelineLayoutCreateInfo pipelineLayoutInfo = {}; pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pipelineLayoutInfo.setLayoutCount = 1; pipelineLayoutInfo.pSetLayouts = &layout; pipelineLayoutInfo.pushConstantRangeCount = 1; pipelineLayoutInfo.pPushConstantRanges = &colorState;
        VkPipelineLayout pipelineLayout; CHECK(CreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout));
        VkShaderModuleCreateInfo shaderInfo = {}; shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; shaderInfo.codeSize = sizeof(gamescope_color_spv); shaderInfo.pCode = gamescope_color_spv;
        if (easu) {
            shaderInfo.pCode=fp16 ? cs_easu_fp16 : cs_easu;
            shaderInfo.codeSize=fp16 ? sizeof(cs_easu_fp16) : sizeof(cs_easu);
        } else if (rcas) {
            shaderInfo.pCode=rcas1px ? cs_composite_rcas_1px : cs_composite_rcas;
            shaderInfo.codeSize=rcas1px ? sizeof(cs_composite_rcas_1px) : sizeof(cs_composite_rcas);
        }
        VkShaderModule shader; CHECK(CreateShaderModule(device, &shaderInfo, nullptr, &shader));
        CHECK(GetFenceStatus(device, fence));
        VkComputePipelineCreateInfo pipelineInfo = {}; pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO; pipelineInfo.layout = pipelineLayout;
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr};
        // Match descriptor_set.h: sRGB=1, passthru=4, Gamma22=0, Count=2.
        const uint32_t constants[7] = {windowBounds ? 2u : 1u,0,0,0, windowBounds ? 9u : raw || doubleEncode ? 4u : 1u,
            raw || missingEncode ? 2u : 0u,0};
        std::array<VkSpecializationMapEntry,7> entries = {};
        for (uint32_t i = 0; i < entries.size(); ++i)
            entries[i] = {i == 6 ? 7u : i, i * uint32_t(sizeof(uint32_t)), sizeof(uint32_t)};
        VkSpecializationInfo specialization = {uint32_t(entries.size()), entries.data(), sizeof(constants), constants};
        pipelineInfo.stage.pSpecializationInfo = &specialization;
        VkPipeline pipeline; CHECK(CreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
        CHECK(GetFenceStatus(device, fence));
        constexpr unsigned frameCount = 4;
        for (uint32_t frame = 0; !pipelineOnly && frame < frameCount; ++frame) {
            const uint32_t mask = frame % 4;
            CHECK(ResetCommandBuffer(command, 0));
            VkCommandBufferBeginInfo begin = {}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            CHECK(BeginCommandBuffer(command, &begin));
            VkMemoryBarrier ordered = {}; ordered.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            ordered.srcAccessMask = ordered.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            CmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               0, 1, &ordered, 0, nullptr, 0, nullptr);
            VkImageMemoryBarrier target = {}; target.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            target.oldLayout = frame ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
            target.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            target.srcAccessMask = frame ? VkAccessFlags(VK_ACCESS_TRANSFER_READ_BIT) : 0u;
            target.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            target.srcQueueFamilyIndex = target.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            target.image = outputImage; target.subresourceRange = outputViewInfo.subresourceRange;
            CmdPipelineBarrier(command, frame ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,nullptr,0,nullptr,1,&target);
            CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
            CmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &mask);
            const unsigned tile=easu || rcas ? 16u : 8u;
            CmdDispatch(command, (side+tile-1)/tile, (side+tile-1)/tile, 1);
            VkMemoryBarrier visible = {}; visible.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER; visible.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; visible.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            target.oldLayout = VK_IMAGE_LAYOUT_GENERAL; target.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            target.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; target.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            CmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&target);
            VkBufferImageCopy copy = {}; copy.bufferOffset = 2048; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent = {side,side,1};
            CmdCopyImageToBuffer(command,outputImage,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
            visible.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&visible,0,nullptr,0,nullptr);
            CHECK(EndCommandBuffer(command)); CHECK(ResetFences(device, 1, &fence));
            VkSubmitInfo submit = {}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
            CHECK(QueueSubmit(queue, 1, &submit, fence));
            CHECK(WaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
            const unsigned char* pixel = static_cast<unsigned char*>(mapped)+2048;
            std::printf("VPP_GAMESCOPE_COMPOSITE_PIXEL mask=%u rgba=%u,%u,%u,%u\n",mask,pixel[0],pixel[1],pixel[2],pixel[3]);
            for (unsigned i=0;i<side*side*4;++i) {
                const int expected = windowBounds && i%4==3 ? 255 :
                    i%4==3 || easu || raw || !(mask&1) ? input[i%4] : 0;
                if (std::abs(int(pixel[i])-expected)>1) fail("actual compositor midtone",i);
            }
            std::printf("VPP_GAMESCOPE_COMPOSITE_MASK_PASS mask=%u rgba=%u,%u,%u,%u\n",mask,pixel[0],pixel[1],pixel[2],pixel[3]);
        }
        DestroyPipeline(device, pipeline, nullptr); DestroyShaderModule(device, shader, nullptr);
        DestroyPipelineLayout(device, pipelineLayout, nullptr); DestroyDescriptorPool(device, descriptorPool, nullptr);
        if (opaqueView) DestroyImageView(device,opaqueView,nullptr);
        DestroyDescriptorSetLayout(device, layout, nullptr); DestroySampler(device, convertedSampler, nullptr); DestroySampler(device, sampler, nullptr);
        }
        DestroyFence(device, fence, nullptr); DestroyCommandPool(device, pool, nullptr);
        DestroyImageView(device,outputView,nullptr); DestroyImage(device,outputImage,nullptr); FreeMemory(device,outputBacking,nullptr);
        UnmapMemory(device, backing); DestroyBuffer(device, buffer, nullptr); FreeMemory(device, backing, nullptr);
    }
    DestroySamplerYcbcrConversion(device, conversion, nullptr); DestroyDevice(device, nullptr); DestroyInstance(instance, nullptr); dlclose(loader);
    if (uploadOnly) std::printf("VPP_GAMESCOPE_UPLOAD_PASS diagnosticReadbackBytes=0\n");
    else if (pipelineOnly) std::printf("VPP_GAMESCOPE_PIPELINE_PASS diagnosticReadbackBytes=0\n");
    else {
        std::printf("VPP_GAMESCOPE_COMPOSITE_PASS masks=4 diagnosticReadbackBytes=%u nullDescriptorEnabled=0\n",side*side*16);
    }
}
