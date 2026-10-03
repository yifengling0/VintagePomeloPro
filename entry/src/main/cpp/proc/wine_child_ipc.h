#pragma once

#include <cstdint>
#include <string>

namespace winehua::wineipc {

constexpr int32_t kVersion = 1;
constexpr uint32_t kBootstrap = 1;
constexpr uint32_t kAttachSurface = 2;
constexpr uint32_t kDetachSurface = 3;
constexpr uint32_t kQuerySurface = 4;
constexpr uint32_t kFinishSurfaceProbe = 5;
constexpr uint32_t kResizeSurface = 6;
constexpr int32_t kMaxFds = 16;
constexpr char kProbeParams[] = "__winehua_direct_ipc_probe__";
constexpr char kSurfaceProbeParams[] = "__winehua_direct_surface_ipc_probe__";
inline bool IsProbeParams(const std::string& params) {
    return params == kProbeParams || params == std::string(kProbeParams) + "|__env=WINEHUA_DIRECT_NCP=1" ||
        params == kSurfaceProbeParams || params == std::string(kSurfaceProbeParams) + "|__env=WINEHUA_VULKAN_BACKEND=direct";
}
constexpr uint32_t kProbeToken = 0x57484950; // WHIP

struct ProbeResult {
    int32_t pid;
    int32_t status;
    uint32_t token;
    int32_t fdCount;
};

// The Wayland client PID is the Wine NCP host PID. A toplevel may be reused
// after destruction, so every producer handoff has a strictly newer generation.
struct DirectSurfaceToken {
    int32_t clientPid;
    uint32_t toplevelId;
    uint32_t wlSurfaceId;
    uint64_t generation;
    int32_t width;
    int32_t height;
};

} // namespace winehua::wineipc
