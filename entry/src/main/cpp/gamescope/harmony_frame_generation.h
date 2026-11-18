// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <dlfcn.h>
#include <string>

namespace winehua::gamescope {
struct HarmonyFrameGenerationCapability {
    bool libraryLoaded = false;
    bool supportQueryAvailable = false;
    bool vulkanEntryPointsAvailable = false;
    std::string deviceSupport = "unknown";
    std::string stage = "library-unavailable";
    std::string detail;
};

// Query in the application's linker namespace, off the UI/render threads.
// No hard dependency on SDK 26: existing API 23 systems must still load VPP.
// A positive query qualifies the candidate only, never its output or FPS.
inline HarmonyFrameGenerationCapability ProbeHarmonyFrameGeneration(
    const char* libraryName = "libframegeneration.so")
{
    HarmonyFrameGenerationCapability result;
    void* library = dlopen(libraryName, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        const char* error = dlerror();
        result.detail = error ? error : "System frame generation library unavailable";
        return result;
    }
    struct CloseLibrary {
        void* handle;
        ~CloseLibrary() { dlclose(handle); }
    } close{library};
    result.libraryLoaded = true;
    // Public C ABI: bool HMS_FG_IsFrameGenerationSupported(FG_FeatureType).
    // FG_FeatureType is a 32-bit enum; INTERPOLATION_AI_VULKAN = 4 (SDK 26).
    using Query = bool (*)(int);
    const auto query = reinterpret_cast<Query>(dlsym(library, "HMS_FG_IsFrameGenerationSupported"));
    if (!query) {
        result.stage = "support-query-unavailable";
        result.detail = "AI capability query requires a system exposing the SDK 26 API";
        return result;
    }
    result.supportQueryAvailable = true;
    const bool supported = query(4);
    result.deviceSupport = supported ? "supported" : "unsupported";
    if (!supported) {
        result.stage = "device-unsupported";
        result.detail = "System AI Vulkan frame generation capability query returned false";
        return result;
    }
    for (const char* symbol : {
        "HMS_FG_CreateContext_VK", "HMS_FG_DestroyContext_VK",
        "HMS_FG_SetAlgorithmMode_VK", "HMS_FG_SetResolution_VK",
        "HMS_FG_SetImageFormat_VK", "HMS_FG_SetIntegrationMode_VK",
        "HMS_FG_Activate_VK", "HMS_FG_Deactivate_VK",
        "HMS_FG_CreateImage_VK", "HMS_FG_DestroyImage_VK", "HMS_FG_Dispatch_VK"
    }) {
        if (!dlsym(library, symbol)) {
            result.stage = "vulkan-entry-point-unavailable";
            result.detail = symbol;
            return result;
        }
    }
    result.vulkanEntryPointsAvailable = true;
    result.stage = "candidate-available";
    result.detail = "Device query passed; Gamescope camera input and system presentation still require validation";
    return result;
}
}
