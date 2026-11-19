// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <dlfcn.h>
#include <initializer_list>
#include <string>

namespace winehua::gamescope {
struct HarmonyFrameGenerationModeCapability {
    bool vulkanEntryPointsAvailable = false;
    std::string deviceSupport = "unknown";
    std::string stage = "library-unavailable";
    std::string detail;
};

struct HarmonyFrameGenerationCapability {
    bool libraryLoaded = false;
    bool supportQueryAvailable = false;
    HarmonyFrameGenerationModeCapability gpuVulkan;
    HarmonyFrameGenerationModeCapability aiVulkan;
};

// Query in the application's linker namespace, off the UI thread.
// No hard dependency on SDK 26: existing API 23 systems must still load VPP.
// A positive query qualifies the candidate only, never its output or FPS.
inline HarmonyFrameGenerationCapability ProbeHarmonyFrameGeneration(
    const char* libraryName = "libframegeneration.so")
{
    HarmonyFrameGenerationCapability result;
    void* library = dlopen(libraryName, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        const char* error = dlerror();
        result.gpuVulkan.detail = error ? error : "System frame generation library unavailable";
        result.aiVulkan.detail = result.gpuVulkan.detail;
        return result;
    }
    struct CloseLibrary {
        void* handle;
        ~CloseLibrary() { dlclose(handle); }
    } close{library};
    result.libraryLoaded = true;
    // Public C ABI: bool HMS_FG_IsFrameGenerationSupported(FG_FeatureType).
    // FG_FeatureType is a 32-bit enum. These values come from the SDK 26 header.
    constexpr int InterpolationGpuVulkan = 1;
    constexpr int InterpolationAiVulkan = 4;
    using Query = bool (*)(int);
    const auto query = reinterpret_cast<Query>(dlsym(library, "HMS_FG_IsFrameGenerationSupported"));
    result.supportQueryAvailable = query != nullptr;
    const char* commonEntryPoints[] = {
        "HMS_FG_CreateContext_VK", "HMS_FG_DestroyContext_VK",
        "HMS_FG_SetAlgorithmMode_VK", "HMS_FG_SetResolution_VK",
        "HMS_FG_SetImageFormat_VK",
        "HMS_FG_Activate_VK", "HMS_FG_Deactivate_VK",
        "HMS_FG_CreateImage_VK", "HMS_FG_DestroyImage_VK", "HMS_FG_Dispatch_VK"
    };
    auto probeMode = [&](int feature, std::initializer_list<const char*> additionalEntryPoints) {
        HarmonyFrameGenerationModeCapability mode;
        if (query) {
            if (!query(feature)) {
                mode.deviceSupport = "unsupported";
                mode.stage = "device-unsupported";
                mode.detail = "System capability query returned false";
                return mode;
            }
            mode.deviceSupport = "supported";
        }
        mode.stage = "vulkan-entry-point-unavailable";
        for (const char* symbol : commonEntryPoints) {
            if (!dlsym(library, symbol)) {
                mode.detail = symbol;
                return mode;
            }
        }
        for (const char* symbol : additionalEntryPoints) {
            if (!dlsym(library, symbol)) {
                mode.detail = symbol;
                return mode;
            }
        }
        mode.vulkanEntryPointsAvailable = true;
        mode.stage = query ? "candidate-available" : "runtime-check-required";
        mode.detail = query ? "Device query and Vulkan entry points verified" :
            "GPU Vulkan entry points verified; use a test scene to check device support";
        return mode;
    };
    result.gpuVulkan = probeMode(InterpolationGpuVulkan, {});
    // AI interpolation requires system presentation. GPU interpolation can be game-presented.
    if (query) {
        result.aiVulkan = probeMode(InterpolationAiVulkan, {"HMS_FG_SetIntegrationMode_VK"});
    } else {
        // The original GPU APIs are available on some systems before the SDK
        // 26 capability query. AI uses a distinct capability and present path.
        result.aiVulkan.stage = "support-query-unavailable";
        result.aiVulkan.detail = "AI capability query requires a system exposing the SDK 26 API";
    }
    return result;
}
}
