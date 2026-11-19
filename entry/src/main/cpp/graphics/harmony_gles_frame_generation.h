#pragma once

#include "harmony_frame_policy.h"

#include <GLES3/gl31.h>
#include <graphics_game_sdk/frame_generation_gles.h>

#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <type_traits>

namespace winehua::graphics {

// ABI of virgl_renderer_winehua_take_gl_scene. The renderer retains the depth
// surface until the next producer command, including the present callback.
struct HarmonyGlesFrameScene {
    uint32_t request, width, height, clipZeroToOne, depthTexture;
    float viewProjection[16], inverseViewProjection[16];
};
static_assert(sizeof(HarmonyGlesFrameScene) == 148);

inline HarmonyGlesFrameScene TakeHarmonyGlesFrameScene()
{
    using TakeScene = int (*)(HarmonyGlesFrameScene*, unsigned);
    static const auto takeScene = [] {
        void* renderer = dlopen("libvirglrenderer.so.1", RTLD_NOW | RTLD_NOLOAD);
        if (!renderer) return static_cast<TakeScene>(nullptr);
        const auto take = reinterpret_cast<TakeScene>(dlsym(renderer, "virgl_renderer_winehua_take_gl_scene"));
        dlclose(renderer);
        return take;
    }();
    HarmonyGlesFrameScene scene{};
    if (takeScene) takeScene(&scene, sizeof(scene));
    return scene;
}

class HarmonyGlesFrameGeneration {
public:
    void Reset()
    {
        if (context_) {
            glFinish();
            deactivate_(context_);
            destroy_(&context_);
            context_ = nullptr;
        }
        if (output_) glDeleteTextures(1, &output_);
        output_ = 0;
        if (library_) dlclose(library_);
        library_ = nullptr;
        request_ = width_ = height_ = clipZeroToOne_ = 0;
        failed_ = false;
    }

    GLuint Generate(const HarmonyGlesFrameScene& scene, GLuint color, uint32_t& status)
    {
        if (request_ != scene.request || width_ != scene.width ||
            height_ != scene.height || clipZeroToOne_ != scene.clipZeroToOne) {
            Reset();
            request_ = scene.request;
            width_ = scene.width;
            height_ = scene.height;
            clipZeroToOne_ = scene.clipZeroToOne;
        }
        if (failed_) {
            status = 6;
            return 0;
        }
        if (!CompatibleTexture(color, false) || !CompatibleTexture(scene.depthTexture, true)) {
            status = 10;
            return 0;
        }
        for (unsigned i = 0; i < 16; i++) {
            if (!std::isfinite(scene.viewProjection[i]) || !std::isfinite(scene.inverseViewProjection[i])) {
                status = 10;
                return 0;
            }
        }
        if (!context_ && !Start()) {
            failed_ = true;
            status = 6;
            return 0;
        }

        FG_DispatchDescription_GLES description{};
        description.inputColor = color;
        description.inputDepthStencil = scene.depthTexture;
        description.outputColor = output_;
        std::memcpy(&description.viewProj, scene.viewProjection, sizeof(description.viewProj));
        std::memcpy(&description.invViewProj, scene.inverseViewProjection, sizeof(description.invViewProj));
        const auto result = dispatch_(context_, &description);
        if (result == FG_SUCCESS) return output_;
        status = result == FG_COLLECTING_PREVIOUS_FRAMES ? 1 : 6;
        failed_ = result != FG_COLLECTING_PREVIOUS_FRAMES;
        return 0;
    }

private:
    bool CompatibleTexture(GLuint texture, bool depth) const
    {
        if (!texture || glIsTexture(texture) != GL_TRUE) return false;
        glBindTexture(GL_TEXTURE_2D, texture);
        GLint width = 0, height = 0, format = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
        return glGetError() == GL_NO_ERROR &&
            width == static_cast<GLint>(width_) && height == static_cast<GLint>(height_) &&
            (depth ? format == GL_DEPTH24_STENCIL8 || format == GL_DEPTH32F_STENCIL8 : format == GL_RGBA8);
    }

    bool Start()
    {
        library_ = dlopen("libframegeneration.so", RTLD_NOW | RTLD_LOCAL);
        if (!library_) return false;
        const auto load = [this](auto& fn, const char* name) {
            fn = reinterpret_cast<std::decay_t<decltype(fn)>>(dlsym(library_, name));
            return fn != nullptr;
        };
        using QuerySupport = bool (*)(int);
        constexpr int GpuGlesInterpolation = 0;
        QuerySupport supported = nullptr;
        decltype(&HMS_FG_CreateContext_GLES) create = nullptr;
        decltype(&HMS_FG_SetAlgorithmMode_GLES) algorithm = nullptr;
        decltype(&HMS_FG_SetResolution_GLES) resolution = nullptr;
        decltype(&HMS_FG_SetCvvZSemantic_GLES) clipDepth = nullptr;
        decltype(&HMS_FG_Activate_GLES) activate = nullptr;
        if ((load(supported, "HMS_FG_IsFrameGenerationSupported") && !supported(GpuGlesInterpolation)) ||
            !load(create, "HMS_FG_CreateContext_GLES") ||
            !load(destroy_, "HMS_FG_DestroyContext_GLES") ||
            !load(algorithm, "HMS_FG_SetAlgorithmMode_GLES") ||
            !load(resolution, "HMS_FG_SetResolution_GLES") ||
            !load(clipDepth, "HMS_FG_SetCvvZSemantic_GLES") ||
            !load(activate, "HMS_FG_Activate_GLES") ||
            !load(deactivate_, "HMS_FG_Deactivate_GLES") ||
            !load(dispatch_, "HMS_FG_Dispatch_GLES"))
            return false;
        FG_AlgorithmModeInfo mode{};
        mode.predictionMode = FG_PREDICTION_MODE_INTERPOLATION;
        mode.meMode = FG_ME_MODE_BASIC;
        const FG_Dimension2D size{width_, height_};
        const FG_ResolutionInfo dimensions{size, size, size};
        const auto semantic = clipZeroToOne_ ? FG_CVV_Z_SEMANTIC_ZERO_TO_ONE_FORWARD_Z :
            FG_CVV_Z_SEMANTIC_MINUS_ONE_TO_ONE_FORWARD_Z;
        context_ = create();
        if (!context_) return false;
        if (algorithm(context_, &mode) != FG_SUCCESS ||
            resolution(context_, &dimensions) != FG_SUCCESS ||
            clipDepth(context_, semantic) != FG_SUCCESS ||
            activate(context_) != FG_SUCCESS) {
            destroy_(&context_);
            context_ = nullptr;
            return false;
        }
        glGenTextures(1, &output_);
        glBindTexture(GL_TEXTURE_2D, output_);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width_, height_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        return output_ && glGetError() == GL_NO_ERROR;
    }

    void* library_ = nullptr;
    FG_Context_GLES* context_ = nullptr;
    decltype(&HMS_FG_Deactivate_GLES) deactivate_ = nullptr;
    decltype(&HMS_FG_DestroyContext_GLES) destroy_ = nullptr;
    decltype(&HMS_FG_Dispatch_GLES) dispatch_ = nullptr;
    GLuint output_ = 0;
    uint32_t request_ = 0, width_ = 0, height_ = 0, clipZeroToOne_ = 0;
    bool failed_ = false;
};

} // namespace winehua::graphics
