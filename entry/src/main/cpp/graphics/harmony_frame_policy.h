#pragma once

#include <cstdint>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace winehua::graphics {

// Shared by the app, the Venus presenter and Wine. DXVK maps the first
// uint32_t read-only. Changing the low bit also advances the generation.
struct HarmonyFramePolicy {
    uint32_t request;
    uint32_t displayPeriodNs;
    uint64_t result;
};
static_assert(sizeof(HarmonyFramePolicy) == 16);

struct HarmonyFrameScene {
    uint32_t version, size;
    uint64_t serial, color, depth;
    uint32_t colorFormat, colorLayout, depthFormat, depthLayout, width, height;
    float viewProjection[16], inverseViewProjection[16];
};
static_assert(sizeof(HarmonyFrameScene) == 184);

inline constexpr char kHarmonyFramePolicyPath[] =
    "/data/storage/el2/base/files/.vpp-harmony-frame-policy-v1";
inline constexpr uint64_t kHarmonyMaxSourceIntervalNs = 70'000'000;

inline HarmonyFramePolicy* MapHarmonyFramePolicy(bool writer)
{
    const int flags = writer ? O_RDWR | O_CREAT : O_RDWR;
    const int fd = open(kHarmonyFramePolicyPath, flags | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return nullptr;
    struct stat info{};
    bool valid = fstat(fd, &info) == 0 && S_ISREG(info.st_mode);
    if (valid && writer && info.st_size == 0)
        valid = ftruncate(fd, sizeof(HarmonyFramePolicy)) == 0;
    else
        valid = valid && info.st_size == sizeof(HarmonyFramePolicy);
    void* view = valid ? mmap(nullptr, sizeof(HarmonyFramePolicy), PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, 0) : MAP_FAILED;
    close(fd);
    return view == MAP_FAILED ? nullptr : static_cast<HarmonyFramePolicy*>(view);
}

inline HarmonyFramePolicy* HarmonyFramePolicyForApp()
{
    static auto* const policy = MapHarmonyFramePolicy(true);
    return policy;
}

inline uint32_t HarmonyFrameRequest(const HarmonyFramePolicy* policy)
{
    return policy ? __atomic_load_n(&policy->request, __ATOMIC_ACQUIRE) : 0;
}

inline void SelectHarmonyFrameGeneration(HarmonyFramePolicy* policy, bool enabled)
{
    if (!policy) return;
    const uint32_t previous = HarmonyFrameRequest(policy);
    if (bool(previous & 1u) != enabled)
        __atomic_store_n(&policy->request, ((previous & ~1u) + 2u) | uint32_t(enabled), __ATOMIC_RELEASE);
}

inline void ReportHarmonyFrameGeneration(HarmonyFramePolicy* policy, uint32_t request,
                                         uint32_t state, uint32_t generated)
{
    if (!policy || HarmonyFrameRequest(policy) != request) return;
    const uint64_t result = (uint64_t(request) << 32) | (uint64_t(generated & 0x00ffffffu) << 8) | state;
    __atomic_store_n(&policy->result, result, __ATOMIC_RELEASE);
}

} // namespace winehua::graphics
