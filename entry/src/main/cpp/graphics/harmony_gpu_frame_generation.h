// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <dlfcn.h>
#include <initializer_list>
#include <graphics_game_sdk/frame_generation_vk.h>

namespace winehua::graphics {

struct FrameGenerationImage {
    VkImage image;
    VkImageView view;
    VkFormat format;
};

// The producer keeps the color and depth images alive on this same device.
// Stop or replace the scene after retiring the recorded GPU work.
class HarmonyGpuFrameGeneration {
public:
    HarmonyGpuFrameGeneration() = default;
    HarmonyGpuFrameGeneration(const HarmonyGpuFrameGeneration&) = delete;
    HarmonyGpuFrameGeneration& operator=(const HarmonyGpuFrameGeneration&) = delete;

    ~HarmonyGpuFrameGeneration()
    {
        if (active_) deactivate_(context_);
        for (FG_Image_VK* image : images_) {
            if (image) destroyImage_(context_, image);
        }
        if (context_) destroyContext_(&context_);
        if (library_) dlclose(library_);
    }

    const char* Start(const FG_ContextDescription_VK& device, VkExtent2D extent,
                      const FrameGenerationImage& color, const FrameGenerationImage& depth,
                      const FrameGenerationImage& prediction)
    {
        library_ = dlopen("libframegeneration.so", RTLD_NOW | RTLD_LOCAL);
        if (!library_) return "libframegeneration.so unavailable";
        if (!Load(createContext_, "HMS_FG_CreateContext_VK") ||
            !Load(destroyContext_, "HMS_FG_DestroyContext_VK") ||
            !Load(setAlgorithm_, "HMS_FG_SetAlgorithmMode_VK") ||
            !Load(setResolution_, "HMS_FG_SetResolution_VK") ||
            !Load(setFormat_, "HMS_FG_SetImageFormat_VK") ||
            !Load(activate_, "HMS_FG_Activate_VK") ||
            !Load(deactivate_, "HMS_FG_Deactivate_VK") ||
            !Load(createImage_, "HMS_FG_CreateImage_VK") ||
            !Load(destroyImage_, "HMS_FG_DestroyImage_VK") ||
            !Load(dispatch_, "HMS_FG_Dispatch_VK")) {
            return "Vulkan frame generation entry point unavailable";
        }
        context_ = createContext_(&device);
        if (!context_) return "HMS_FG_CreateContext_VK failed";
        FG_AlgorithmModeInfo algorithm{};
        algorithm.predictionMode = FG_PREDICTION_MODE_INTERPOLATION;
        algorithm.meMode = FG_ME_MODE_BASIC;
        error_ = setAlgorithm_(context_, &algorithm);
        if (error_ != FG_SUCCESS) return "HMS_FG_SetAlgorithmMode_VK failed";
        const FG_Dimension2D size{extent.width, extent.height};
        const FG_ResolutionInfo resolution{size, size, size};
        error_ = setResolution_(context_, &resolution);
        if (error_ != FG_SUCCESS) return "HMS_FG_SetResolution_VK failed";
        const FG_ImageFormat_VK formats{color.format, depth.format, prediction.format};
        error_ = setFormat_(context_, &formats);
        if (error_ != FG_SUCCESS) return "HMS_FG_SetImageFormat_VK failed";
        error_ = activate_(context_);
        if (error_ != FG_SUCCESS) return "HMS_FG_Activate_VK failed";
        active_ = true;
        unsigned index = 0;
        for (const auto* image : {&color, &depth, &prediction}) {
            images_[index] = createImage_(context_, image->image, image->view);
            if (!images_[index++]) return "HMS_FG_CreateImage_VK failed";
        }
        return nullptr;
    }

    FG_ErrorCode Record(VkCommandBuffer command, const FG_Mat4x4& camera,
                        const FG_Mat4x4& inverseCamera, bool predictionInitialized)
    {
        const FG_ImageSync_VK sample{VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT};
        const FG_ImageSync_VK undefined{static_cast<VkAccessFlagBits>(0), VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT};
        FG_DispatchDescription_VK frame{};
        frame.inputColorInfo = {images_[0], sample, sample};
        frame.inputDepthStencilInfo = {images_[1], sample, sample};
        frame.outputColorInfo = {images_[2], predictionInitialized ? sample : undefined, sample};
        frame.viewProj = camera;
        frame.invViewProj = inverseCamera;
        frame.vkCommandBuffer = command;
        return dispatch_(context_, &frame);
    }

    int Error() const { return error_; }

private:
    template <typename Fn>
    bool Load(Fn& fn, const char* name)
    {
        fn = reinterpret_cast<Fn>(dlsym(library_, name));
        return fn != nullptr;
    }

    void* library_ = nullptr;
    FG_Context_VK* context_ = nullptr;
    FG_Image_VK* images_[3]{};
    bool active_ = false;
    FG_ErrorCode error_ = FG_SUCCESS;
    decltype(&HMS_FG_CreateContext_VK) createContext_ = nullptr;
    decltype(&HMS_FG_DestroyContext_VK) destroyContext_ = nullptr;
    decltype(&HMS_FG_SetAlgorithmMode_VK) setAlgorithm_ = nullptr;
    decltype(&HMS_FG_SetResolution_VK) setResolution_ = nullptr;
    decltype(&HMS_FG_SetImageFormat_VK) setFormat_ = nullptr;
    decltype(&HMS_FG_Activate_VK) activate_ = nullptr;
    decltype(&HMS_FG_Deactivate_VK) deactivate_ = nullptr;
    decltype(&HMS_FG_CreateImage_VK) createImage_ = nullptr;
    decltype(&HMS_FG_DestroyImage_VK) destroyImage_ = nullptr;
    decltype(&HMS_FG_Dispatch_VK) dispatch_ = nullptr;
};

} // namespace winehua::graphics
