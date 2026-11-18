// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <cstdint>

// Versioned, synchronous command-recording ABI. Pixels are borrowed for the
// call; NativeBuffers are retained by core imports. Fence FDs are borrowed.
// GPU work remains asynchronous. A successful call exports one completion FD
// covering every sampled buffer; the caller duplicates it for queue release.
struct VppGamescopeLayer {
    void* native = nullptr;
    const void* pixels = nullptr;
    uint64_t key = 0, serial = 0;
    uint64_t pixelBytes = 0;
    uint32_t width = 0, height = 0;
    float x = 0, y = 0, w = 0, h = 0;
    float u0 = 0, u1 = 1, v0 = 0, v2 = 1;
    int32_t fence = -1;
    uint32_t vulkan = 0, opaque = 1, upscale = 0;
};
struct VppGamescopeFrame {
    uint32_t size = sizeof(VppGamescopeFrame), width = 0, height = 0;
    uint32_t count = 0, filter = 0, sharpness = 10;
    const VppGamescopeLayer* layers = nullptr;
    int32_t completion = -1;
    uint32_t gpuLayers = 0, fsrPasses = 0;
    uint64_t nowNs = 0, displayPeriodNs = 0;
    uint32_t frameGeneration = 0, historyReset = 0;
    // Results count unique source frames separately from generated output.
    uint32_t generated = 0, frameGenerationState = 0;
    uint32_t completedGenerated = 0;
    uint64_t nextTickNs = 0, frameGenerationGpuNs = 0;
};
enum VppFrameGenerationState : uint32_t {
    VPP_FG_OFF, VPP_FG_WARMING, VPP_FG_RUNNING, VPP_FG_DISPLAY_RATE,
    VPP_FG_UNSUPPORTED, VPP_FG_GPU_BUSY, VPP_FG_FAILED, VPP_FG_NO_GPU_GAME,
    VPP_FG_NO_CADENCE, VPP_FG_BUDGET
};
extern "C" {
bool VppGamescopeEmbeddedStart(void* nativeWindow, uint32_t width, uint32_t height);
int VppGamescopeEmbeddedPresent(VppGamescopeFrame* frame);
void VppGamescopeEmbeddedStop();
}
