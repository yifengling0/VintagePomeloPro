#include "graphics/graphics_profile.h"
#include "graphics/present_pacing.h"
#include "graphics/present_policy.h"
#include "graphics/virgl_host_config.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, std::string_view message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

struct ExpectedProfile {
    std::string_view name;
    std::string_view shadowMode;
    std::string_view shadowSelector;
    bool mergeShadowRanges = true;
    bool waitGpuUpload = false;
    bool serializeDescriptorUpdates = false;
    bool deferSharedMemoryUnref = false;
    bool perfSummary = false;
    winehua::GpuUploadPolicy gpuUpload = winehua::GpuUploadPolicy::Automatic;
};

struct ExpectedGuestPolicy {
    std::string_view name;
    bool preciseShadow;
    winehua::GuestSubmissionPolicy submission;
    bool perfSummary = false;
    bool directFenceWait = false;
    bool tracePresentImage = false;
    bool deferSharedMemoryUnref = false;
    bool disableGpuUpload = false;
    bool serializeDescriptorUpdates = false;
};

void CheckProfileField(bool condition, const ExpectedProfile& expected,
                       std::string_view field)
{
    if (condition) return;
    std::cerr << "FAIL: profile " << expected.name << " field " << field << '\n';
    ++failures;
}

void TestBackendPolicy()
{
    using winehua::D3dBackendKind;
    Check(winehua::ParseD3dBackend("wined3d") == D3dBackendKind::WineD3d,
          "WineD3D backend classification");
    Check(winehua::ParseD3dBackend("dxvk_legacy") == D3dBackendKind::DxvkLegacy,
          "DXVK 1.10 backend classification");
    Check(winehua::ParseD3dBackend("dxvk_modern_2_6") == D3dBackendKind::DxvkModern26,
          "DXVK 2.6 backend classification");
    Check(winehua::ParseD3dBackend("typo") == D3dBackendKind::Unknown,
          "unknown backend fails closed");
    Check(winehua::UsesVenusPresent(D3dBackendKind::DxvkLegacy),
          "DXVK 1.10 uses Venus present");
    Check(winehua::UsesVenusPresent(D3dBackendKind::DxvkModern26),
          "DXVK 2.6 uses Venus present");
    Check(!winehua::UsesVenusPresent(D3dBackendKind::WineD3d),
          "WineD3D uses VirGL present");
    winehua::ProductGraphicsPolicy wined3dPolicy;
    winehua::ProductGraphicsPolicy legacyPolicy;
    winehua::ProductGraphicsPolicy modernPolicy;
    winehua::ProductGraphicsPolicy vkd3dPolicy;
    Check(winehua::ResolveProductGraphicsPolicy(
              D3dBackendKind::WineD3d, &wined3dPolicy),
          "resolve WineD3D product route");
    Check(wined3dPolicy.route == winehua::kProductVirglRoute &&
              wined3dPolicy.host.shadowMode == "full" &&
              !wined3dPolicy.guest.preciseShadow,
          "WineD3D uses the neutral product VirGL route");
    Check(winehua::ResolveProductGraphicsPolicy(
              D3dBackendKind::DxvkLegacy, &legacyPolicy) &&
              winehua::ResolveProductGraphicsPolicy(
                  D3dBackendKind::DxvkModern26, &modernPolicy),
          "resolve both DXVK product routes");
    Check(legacyPolicy.route == winehua::kProductVulkanRoute &&
              modernPolicy.route == winehua::kProductVulkanRoute &&
              legacyPolicy.host.shadowMode == modernPolicy.host.shadowMode &&
              legacyPolicy.host.shadowSelector == modernPolicy.host.shadowSelector,
          "DXVK versions share one product Vulkan policy");
    Check(winehua::ResolveProductGraphicsPolicy(
              D3dBackendKind::Vkd3dLimited500k, &vkd3dPolicy),
          "resolve VKD3D product route");
    Check(vkd3dPolicy.route == winehua::kProductVulkanRoute &&
              vkd3dPolicy.host.shadowMode == "precise" &&
              vkd3dPolicy.guest.submission ==
                  winehua::GuestSubmissionPolicy::StrongRingBarrier &&
              vkd3dPolicy.guest.directFenceWait,
          "VKD3D stays on Vulkan route with adapter policy");
    Check(!winehua::ResolveProductGraphicsPolicy(
              D3dBackendKind::Unknown, &legacyPolicy),
          "unknown backend has no product route");

    winehua::DxvkRuntimeProfile runtime;
    Check(winehua::ResolveDxvkRuntimeProfile(D3dBackendKind::DxvkLegacy, &runtime),
          "resolve DXVK 1.10 runtime");
    Check(runtime.directory == "legacy" && runtime.version == "1.10.3" &&
              runtime.relaxedFeatureCompatibility &&
              runtime.commandQueryReset && runtime.dynamicMappedFlush &&
              runtime.batchMappedFlush &&
              runtime.disableSemaphoreFeedback,
          "DXVK 1.10 high-performance runtime contract");
    Check(winehua::ResolveDxvkRuntimeProfile(D3dBackendKind::DxvkModern26, &runtime),
          "resolve DXVK 2.6 runtime");
    Check(runtime.directory == "modern-2.6" && runtime.version == "2.6.2" &&
              !runtime.relaxedFeatureCompatibility &&
              !runtime.commandQueryReset && !runtime.dynamicMappedFlush &&
              runtime.batchMappedFlush && runtime.disableSemaphoreFeedback,
          "DXVK 2.6 runtime contract");
    winehua::DxvkRuntimeProfile vkd3dRuntime;
    Check(winehua::ResolveDxvkRuntimeProfile(
              D3dBackendKind::Vkd3dLimited500k, &vkd3dRuntime),
          "resolve VKD3D companion DXGI runtime");
    Check(vkd3dRuntime.directory == runtime.directory &&
              vkd3dRuntime.version == runtime.version &&
              vkd3dRuntime.batchMappedFlush == runtime.batchMappedFlush,
          "VKD3D shares the DXVK 2.6 companion runtime");
    Check(!winehua::ResolveDxvkRuntimeProfile(D3dBackendKind::WineD3d, &runtime),
          "WineD3D does not resolve a DXVK runtime");
    Check(!winehua::ResolveDxvkRuntimeProfile(D3dBackendKind::Unknown, &runtime),
          "unknown backend does not resolve a DXVK runtime");
}

void TestProfileResolution()
{
    constexpr std::array<ExpectedProfile, 5> expectedProfiles = {{
        {"observe-product-summary", "precise-dirty",
         "inline-gpu-upload-coverage-sort",
         true, false, false, false, true},
        {"observe-frame-timeline", "precise-dirty", "frame-timeline",
         true, false, false, false, true},
        {"trace-frame-association", "precise-dirty",
         "inline-gpu-upload-frame-assoc-trace",
         true, false, false, false, true},
        {"trace-present-image", "precise-dirty", "present-image-trace"},
        {"isolate-transport-neutral", "full", "0"},
    }};

    for (const ExpectedProfile& expected : expectedProfiles) {
        winehua::ProductGraphicsPolicy experiment;
        if (!winehua::ResolveLabGraphicsExperiment(
                expected.name, winehua::D3dBackendKind::DxvkLegacy,
                &experiment)) {
            std::cerr << "FAIL: resolve profile " << expected.name << '\n';
            ++failures;
            continue;
        }
        const winehua::HostGraphicsProfile& resolved = experiment.host;
        CheckProfileField(resolved.name == expected.name, expected, "name");
        CheckProfileField(resolved.shadowMode == expected.shadowMode,
                          expected, "shadowMode");
        CheckProfileField(resolved.shadowSelector == expected.shadowSelector,
                          expected, "shadowSelector");
        CheckProfileField(resolved.mergeShadowRanges == expected.mergeShadowRanges,
                          expected, "mergeShadowRanges");
        CheckProfileField(resolved.waitGpuUpload == expected.waitGpuUpload,
                          expected, "waitGpuUpload");
        CheckProfileField(
            resolved.serializeDescriptorUpdates == expected.serializeDescriptorUpdates,
            expected, "serializeDescriptorUpdates");
        CheckProfileField(
            resolved.deferSharedMemoryUnref == expected.deferSharedMemoryUnref,
            expected, "deferSharedMemoryUnref");
        CheckProfileField(resolved.perfSummary == expected.perfSummary,
                          expected, "perfSummary");
        CheckProfileField(resolved.gpuUpload == expected.gpuUpload,
                          expected, "gpuUpload");
    }

    winehua::ProductGraphicsPolicy legacyExperiment;
    winehua::ProductGraphicsPolicy modernExperiment;
    Check(winehua::ResolveLabGraphicsExperiment(
              "observe-product-summary", winehua::D3dBackendKind::DxvkLegacy,
              &legacyExperiment) &&
              winehua::ResolveLabGraphicsExperiment(
                  "observe-product-summary",
                  winehua::D3dBackendKind::DxvkModern26,
                  &modernExperiment),
          "resolve LAB experiment on both DXVK generations");
    Check(legacyExperiment.route == winehua::kProductVulkanRoute &&
              modernExperiment.route == winehua::kProductVulkanRoute &&
              legacyExperiment.host.shadowMode == modernExperiment.host.shadowMode &&
              legacyExperiment.host.shadowSelector ==
                  modernExperiment.host.shadowSelector,
          "LAB observation inherits one product Vulkan policy");
    winehua::ProductGraphicsPolicy virglExperiment;
    Check(winehua::ResolveLabGraphicsExperiment(
              "observe-product-summary", winehua::D3dBackendKind::WineD3d,
              &virglExperiment) &&
              virglExperiment.route == winehua::kProductVirglRoute &&
              virglExperiment.host.shadowMode == "full" &&
              virglExperiment.host.perfSummary,
          "common summary observation derives from the VirGL product route");
    Check(!winehua::ResolveLabGraphicsExperiment(
              "trace-present-image", winehua::D3dBackendKind::WineD3d,
              &virglExperiment),
          "Venus-only present trace rejects the VirGL route");

    Check(!winehua::ResolveLabGraphicsExperiment(
              "baseline", winehua::D3dBackendKind::DxvkLegacy,
              &legacyExperiment),
          "historical neutral profile alias stays removed");
    Check(!winehua::ResolveLabGraphicsExperiment(
              "shadow-precise-dirty-ring-frame-timeline",
              winehua::D3dBackendKind::DxvkLegacy, &legacyExperiment),
          "historical combinatorial profile stays removed");
    Check(!winehua::ResolveLabGraphicsExperiment(
              winehua::kProductVulkanRoute,
              winehua::D3dBackendKind::DxvkLegacy, &legacyExperiment),
          "product route is not a LAB profile id");
}

bool HasEnvironmentLine(const std::vector<std::string>& environment,
                        std::string_view expected)
{
    for (const std::string& line : environment) {
        if (line == expected) return true;
    }
    return false;
}

void TestGuestProfileResolution()
{
    using Submission = winehua::GuestSubmissionPolicy;
    constexpr std::array<ExpectedGuestPolicy, 5> expectedPolicies = {{
        {"observe-product-summary", true, Submission::StrongRingBarrier, true},
        {"observe-frame-timeline", true, Submission::StrongRingBarrier},
        {"trace-frame-association", true, Submission::StrongRingBarrier},
        {"trace-present-image", true, Submission::StrongRingBarrier,
         false, false, true},
        {"isolate-transport-neutral", false, Submission::Default},
    }};

    for (const ExpectedGuestPolicy& expected : expectedPolicies) {
        winehua::ProductGraphicsPolicy experiment;
        if (!winehua::ResolveLabGraphicsExperiment(
                expected.name, winehua::D3dBackendKind::DxvkLegacy,
                &experiment)) {
            std::cerr << "FAIL: resolve guest policy " << expected.name << '\n';
            ++failures;
            continue;
        }
        const winehua::GuestGraphicsPolicy& resolved = experiment.guest;
        Check(resolved.preciseShadow == expected.preciseShadow,
              std::string(expected.name) + " guest precise shadow");
        Check(resolved.submission == expected.submission,
              std::string(expected.name) + " guest submission");
        Check(resolved.perfSummary == expected.perfSummary,
              std::string(expected.name) + " guest perf summary");
        Check(resolved.directFenceWait == expected.directFenceWait,
              std::string(expected.name) + " direct fence wait");
        Check(resolved.tracePresentImage == expected.tracePresentImage,
              std::string(expected.name) + " present image trace");
        Check(resolved.deferSharedMemoryUnref == expected.deferSharedMemoryUnref,
              std::string(expected.name) + " deferred shmem unref");
        Check(resolved.disableGpuUpload == expected.disableGpuUpload,
              std::string(expected.name) + " disabled GPU upload");
        Check(resolved.serializeDescriptorUpdates ==
                  expected.serializeDescriptorUpdates,
              std::string(expected.name) + " descriptor serialization");
    }

    std::vector<std::string> environment;
    Check(winehua::BuildProductGuestGraphicsEnvironment(
              winehua::D3dBackendKind::DxvkLegacy, &environment),
          "serialize product DXVK 1.10 guest policy");
    Check(HasEnvironmentLine(environment,
              "WINEHUA_GRAPHICS_PROFILE=product-vulkan") &&
              HasEnvironmentLine(environment,
              "DXVK_WINEHUA_PRECISE_SHADOW=1") &&
              HasEnvironmentLine(environment,
              "VN_WINEHUA_STRONG_RING_BARRIER=1") &&
              HasEnvironmentLine(environment,
              "VN_WINEHUA_REMOTE_MEMORY_SYNC=1") &&
              HasEnvironmentLine(environment,
              "VN_PERF=no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring") &&
              HasEnvironmentLine(environment,
              "DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1"),
          "DXVK 1.10 receives common precise/strong legacy policy");

    Check(winehua::BuildProductGuestGraphicsEnvironment(
              winehua::D3dBackendKind::DxvkModern26, &environment),
          "serialize product DXVK 2.6 guest policy");
    Check(!HasEnvironmentLine(environment,
              "DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1") &&
              HasEnvironmentLine(environment,
              "VN_PERF=no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring"),
          "DXVK 2.6 excludes legacy mapped-flush switch");

    Check(winehua::BuildProductGuestGraphicsEnvironment(
              winehua::D3dBackendKind::Vkd3dLimited500k, &environment),
          "serialize product VKD3D guest policy");
    Check(HasEnvironmentLine(environment,
              "WINEHUA_GRAPHICS_PROFILE=product-vulkan") &&
              HasEnvironmentLine(environment,
              "VN_WINEHUA_STRONG_RING_BARRIER=1") &&
              HasEnvironmentLine(environment,
              "VN_WINEHUA_DIRECT_FENCE_WAIT=1") &&
              !HasEnvironmentLine(environment,
              "DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1"),
          "VKD3D uses Vulkan route with its qualified modern adapter policy");

    Check(winehua::BuildLabGuestGraphicsEnvironment(
              "isolate-transport-neutral",
              winehua::D3dBackendKind::DxvkLegacy, &environment),
          "serialize neutral transport isolation");
    Check(HasEnvironmentLine(environment,
              "DXVK_WINEHUA_PRECISE_SHADOW=0") &&
              HasEnvironmentLine(environment,
              "VN_WINEHUA_STRONG_RING_BARRIER=0") &&
              HasEnvironmentLine(environment,
              "VN_WINEHUA_REMOTE_MEMORY_SYNC=0") &&
              HasEnvironmentLine(environment,
              "DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=0"),
          "neutral isolation resets precise/strong/remote/mapped-flush policy");

    Check(winehua::BuildLabGuestGraphicsEnvironment(
              "observe-product-summary",
              winehua::D3dBackendKind::DxvkLegacy, &environment),
          "serialize product-derived observation policy");
    Check(HasEnvironmentLine(environment,
              "VN_WINEHUA_STRONG_RING_BARRIER=1") &&
              HasEnvironmentLine(environment, "VN_WINEHUA_PERF_SUMMARY=1"),
          "observation retains product transport and enables periodic summary");

    Check(winehua::BuildLabGuestGraphicsEnvironment(
              "observe-frame-timeline",
              winehua::D3dBackendKind::DxvkModern26, &environment),
          "serialize Modern product-derived timeline observation");
    Check(HasEnvironmentLine(environment,
              "WINEHUA_GRAPHICS_PROFILE=observe-frame-timeline") &&
              !HasEnvironmentLine(environment,
              "DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1") &&
              HasEnvironmentLine(environment,
              "VN_PERF=no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring"),
          "Modern observation applies runtime capabilities without legacy switches");

    environment = {"STALE=1"};
    Check(!winehua::BuildLabGuestGraphicsEnvironment(
              "unknown-experiment", winehua::D3dBackendKind::DxvkLegacy,
              &environment) && environment.empty(),
          "unknown Guest experiment fails closed and clears stale output");
}

void TestPresentPacing()
{
    winehua::GlPresentFailureBackoff backoff;
    Check(backoff.PendingDeadline(100) == 0, "healthy GL has no retry gate");
    Check(backoff.Fail(100, 8000000) == 8000100, "first GL failure waits one frame");
    Check(backoff.PendingDeadline(101) == 8000100, "failed GL skips GPU before deadline");
    Check(backoff.PendingDeadline(8000100) == 0, "failed GL can retry when due");
    Check(backoff.Fail(8000100, 8000000) == 24000100, "repeated GL failure backs off");
    uint64_t now = 24000100;
    for (int i = 0; i < 1000; ++i) {
        const uint64_t deadline = backoff.Fail(now, 8000000);
        Check(deadline > now && deadline - now <= 50000000,
              "GL failure delay stays future and within Guest clamp");
        now = deadline;
    }
    Check(backoff.Fail(now, 8000000) - now == 50000000, "persistent failure is capped at 20 Hz");
    backoff.Reset();
    Check(backoff.PendingDeadline(now) == 0, "success or new target clears GL retry state");
    Check(backoff.Fail(now, 0) - now == winehua::kDefaultPresentFramePeriodNs,
          "missing GL period cannot disable failure pacing");
    Check(backoff.Fail(UINT64_MAX - 2, 8000000) == UINT64_MAX,
          "GL failure deadline saturates without wraparound");
    Check(winehua::NormalizePresentFramePeriodNs(0) ==
              winehua::kDefaultPresentFramePeriodNs,
          "zero display period uses the common default");
    Check(winehua::NormalizePresentFramePeriodNs(1000000) ==
              winehua::kMinPresentFramePeriodNs,
          "display period clamps to the common minimum");
    Check(winehua::NormalizePresentFramePeriodNs(100000000) ==
              winehua::kMaxPresentFramePeriodNs,
          "display period clamps to the common maximum");
    Check(winehua::PresentPacingPeriodNs(16666667) == 16166667,
          "common pacing applies the dispatch lead");
    Check(winehua::PresentPacingPeriodNs(
              winehua::kMinPresentFramePeriodNs) ==
              winehua::kMinPresentFramePeriodNs,
          "dispatch lead never crosses the common pacing floor");
    const auto first = winehua::EvaluatePresentPacing(100, 0, 16);
    Check(first.presentNow && first.nextDeadlineNs == 0,
          "first frame presents immediately");
    const auto early = winehua::EvaluatePresentPacing(110, 100, 16);
    Check(!early.presentNow && early.nextDeadlineNs == 116,
          "early frame returns the existing deadline");
    const auto due = winehua::EvaluatePresentPacing(116, 100, 16);
    Check(due.presentNow, "frame presents at deadline");
    Check(winehua::RetryPresentDeadlineNs(120, 100, 16) == 136,
          "queue-full retry produces a future deadline");
    Check(winehua::RetryPresentDeadlineNs(110, 100, 16) == 116,
          "queue-full retry preserves a pending deadline");
    Check(winehua::NextPresentDeadlineNs(
              std::numeric_limits<uint64_t>::max() - 4, 16) ==
              std::numeric_limits<uint64_t>::max(),
          "deadline addition saturates");
    Check(!winehua::DirectPresentUsesGuestDeadline(
              winehua::kDirectPresentWarmupFrames - 1),
          "Direct Present warmup keeps clock pacing");
    Check(winehua::DirectPresentUsesGuestDeadline(
              winehua::kDirectPresentWarmupFrames),
          "Direct Present switches to guest deadline after warmup");
    Check(winehua::DirectPresentAcquireTimeoutNs(0) ==
              winehua::kDirectFirstAcquireTimeoutNs,
          "Direct Present allows first-buffer allocation");
    Check(winehua::DirectPresentAcquireTimeoutNs(
              winehua::kDirectPresentWarmupFrames) == 0,
          "Direct Present never blocks queue acquire after warmup");
}

void TestPresenterRuntimePolicy()
{
    const auto production = winehua::ResolvePresenterRuntimePolicy(
        "inline-gpu-upload-coverage-sort", "0", "1", "0");
    Check(!production.traceStages && !production.gpuFrameProfile,
          "production presenter leaves per-frame diagnostics off");
    Check(production.perfSummary,
          "production presenter keeps periodic summary enabled");

    const auto trace = winehua::ResolvePresenterRuntimePolicy(
        "1", "1", "0", "0");
    Check(trace.traceStages && trace.traceFrameOrder && trace.gpuFrameProfile,
          "trace policy derives all requested diagnostics once");
    const auto frameOrder = winehua::ResolvePresenterRuntimePolicy(
        "0", "0", "0", "1");
    Check(frameOrder.traceFrameOrder && !frameOrder.traceStages,
          "present-image trace does not enable all stage logs");
}

void TestVirglHostConfig()
{
    winehua::VirglHostConfig config = {
        "/data/libwinehua_vtest_server.so", "/data/virgl.sock",
        "/data/libs", "egl-thread", "/data/virgl.log", "precise-dirty",
        "inline-gpu-upload-coverage-sort", "1", "1", "0", "0",
    };
    winehua::VirglHostLaunchConfig launch;
    std::string error;
    Check(winehua::BuildVirglHostLaunchConfig(config, &launch, &error),
          "WHIP v10 Host config accepts explicit summary bit");
    Check(launch.entryParams.find(
              "WINEHUA_VTEST_PRESENT_PERF_SUMMARY=1") != std::string::npos &&
              launch.entryParams.find("VKR_WINEHUA_PERF_SUMMARY=1") !=
                  std::string::npos &&
              launch.forwardPerfSummary,
          "Host summary bit reaches renderer, presenter and log forwarder");

    const uint64_t summaryFingerprint =
        winehua::FingerprintVirglHostConfig(config);
    config.perfSummary = "0";
    Check(winehua::FingerprintVirglHostConfig(config) != summaryFingerprint,
          "summary bit participates in Host configuration identity");
    config.perfSummary = "mailbox";
    Check(!winehua::ValidateVirglHostConfig(config, &error),
          "retired present-mode value fails the binary WHIP v10 slot");
}

} // namespace

int main()
{
    TestBackendPolicy();
    TestProfileResolution();
    TestGuestProfileResolution();
    TestPresentPacing();
    TestPresenterRuntimePolicy();
    TestVirglHostConfig();
    if (failures) return EXIT_FAILURE;
    std::cout << "graphics_policy_test: PASS\n";
    return EXIT_SUCCESS;
}
