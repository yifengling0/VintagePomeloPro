#pragma once
#include <cstdint>

namespace steamohos {
struct SurfaceResult {
    // 0 = SHM protocol and keyboard round trip verified; no presentation claim.
    uint32_t status = 1;
    uint32_t displayError = 0;
    uint32_t surfaceId = 0;
    uint32_t configured = 0;
    uint32_t framesSubmitted = 0;
    uint32_t bufferReleases = 0;
    uint32_t frameCallbacks = 0;
    uint32_t keyboardKeymap = 0;
    uint32_t keyboardEntered = 0;
    uint32_t keyEvents = 0;
    uint32_t inputVerified = 0;
    uint32_t windowClosed = 0;
    const char* stage = "connect";
};

// Called on the probe's worker, never on the compositor's dispatch thread.
// notify publishes only the client object ID, not a host wl_resource pointer.
using SurfaceReady = void (*)(uint32_t surfaceId, void* context);
SurfaceResult ProbeSurface(const char* socketPath, uint32_t timeoutMs,
                           SurfaceReady notify = nullptr, void* context = nullptr);
}
