/* Read-only Vulkan format-capability probe through the active Wine Vulkan route.
 * Does not allocate device images or alter feature-level policy.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <stdio.h>
#include <vulkan/vulkan_core.h>

static FILE *out;

static int query_vk(void) {
    HMODULE module = LoadLibraryA("vulkan-1.dll");
    PFN_vkCreateInstance create;
    PFN_vkEnumeratePhysicalDevices enumerate;
    PFN_vkGetPhysicalDeviceProperties properties;
    PFN_vkGetPhysicalDeviceFeatures features;
    PFN_vkGetPhysicalDeviceFormatProperties format;
    PFN_vkGetPhysicalDeviceImageFormatProperties image;
    PFN_vkDestroyInstance destroy;
    VkInstance instance = VK_NULL_HANDLE;
    VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkPhysicalDevice devices[8];
    uint32_t count = 8;
    VkResult result;
    if (!module) { fprintf(out, "vk_loader_error=%lu\n", GetLastError()); return 3; }
    create = (PFN_vkCreateInstance)GetProcAddress(module, "vkCreateInstance");
    enumerate = (PFN_vkEnumeratePhysicalDevices)GetProcAddress(module, "vkEnumeratePhysicalDevices");
    properties = (PFN_vkGetPhysicalDeviceProperties)GetProcAddress(module, "vkGetPhysicalDeviceProperties");
    features = (PFN_vkGetPhysicalDeviceFeatures)GetProcAddress(module, "vkGetPhysicalDeviceFeatures");
    format = (PFN_vkGetPhysicalDeviceFormatProperties)GetProcAddress(module, "vkGetPhysicalDeviceFormatProperties");
    image = (PFN_vkGetPhysicalDeviceImageFormatProperties)GetProcAddress(module, "vkGetPhysicalDeviceImageFormatProperties");
    destroy = (PFN_vkDestroyInstance)GetProcAddress(module, "vkDestroyInstance");
    if (!create || !enumerate || !properties || !features || !format || !image || !destroy) {
        fprintf(out, "vk_exports_incomplete\n"); FreeLibrary(module); return 4;
    }
    app.apiVersion = VK_API_VERSION_1_1;
    app.pApplicationName = "vp-bc3-diagnostic";
    info.pApplicationInfo = &app;
    result = create(&info, NULL, &instance);
    fprintf(out, "vk_create_instance=%d\n", result); fflush(out);
    if (result) { FreeLibrary(module); return 5; }
    result = enumerate(instance, &count, devices);
    VkResult enumeration_result = result;
    fprintf(out, "vk_enumerate=%d count=%u\n", result, count); fflush(out);
    if (!result) for (uint32_t d = 0; d < count; ++d) {
        VkPhysicalDeviceProperties props;
        VkPhysicalDeviceFeatures feats;
        const VkFormat formats[] = {
            VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
            VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK,
            VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK,
            VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_4x4_SRGB_BLOCK,
            VK_FORMAT_ASTC_6x6_UNORM_BLOCK, VK_FORMAT_ASTC_6x6_SRGB_BLOCK,
            VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK,
            VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK,
            VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R16G16B16A16_SFLOAT };
        const VkImageUsageFlags usages[] = { VK_IMAGE_USAGE_SAMPLED_BIT, 7, 0x17 };
        properties(devices[d], &props); features(devices[d], &feats);
        fprintf(out, "vk_device=%s bc=%u max2d=%u maxfb=%ux%u\n", props.deviceName,
            feats.textureCompressionBC, props.limits.maxImageDimension2D,
            props.limits.maxFramebufferWidth, props.limits.maxFramebufferHeight);
        fprintf(out, "vk_compression bc=%u astcLdr=%u etc2=%u maxLayers=%u maxAllocationCount=%u\n",
            feats.textureCompressionBC, feats.textureCompressionASTC_LDR, feats.textureCompressionETC2,
            props.limits.maxImageArrayLayers, props.limits.maxMemoryAllocationCount);
        for (unsigned f = 0; f < sizeof(formats)/sizeof(formats[0]); ++f) {
            VkFormatProperties caps;
            format(devices[d], formats[f], &caps);
            fprintf(out, "vk_format=%u optimal=0x%x linear=0x%x\n", formats[f], caps.optimalTilingFeatures, caps.linearTilingFeatures);
            for (unsigned t = 0; t < 2; ++t) for (unsigned u = 0; u < 3; ++u) for (unsigned m = 0; m < 2; ++m) {
                VkImageFormatProperties limits = { 0 };
                VkImageCreateFlags flags = m ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
                result = image(devices[d], formats[f], VK_IMAGE_TYPE_2D, (VkImageTiling)t, usages[u], flags, &limits);
                fprintf(out, "vk_image format=%u tiling=%u usage=0x%x flags=0x%x result=%d max=%ux%ux%u mips=%u layers=%u samples=0x%x maxbytes=%llu exact_extent_fits=%u\n",
                    formats[f], t, usages[u], flags, result, limits.maxExtent.width, limits.maxExtent.height,
                    limits.maxExtent.depth, limits.maxMipLevels, limits.maxArrayLayers, limits.sampleCounts,
                    (unsigned long long)limits.maxResourceSize,
                    result == VK_SUCCESS && limits.maxExtent.width >= 14360 && limits.maxExtent.height >= 4724);
                fflush(out);
            }
        }
    }
    destroy(instance, NULL);
    FreeLibrary(module);
    return enumeration_result == VK_SUCCESS && count ? 0 : 6;
}

int main(int argc, char **argv) {
    out = fopen(argc > 1 ? argv[1] : "C:\\windows\\temp\\vp-texture-capabilities.tsv", "w");
    if (!out) return 2;
    fprintf(out, "texture_capabilities_probe=1 pid=%lu\n", GetCurrentProcessId());
    int result = query_vk();
    fprintf(out, "probe_complete=%u result=%d\n", result == 0, result);
    fclose(out);
    return result;
}
