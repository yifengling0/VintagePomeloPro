#include <vulkan/vulkan.h>

#include "harmony_gpu_frame_generation.h"
#include "gamescope/harmony_frame_generation.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace {

using winehua::graphics::FrameGenerationImage;
using winehua::graphics::HarmonyGpuFrameGeneration;

struct Scene {
    uint32_t version, size;
    uint64_t serial, color, depth;
    uint32_t colorFormat, colorLayout, depthFormat, depthLayout, width, height;
    float viewProjection[16], inverseViewProjection[16];
};

static_assert(sizeof(Scene) == 184);
static_assert(sizeof(FG_Mat4x4) == sizeof(Scene::viewProjection));

struct Session {
    explicit Session(VkDevice value) : device(value) {}

    ~Session()
    {
        generator.reset();
        ClearInputs();
        if (output.view) vkDestroyImageView(device, output.view, nullptr);
        if (output.image) vkDestroyImage(device, output.image, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
    }

    VkImageView View(VkImage image, VkFormat format, VkImageAspectFlags aspect) const
    {
        VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image = image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = format;
        info.subresourceRange = {aspect, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(device, &info, nullptr, &view);
        return view;
    }

    bool Start(const FG_ContextDescription_VK& context, uint32_t family, const Scene& scene,
               bool systemPresent, VkFormat presentFormat)
    {
        const auto capability = winehua::gamescope::ProbeHarmonyFrameGeneration();
        if (!capability.gpuVulkan.vulkanEntryPointsAvailable) {
            std::fprintf(stderr, "VPP_FG_WINE capability=%s\n", capability.gpuVulkan.stage.c_str());
            return false;
        }
        extent = {scene.width, scene.height};
        inputFormat = static_cast<VkFormat>(scene.colorFormat);
        output.format = presentFormat;
        depthFormat = static_cast<VkFormat>(scene.depthFormat);

        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = output.format;
        image.extent = {extent.width, extent.height, 1};
        image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (vkCreateImage(device, &image, nullptr, &output.image) != VK_SUCCESS) return false;

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, output.image, &requirements);
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(context.vkPhysicalDevice, &properties);
        uint32_t type = 0;
        for (; type < properties.memoryTypeCount; ++type) {
            if ((requirements.memoryTypeBits & (1u << type)) &&
                (properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                break;
        }
        if (type == properties.memoryTypeCount) return false;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        if (vkAllocateMemory(device, &allocation, nullptr, &memory) != VK_SUCCESS ||
            vkBindImageMemory(device, output.image, memory, 0) != VK_SUCCESS)
            return false;
        output.view = View(output.image, output.format, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!output.view) return false;

        VkCommandPoolCreateInfo commands{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        commands.queueFamilyIndex = family;
        commands.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        if (vkCreateCommandPool(device, &commands, nullptr, &pool) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo commandsAllocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandsAllocation.commandPool = pool;
        commandsAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandsAllocation.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &commandsAllocation, &command) != VK_SUCCESS) return false;
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) return false;

        generator.reset(new (std::nothrow) HarmonyGpuFrameGeneration());
        if (!generator) return false;
        const char* error = generator->Start(context, extent,
            {VK_NULL_HANDLE, VK_NULL_HANDLE, inputFormat},
            {VK_NULL_HANDLE, VK_NULL_HANDLE, depthFormat}, output, systemPresent);
        if (error) std::fprintf(stderr, "VPP_FG_WINE initialization=%s code=%d\n", error, generator->Error());
        return !error;
    }

    void ClearInputs()
    {
        if (generator) generator->ReleaseInputs();
        for (auto& view : inputViews) {
            if (view) vkDestroyImageView(device, view, nullptr);
            view = VK_NULL_HANDLE;
        }
    }

    bool Dispatch(VkQueue queue, const Scene& scene)
    {
        if (extent.width != scene.width || extent.height != scene.height ||
            inputFormat != static_cast<VkFormat>(scene.colorFormat) ||
            depthFormat != static_cast<VkFormat>(scene.depthFormat) ||
            (lastSerial && scene.serial != lastSerial + 1))
            return false;

        const auto color = (VkImage)scene.color;
        const auto depth = (VkImage)scene.depth;
        const auto colorLayout = static_cast<VkImageLayout>(scene.colorLayout);
        const auto depthLayout = static_cast<VkImageLayout>(scene.depthLayout);
        inputViews[0] = View(color, inputFormat, VK_IMAGE_ASPECT_COLOR_BIT);
        inputViews[1] = View(depth, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
        if (!inputViews[0] || !inputViews[1] ||
            !generator->BindInputs({color, inputViews[0], inputFormat}, {depth, inputViews[1], depthFormat}))
            return false;

        if (vkResetCommandPool(device, pool, 0) != VK_SUCCESS ||
            vkResetFences(device, 1, &fence) != VK_SUCCESS)
            return false;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) return false;
        FG_Mat4x4 view{}, inverse{};
        std::memcpy(&view, scene.viewProjection, sizeof(view));
        std::memcpy(&inverse, scene.inverseViewProjection, sizeof(inverse));
        const auto result = generator->Record(command, view, inverse, initialized, colorLayout, depthLayout);
        if (vkEndCommandBuffer(command) != VK_SUCCESS) return false;
        if (result != FG_SUCCESS && result != FG_COLLECTING_PREVIOUS_FRAMES) {
            std::fprintf(stderr, "VPP_FG_WINE dispatch_error=%d\n", int(result));
            return false;
        }

        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        // The opt-in proof dispatches on the game's graphics queue. The PE
        // command entry retains both input images until this submit retires.
        if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS ||
            vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
            return false;
        produced = result == FG_SUCCESS;
        initialized |= produced;
        lastSerial = scene.serial;
        if (scene.serial <= 24)
            std::fprintf(stderr, "VPP_FG_WINE sdk_complete serial=%llu status=%d\n",
                         static_cast<unsigned long long>(scene.serial), int(result));
        return true;
    }

    VkDevice device;
    VkExtent2D extent{};
    VkFormat inputFormat = VK_FORMAT_UNDEFINED;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    FrameGenerationImage output{};
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView inputViews[2]{};
    VkFence fence = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    std::unique_ptr<HarmonyGpuFrameGeneration> generator;
    uint64_t lastSerial = 0;
    bool initialized = false;
    bool produced = false;
};

} // namespace

extern "C" __attribute__((visibility("default")))
void VppHarmonyWineFrameDestroy(void* state)
{
    delete static_cast<Session*>(state);
}

extern "C" __attribute__((visibility("default")))
void VppHarmonyWineFramePresented(void* state)
{
    if (state) static_cast<Session*>(state)->ClearInputs();
}

static bool DispatchScene(void** state, VkInstance instance, VkPhysicalDevice physical,
                          VkDevice device, VkQueue queue, uint32_t family,
                          PFN_vkGetInstanceProcAddr loader, const void* payload, bool systemPresent,
                          VkFormat presentFormat)
{
    const auto& scene = *static_cast<const Scene*>(payload);
    auto* session = static_cast<Session*>(*state);
    if (session && session->Dispatch(queue, scene)) return true;
    const bool newSession = !session || (session->lastSerial && scene.serial != session->lastSerial + 1);
    delete session;
    *state = nullptr;
    if (!newSession) return false;

    std::unique_ptr<Session> next(new (std::nothrow) Session(device));
    FG_ContextDescription_VK context{instance, physical, device, 1, loader};
    if (!next || !next->Start(context, family, scene, systemPresent, presentFormat) ||
        !next->Dispatch(queue, scene))
        return false;
    *state = next.release();
    return true;
}

extern "C" __attribute__((visibility("default")))
bool VppHarmonyWineFrameDispatch(void** state, VkInstance instance, VkPhysicalDevice physical,
                                VkDevice device, VkQueue queue, uint32_t family,
                                PFN_vkGetInstanceProcAddr loader, const void* payload)
{
    return DispatchScene(state, instance, physical, device, queue, family, loader, payload, true,
                         static_cast<VkFormat>(static_cast<const Scene*>(payload)->colorFormat));
}

extern "C" __attribute__((visibility("default")))
VkImage VppHarmonyWineFrameDispatchGame(void** state, VkInstance instance, VkPhysicalDevice physical,
                                       VkDevice device, VkQueue queue, uint32_t family,
                                       uint32_t presentFormat, const void* payload)
{
    if (!DispatchScene(state, instance, physical, device, queue, family, vkGetInstanceProcAddr, payload,
                       false, static_cast<VkFormat>(presentFormat)))
        return VK_NULL_HANDLE;
    const auto* session = static_cast<Session*>(*state);
    VppHarmonyWineFramePresented(*state);
    return session->produced ? session->output.image : VK_NULL_HANDLE;
}
