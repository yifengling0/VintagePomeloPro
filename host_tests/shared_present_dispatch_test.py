"""Run the actual presenter manager with stand-in GPU/window targets.

The manager and its waits/locking/routing come from production source. Only
the platform targets are stubs; this checks dispatch progress, not GPU output.
--baseline must reproduce the old blocking behavior and fail the latency gate.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = 'entry/src/main/cpp/graphics/virgl_surface_presenter.cpp'
HEADER = r'''
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "graphics/virgl_ipc_protocol.h"
#include "graphics/present_target.h"
#define LOG_APP 0
template<typename... Args> static void TestLog(Args&&...) {}
#define OH_LOG_INFO(...) TestLog(__VA_ARGS__)
#define OH_LOG_WARN(...) TestLog(__VA_ARGS__)
using namespace winehua;
using SteadyClock = std::chrono::steady_clock;
static uint64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        SteadyClock::now().time_since_epoch()).count();
}
constexpr auto kVenusTargetAttachTimeout = std::chrono::milliseconds(2500);
constexpr auto kVirglTargetAttachTimeout = std::chrono::milliseconds(500);
struct OHNativeWindow {};
static unsigned glFrames, vkFrames;
template<bool Vulkan> class TestTarget : public PresentTarget {
public:
    int Attach(uint64_t, uint64_t, OHNativeWindow*, bool) override { return 0; }
    int Detach(uint64_t) override { return 0; }
    int SetFramePeriod(uint64_t) override { return 0; }
    bool IsVulkan() const override { return Vulkan; }
    int Present(GLuint, uint32_t, uint32_t, uint64_t, uint32_t, uint64_t*) override {
        assert(!Vulkan); ++glFrames; return 0;
    }
    int PresentVenus(uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
        uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
        uint32_t, uint64_t*, void (*)(void*), void*) override {
        assert(Vulkan); ++vkFrames; return 0;
    }
    bool HasVulkanDevice() override { return false; }
    bool PrepareDeviceRelease(uint32_t, uintptr_t) override { return false; }
    bool FinishDeviceRelease(uint32_t, uintptr_t, int32_t) override { return false; }
};
using SurfaceQueueTarget = TestTarget<false>;
namespace winehua { using VenusSurfaceQueueTarget = TestTarget<true>; }
'''
TEST = r'''
static uint64_t Key(uint32_t pid, uint32_t surface) { return (uint64_t(pid) << 32) | surface; }
static int Vk(SurfaceQueuePresenterManager& manager, uint32_t pid, uint32_t surface,
              uint32_t width = 800, uint32_t height = 600) {
    uint64_t deadline = 0;
    return manager.PresentVenus(1, 1, 1, 1, 1, 1, 0, width, height, 37, 0,
        pid, surface, 1, &deadline, nullptr, nullptr);
}
int main() {
    SurfaceQueuePresenterManager manager;
    OHNativeWindow window;
    uint64_t deadline = 0;
    // Simulate the shared dispatch order: unresolved background Vulkan
    // windows alternate with an attached foreground GL client.
    assert(manager.Attach(Key(100, 47), 16666667, 0, &window) == 0);
    for (uint32_t surface = 1; surface <= 64; ++surface) {
        const uint64_t started = NowUs();
        assert(Vk(manager, 200, surface) == -EAGAIN);
        assert(manager.Present(100, 47, 1, 800, 600, 0, surface, &deadline) == 0);
        const uint64_t elapsed = NowUs() - started;
        if (elapsed >= 100000) {
            fprintf(stderr, "background target blocked foreground dispatch: %llu us\n",
                (unsigned long long)elapsed);
            return 1;
        }
    }
    assert(glFrames == 64);
    assert(manager.Present(300, 48, 1, 800, 600, 0, 1, &deadline) == kPresentNoTarget);
    // A miss must remain query-visible and recover when the consumer attaches.
    assert(manager.Attach(Key(300, 48), 16666667, 0, &window) == 0);
    assert(manager.Present(300, 48, 1, 800, 600, 0, 2, &deadline) == 0);
    assert(manager.Attach(Key(200, 64), 16666667, virgl_ipc::kSurfaceVulkan, &window) == 0);
    assert(Vk(manager, 200, 64) == 0);
    assert(manager.Present(200, 64, 1, 800, 600, 0, 1, &deadline) == kPresentInvalid);
    assert(manager.Detach(Key(200, 64)) == 0);
    assert(manager.Attach(Key(200, 64), 16666667, 0, &window) == 0);
    assert(manager.Present(200, 64, 1, 800, 600, 0, 2, &deadline) == 0);
    assert(Vk(manager, 200, 64) == kPresentInvalid);
    assert(manager.Detach(Key(300, 48)) == 0);
    assert(manager.Present(300, 48, 1, 800, 600, 0, 3, &deadline) == kPresentNoTarget);
    const auto query = manager.Query();
    bool found = false;
    for (uint32_t i = 0; i < query.count; ++i) {
        const auto& surface = query.surfaces[i];
        if (surface.surfaceKey == Key(300, 48)) {
            found = true;
            assert(!(surface.flags & virgl_ipc::kSurfaceVulkan));
            assert(!(surface.flags & virgl_ipc::kSurfaceAttached));
        }
    }
    assert(found);
    assert(Vk(manager, 400, 100, 1, 1) == -EAGAIN);
    manager.Reset();
    assert(manager.Query().count == 0);
    puts("shared present dispatch: 64 background misses, foreground progress, attach recovery, mixed kinds and detach passed");
}
'''

def main():
    args = argparse.ArgumentParser()
    args.add_argument('--baseline', action='store_true')
    args.add_argument('--baseline-ref', default='HEAD',
                      help='Git revision containing the old blocking presenter')
    options = args.parse_args()
    source = (subprocess.check_output(['git', 'show', options.baseline_ref+':'+SOURCE], cwd=ROOT).decode()
              if options.baseline else (ROOT/SOURCE).read_text())
    start = source.index('class SurfaceQueuePresenterManager {')
    end = source.index('\nSurfaceQueuePresenterManager g_presenters;', start)
    with tempfile.TemporaryDirectory(prefix='shared-present-test-') as temp:
        folder = Path(temp)
        (folder/'GLES3').mkdir()
        (folder/'native_window').mkdir()
        (folder/'GLES3/gl3.h').write_text('typedef unsigned GLuint;\n')
        (folder/'native_window/external_window.h').write_text('struct OHNativeWindow;\n')
        cpp = folder/'test.cpp'
        cpp.write_text(HEADER + source[start:end] + TEST)
        exe = folder/'test'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-I'+str(folder), '-I'+str(ROOT/'entry/src/main/cpp'),
                        str(cpp), '-o', str(exe), '-pthread'], check=True)
        result = subprocess.run([str(exe)], timeout=10)
        if options.baseline:
            assert result.returncode == 1, 'old source must reproduce blocked dispatch'
            print('baseline reproduced shared-dispatch blocking')
        else:
            result.check_returncode()

if __name__ == '__main__':
    main()
