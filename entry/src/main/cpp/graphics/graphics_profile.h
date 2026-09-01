#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace winehua {

// Normal product operation exposes two routes. Backend-specific compatibility
// stays in the Native policy/adapter and must not create another product
// profile id.
inline constexpr std::string_view kProductVirglRoute = "product-virgl";
inline constexpr std::string_view kProductVulkanRoute = "product-vulkan";

enum class D3dBackendKind {
    Unknown,
    WineD3d,
    DxvkLegacy,
    DxvkModern26,
    Vkd3dLimited500k,
};

enum class GpuUploadPolicy {
    Automatic,
    Disabled,
    Cpu,
};

struct HostGraphicsProfile {
    std::string_view name;
    std::string_view shadowMode;
    std::string_view shadowSelector;
    bool mergeShadowRanges = true;
    bool waitGpuUpload = false;
    bool serializeDescriptorUpdates = false;
    bool deferSharedMemoryUnref = false;
    bool perfSummary = false;
    GpuUploadPolicy gpuUpload = GpuUploadPolicy::Automatic;
};

struct DxvkRuntimeProfile {
    std::string_view directory;
    std::string_view version;
    // Runtime-specific behavior is expressed as narrow capabilities instead
    // of branching on a generation name at every call site.
    bool relaxedFeatureCompatibility = false;
    bool commandQueryReset = false;
    bool dynamicMappedFlush = false;
    bool batchMappedFlush = false;
    bool disableSemaphoreFeedback = false;
};

enum class GuestSubmissionPolicy {
    Default,
    SynchronousSubmit,
    StrongRingBarrier,
};

// Guest-side behavior derived from the same profile that configures the Host.
// Adapters serialize this policy; ArkTS must not rebuild it from profile names.
struct GuestGraphicsPolicy {
    bool preciseShadow = false;
    GuestSubmissionPolicy submission = GuestSubmissionPolicy::Default;
    bool perfSummary = false;
    bool directFenceWait = false;
    bool tracePresentImage = false;
    bool deferSharedMemoryUnref = false;
    bool disableGpuUpload = false;
    bool serializeDescriptorUpdates = false;
};

struct ProductGraphicsPolicy {
    std::string_view route;
    HostGraphicsProfile host;
    GuestGraphicsPolicy guest;
};

D3dBackendKind ParseD3dBackend(std::string_view backend);
bool IsDxvkBackend(D3dBackendKind backend);
bool UsesVenusPresent(D3dBackendKind backend);
// Resolves one of the two normal product routes. DXVK generations and the
// VKD3D DXGI companion may differ in adapter details without becoming routes.
bool ResolveProductGraphicsPolicy(D3dBackendKind backend,
                                  ProductGraphicsPolicy* policy);
// VKD3D uses DXVK 2.6.2's DXGI companion. WineD3D and unknown backends do
// not resolve a DXVK runtime.
bool ResolveDxvkRuntimeProfile(D3dBackendKind backend,
                               DxvkRuntimeProfile* profile);

// Resolves one of the deliberately small LAB experiment deltas on top of the
// selected product policy. Experiments are observations or single-variable
// isolations; they are not additional product routes.
bool ResolveLabGraphicsExperiment(std::string_view id,
                                  D3dBackendKind backend,
                                  ProductGraphicsPolicy* policy);

// Serializes the common Guest policy for the selected DXVK generation. The
// legacy-only mapped-flush switch is omitted for DXVK 2.6/VKD3D companion
// sessions so modern runtimes remain free of legacy compatibility knobs.
bool BuildLabGuestGraphicsEnvironment(std::string_view name,
                                      D3dBackendKind backend,
                                      std::vector<std::string>* environment);

// Serializes the Guest half of a normal product route. This keeps product
// operation independent from the historical LAB profile registry.
bool BuildProductGuestGraphicsEnvironment(
    D3dBackendKind backend, std::vector<std::string>* environment);

} // namespace winehua
