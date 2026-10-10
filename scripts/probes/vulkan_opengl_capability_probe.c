/* Read-only Vulkan capability capture for desktop OpenGL/Zink qualification.
 * Works as an OHOS executable or a Wine PE32/PE64 probe. No device is created,
 * no features are enabled, and this result is not a GL conformance claim. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static void string(FILE *out, const char *value)
{
    fputc('"', out);
    for (; *value; ++value) {
        unsigned char c = (unsigned char)*value;
        if (c == '"' || c == '\\') fputc('\\', out);
        if (c < 32) fprintf(out, "\\u%04x", c); else fputc(c, out);
    }
    fputc('"', out);
}

static int has_extension(const VkExtensionProperties *ext, uint32_t count, const char *name)
{
    uint32_t i;
    for (i = 0; i < count; ++i) if (!strcmp(ext[i].extensionName, name)) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkCreateInstance create;
    PFN_vkEnumerateInstanceVersion version;
    PFN_vkEnumeratePhysicalDevices enumerate;
    PFN_vkDestroyInstance destroy;
    PFN_vkGetPhysicalDeviceProperties properties;
    PFN_vkGetPhysicalDeviceFeatures features;
    PFN_vkGetPhysicalDeviceFeatures2 features2;
    PFN_vkEnumerateDeviceExtensionProperties extensions;
    VkInstance instance;
    VkPhysicalDevice *devices;
    uint32_t loader_version = VK_API_VERSION_1_0, count = 0, i;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    FILE *out;
#ifdef _WIN32
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    gipa = lib ? (PFN_vkGetInstanceProcAddr)GetProcAddress(lib, "vkGetInstanceProcAddr") : NULL;
#else
    void *lib = dlopen(argc > 2 ? argv[2] : "libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    gipa = lib ? (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr") : NULL;
#endif
    if (!gipa) { fprintf(stderr, "Vulkan loader unavailable\n"); return 2; }
    create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    version = (PFN_vkEnumerateInstanceVersion)gipa(NULL, "vkEnumerateInstanceVersion");
    if (!create || (version && version(&loader_version) != VK_SUCCESS)) return 3;
    app.pApplicationName = "VP OpenGL capability capture";
    app.apiVersion = loader_version < VK_API_VERSION_1_3 ? loader_version : VK_API_VERSION_1_3;
    ci.pApplicationInfo = &app;
    if (create(&ci, NULL, &instance) != VK_SUCCESS) return 4;
#define LOAD(variable, name) variable = (PFN_##name)gipa(instance, #name)
    LOAD(enumerate, vkEnumeratePhysicalDevices);
    LOAD(destroy, vkDestroyInstance);
    LOAD(properties, vkGetPhysicalDeviceProperties);
    LOAD(features, vkGetPhysicalDeviceFeatures);
    LOAD(features2, vkGetPhysicalDeviceFeatures2);
    LOAD(extensions, vkEnumerateDeviceExtensionProperties);
    if (!enumerate || !destroy || !properties || !features || !extensions) return 5;
    if (enumerate(instance, &count, NULL) != VK_SUCCESS || !count) return 6;
    devices = calloc(count, sizeof(*devices));
    if (!devices || enumerate(instance, &count, devices) != VK_SUCCESS) return 7;
    out = argc > 1 ? fopen(argv[1], "w") : stdout;
    if (!out) return 8;
    fprintf(out, "{\"schemaVersion\":1,\"queryOnly\":true,\"pointerBits\":%u,\"loaderApi\":%u,\"devices\":[", (unsigned)(8*sizeof(void*)), loader_version);
    for (i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties p;
        VkPhysicalDeviceFeatures f;
        VkExtensionProperties *ext;
        uint32_t ec = 0, n;
        VkPhysicalDeviceVulkan12Features v12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceConditionalRenderingFeaturesEXT cond = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT};
        VkPhysicalDeviceTransformFeedbackFeaturesEXT tf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
        VkPhysicalDeviceDepthClipEnableFeaturesEXT dc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
        VkPhysicalDeviceCustomBorderColorFeaturesEXT bc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
        VkPhysicalDeviceLineRasterizationFeaturesEXT line = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        properties(devices[i], &p);
        features(devices[i], &f);
        if (extensions(devices[i], NULL, &ec, NULL) != VK_SUCCESS) return 9;
        ext = calloc(ec ? ec : 1, sizeof(*ext));
        if (!ext || extensions(devices[i], NULL, &ec, ext) != VK_SUCCESS) return 9;
        if (features2 && app.apiVersion >= VK_API_VERSION_1_1) {
#define CHAIN(value) do { value.pNext = f2.pNext; f2.pNext = &value; } while (0)
            if (p.apiVersion >= VK_API_VERSION_1_2 && app.apiVersion >= VK_API_VERSION_1_2) CHAIN(v12);
            if (has_extension(ext, ec, "VK_EXT_conditional_rendering")) CHAIN(cond);
            if (has_extension(ext, ec, "VK_EXT_transform_feedback")) CHAIN(tf);
            if (has_extension(ext, ec, "VK_EXT_depth_clip_enable")) CHAIN(dc);
            if (has_extension(ext, ec, "VK_EXT_custom_border_color")) CHAIN(bc);
            if (has_extension(ext, ec, "VK_EXT_line_rasterization")) CHAIN(line);
            features2(devices[i], &f2);
        }
        fprintf(out, "%s{\"name\":", i ? "," : ""); string(out, p.deviceName);
        fprintf(out, ",\"api\":%u,\"features\":{", p.apiVersion);
#define FEATURE(name) fprintf(out, "\"%s\":%s,", #name, f.name ? "true" : "false")
        FEATURE(robustBufferAccess); FEATURE(logicOp); FEATURE(fillModeNonSolid);
        FEATURE(alphaToOne); FEATURE(shaderClipDistance); FEATURE(independentBlend);
        FEATURE(depthClamp); FEATURE(geometryShader); FEATURE(shaderTessellationAndGeometryPointSize);
        FEATURE(dualSrcBlend); FEATURE(sampleRateShading); FEATURE(tessellationShader);
        FEATURE(imageCubeArray); FEATURE(multiViewport); FEATURE(shaderFloat64);
        FEATURE(shaderInt64); FEATURE(multiDrawIndirect); FEATURE(samplerAnisotropy);
        FEATURE(depthBiasClamp); FEATURE(shaderCullDistance); FEATURE(shaderStorageImageExtendedFormats);
        FEATURE(shaderStorageImageWriteWithoutFormat); FEATURE(vertexPipelineStoresAndAtomics);
        FEATURE(fragmentStoresAndAtomics);
#define EXTRA(value, name) fprintf(out, "\"%s\":%s,", #name, value.name ? "true" : "false")
        EXTRA(v12, scalarBlockLayout); EXTRA(v12, timelineSemaphore); EXTRA(v12, samplerMirrorClampToEdge);
        EXTRA(v12, drawIndirectCount); EXTRA(cond, conditionalRendering); EXTRA(tf, transformFeedback);
        EXTRA(dc, depthClipEnable); EXTRA(bc, customBorderColorWithoutFormat);
        EXTRA(line, rectangularLines); EXTRA(line, bresenhamLines);
        fprintf(out, "\"smoothLines\":%s},\"limits\":{", line.smoothLines ? "true" : "false");
#define LIMIT(name) fprintf(out, "\"%s\":%u,", #name, p.limits.name)
        LIMIT(maxPerStageDescriptorSamplers); LIMIT(maxImageDimension1D); LIMIT(maxImageDimension2D);
        LIMIT(maxImageDimension3D); LIMIT(maxImageDimensionCube); LIMIT(maxImageArrayLayers);
        LIMIT(maxViewports); LIMIT(maxUniformBufferRange); LIMIT(maxPerStageDescriptorUniformBuffers);
        fprintf(out, "\"maxBoundDescriptorSets\":%u},\"extensions\":{", p.limits.maxBoundDescriptorSets);
        for (n = 0; n < ec; ++n) {
            if (n) fputc(',', out);
            string(out, ext[n].extensionName); fprintf(out, ":%u", ext[n].specVersion);
        }
        fprintf(out, "}}");
        free(ext);
    }
    fprintf(out, "]}\n");
    if (out != stdout) fclose(out);
    free(devices);
    destroy(instance, NULL);
    return 0;
}
