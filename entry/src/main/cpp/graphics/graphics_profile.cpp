#include "graphics/graphics_profile.h"

#include <utility>

namespace winehua {
namespace {

constexpr std::string_view kObserveProductSummary = "observe-product-summary";
constexpr std::string_view kObserveFrameTimeline = "observe-frame-timeline";
constexpr std::string_view kTraceFrameAssociation = "trace-frame-association";
constexpr std::string_view kTracePresentImage = "trace-present-image";
constexpr std::string_view kIsolateTransportNeutral = "isolate-transport-neutral";

void AppendBooleanEnvironment(std::vector<std::string>* environment,
                              std::string_view key, bool enabled)
{
    environment->emplace_back(std::string(key) + (enabled ? "=1" : "=0"));
}

bool SerializeGuestGraphicsEnvironment(
    std::string_view policyName, const GuestGraphicsPolicy& policy,
    D3dBackendKind backend, std::vector<std::string>* environment)
{
    if (!environment) return false;
    environment->clear();
    DxvkRuntimeProfile runtime;
    if (!ResolveDxvkRuntimeProfile(backend, &runtime)) return false;

    environment->emplace_back("WINEHUA_GRAPHICS_PROFILE=" +
                              std::string(policyName));
    AppendBooleanEnvironment(environment, "DXVK_WINEHUA_PRECISE_SHADOW",
                             policy.preciseShadow);
    AppendBooleanEnvironment(environment, "VN_WINEHUA_STRONG_RING_BARRIER",
                             policy.submission ==
                                 GuestSubmissionPolicy::StrongRingBarrier);
    AppendBooleanEnvironment(environment,
                             "VKR_WINEHUA_DESCRIPTOR_UPDATE_SERIALIZE",
                             policy.serializeDescriptorUpdates);
    AppendBooleanEnvironment(environment, "VN_WINEHUA_PERF_SUMMARY",
                             policy.perfSummary);
    AppendBooleanEnvironment(environment, "VN_WINEHUA_DIRECT_FENCE_WAIT",
                             policy.directFenceWait);
    AppendBooleanEnvironment(environment, "WINEHUA_DXVK_TRACE_PRESENT_IMAGE",
                             policy.tracePresentImage);
    AppendBooleanEnvironment(environment, "VN_WINEHUA_DEFER_SHMEM_UNREF",
                             policy.deferSharedMemoryUnref);

    AppendBooleanEnvironment(environment, "VN_WINEHUA_REMOTE_MEMORY_SYNC",
                             policy.preciseShadow);
    if (runtime.dynamicMappedFlush)
        AppendBooleanEnvironment(environment,
                                 "DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED",
                                 policy.preciseShadow);

    std::string vnPerf = "VN_PERF=no_fence_feedback,no_query_feedback";
    if (runtime.disableSemaphoreFeedback)
        vnPerf += ",no_semaphore_feedback";
    switch (policy.submission) {
    case GuestSubmissionPolicy::SynchronousSubmit:
        vnPerf += ",no_async_queue_submit";
        break;
    case GuestSubmissionPolicy::Default:
    case GuestSubmissionPolicy::StrongRingBarrier:
        vnPerf += ",no_multi_ring";
        break;
    }
    environment->emplace_back(std::move(vnPerf));
    if (policy.disableGpuUpload)
        environment->emplace_back("VKR_WINEHUA_GPU_UPLOAD=0");
    return true;
}

} // namespace

D3dBackendKind ParseD3dBackend(std::string_view backend)
{
    if (backend == "wined3d") return D3dBackendKind::WineD3d;
    if (backend == "dxvk_legacy") return D3dBackendKind::DxvkLegacy;
    if (backend == "dxvk_modern_2_6") return D3dBackendKind::DxvkModern26;
    if (backend == "vkd3d_limited_500k") return D3dBackendKind::Vkd3dLimited500k;
    return D3dBackendKind::Unknown;
}

bool IsDxvkBackend(D3dBackendKind backend)
{
    return backend == D3dBackendKind::DxvkLegacy ||
        backend == D3dBackendKind::DxvkModern26;
}

bool UsesVenusPresent(D3dBackendKind backend)
{
    return IsDxvkBackend(backend) || backend == D3dBackendKind::Vkd3dLimited500k;
}

bool ResolveProductGraphicsPolicy(D3dBackendKind backend,
                                  ProductGraphicsPolicy* policy)
{
    if (!policy) return false;
    ProductGraphicsPolicy resolved;
    switch (backend) {
    case D3dBackendKind::WineD3d: {
        resolved.route = kProductVirglRoute;
        resolved.host = {resolved.route, "full", "0"};
        break;
    }
    case D3dBackendKind::DxvkLegacy:
    case D3dBackendKind::DxvkModern26: {
        resolved.route = kProductVulkanRoute;
        resolved.host = {resolved.route, "precise-dirty",
                         "inline-gpu-upload-coverage-sort"};
        resolved.guest.preciseShadow = true;
        resolved.guest.submission =
            GuestSubmissionPolicy::StrongRingBarrier;
        break;
    }
    case D3dBackendKind::Vkd3dLimited500k: {
        resolved.route = kProductVulkanRoute;
        resolved.host = {resolved.route, "precise", "0"};
        resolved.guest.preciseShadow = true;
        resolved.guest.submission =
            GuestSubmissionPolicy::StrongRingBarrier;
        resolved.guest.directFenceWait = true;
        break;
    }
    case D3dBackendKind::Unknown:
        return false;
    }
    *policy = resolved;
    return true;
}

bool ResolveDxvkRuntimeProfile(D3dBackendKind backend,
                               DxvkRuntimeProfile* profile)
{
    if (!profile) return false;
    switch (backend) {
    case D3dBackendKind::DxvkLegacy:
        *profile = {
            "legacy", "1.10.3",
            true,  // relaxedFeatureCompatibility
            true,  // commandQueryReset
            true,  // dynamicMappedFlush
            true,  // batchMappedFlush
            // Product DXVK sessions also expose VKD3D-Proton's D3D12
            // companion. Venus semaphore feedback can stall that vtest path
            // even when legacy DXVK supplies d3d11/dxgi.
            true,  // disableSemaphoreFeedback
        };
        return true;
    case D3dBackendKind::DxvkModern26:
    case D3dBackendKind::Vkd3dLimited500k:
        *profile = {
            "modern-2.6", "2.6.2",
            false, // relaxedFeatureCompatibility
            false, // commandQueryReset
            false, // dynamicMappedFlush
            true,  // batchMappedFlush
            true,  // disableSemaphoreFeedback
        };
        return true;
    case D3dBackendKind::WineD3d:
    case D3dBackendKind::Unknown:
        return false;
    }
    return false;
}

bool ResolveLabGraphicsExperiment(std::string_view id,
                                  D3dBackendKind backend,
                                  ProductGraphicsPolicy* policy)
{
    if (!policy || backend == D3dBackendKind::Unknown) return false;

    ProductGraphicsPolicy resolved;
    if (!ResolveProductGraphicsPolicy(backend, &resolved)) return false;
    const bool venusPresent = UsesVenusPresent(backend);

    std::string_view canonicalId;
    if (id == kObserveProductSummary) {
        canonicalId = kObserveProductSummary;
        resolved.host.perfSummary = true;
        resolved.guest.perfSummary = true;
    } else if (id == kObserveFrameTimeline) {
        canonicalId = kObserveFrameTimeline;
        resolved.host.shadowSelector = "frame-timeline";
        resolved.host.perfSummary = true;
    } else if (id == kTraceFrameAssociation) {
        if (!venusPresent) return false;
        canonicalId = kTraceFrameAssociation;
        resolved.host.shadowSelector = "inline-gpu-upload-frame-assoc-trace";
        resolved.host.perfSummary = true;
    } else if (id == kTracePresentImage) {
        if (!venusPresent) return false;
        canonicalId = kTracePresentImage;
        resolved.host.shadowSelector = "present-image-trace";
        resolved.guest.tracePresentImage = true;
    } else if (id == kIsolateTransportNeutral) {
        if (!venusPresent) return false;
        canonicalId = kIsolateTransportNeutral;
        // A deliberate negative control: retain the selected DXVK runtime but
        // remove WineHua's precise-shadow and ring synchronization policy.
        resolved.host = {canonicalId, "full", "0"};
        resolved.guest = {};
    } else {
        return false;
    }

    resolved.host.name = canonicalId;
    *policy = resolved;
    return true;
}

bool BuildLabGuestGraphicsEnvironment(std::string_view name,
                                      D3dBackendKind backend,
                                      std::vector<std::string>* environment)
{
    if (!environment) return false;
    environment->clear();
    ProductGraphicsPolicy experiment;
    if (!ResolveLabGraphicsExperiment(name, backend, &experiment)) return false;
    return SerializeGuestGraphicsEnvironment(name, experiment.guest, backend,
                                             environment);
}

bool BuildProductGuestGraphicsEnvironment(
    D3dBackendKind backend, std::vector<std::string>* environment)
{
    if (!environment) return false;
    environment->clear();
    ProductGraphicsPolicy policy;
    if (!ResolveProductGraphicsPolicy(backend, &policy)) return false;
    return SerializeGuestGraphicsEnvironment(policy.route, policy.guest,
                                             backend, environment);
}

} // namespace winehua
