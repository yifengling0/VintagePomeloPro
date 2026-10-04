"""Run production binding cleanup and the geometry lookup's lifetime guard.

Platform resources are stand-ins; this verifies destroyed producers, takeover
claims and geometry lookup, rather than native GPU output.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = 'entry/src/main/cpp/compositor/frame/zc_bridge.cpp'

STUB = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
template<typename... Args> static void TestLog(Args&&...) {}
#define LOG_APP 0
#define OH_LOG_INFO(...) TestLog(__VA_ARGS__)
struct SurfaceData { bool hasToplevel = false, isSubsurface = false; };
struct wl_resource { SurfaceData* data; };
static void* wl_resource_get_user_data(wl_resource* res) { return res->data; }
struct ZeroCopyLayerInfo {};
struct WineHuaPresentBinding { struct { uint32_t ownerHostPid, wlSurfaceId; } window; };
struct Manager {
    std::mutex mutex;
    std::unordered_map<uint64_t, wl_resource*> resources;
    auto Lock() { return std::unique_lock<std::mutex>(mutex); }
    wl_resource* FindSurfaceResource(uint64_t key) {
        const auto it = resources.find(key);
        return it == resources.end() ? nullptr : it->second;
    }
};
struct Compositor { Manager tmgr_; };
class ZcBridge {
public:
    Compositor comp_;
    std::unordered_map<uint64_t, WineHuaPresentBinding> presentBindings_;
    std::unordered_map<uint64_t, uint64_t> windowBindings_, lastPresentUsByKey_;
    std::unordered_map<uint64_t, unsigned> bindDiagProducers_;
    std::unordered_set<uint64_t> bindingRejectedLogged_;
    std::mutex presentLivenessMutex_;
    unsigned resolveCalls = 0;
    bool ResolvePresentBinding(uint64_t key, uint32_t, uint32_t,
                               WineHuaPresentBinding* result, bool*) {
        ++resolveCalls;
        const auto it = presentBindings_.find(key);
        if (it == presentBindings_.end()) return false;
        *result = it->second; return true;
    }
    bool GetLayerInfo(uint64_t, uint32_t, int, int, ZeroCopyLayerInfo&, const char**);
    void InvalidateBindingsForSurface(uint32_t, uint32_t);
};
'''

TEST = r'''
static uint64_t Key(uint32_t pid, uint32_t id) { return (uint64_t(pid) << 32) | id; }
int main() {
    ZcBridge bridge;
    const auto old = Key(200, 37), next = Key(200, 164), other = Key(300, 37);
    const auto window = Key(100, 25), otherWindow = Key(100, 30);
    bridge.presentBindings_[old] = {{100, 25}};
    bridge.presentBindings_[next] = {{100, 25}};
    bridge.presentBindings_[other] = {{100, 30}};
    bridge.windowBindings_[window] = next;
    bridge.windowBindings_[otherWindow] = other;
    bridge.bindingRejectedLogged_.insert(old);
    bridge.bindDiagProducers_[old] = 1;
    bridge.lastPresentUsByKey_[old] = 123;
    bridge.InvalidateBindingsForSurface(200, 37);
    assert(!bridge.presentBindings_.count(old));
    assert(bridge.presentBindings_.count(next));
    assert(bridge.windowBindings_.at(window) == next);
    assert(!bridge.bindingRejectedLogged_.count(old));
    assert(!bridge.bindDiagProducers_.count(old));
    assert(!bridge.lastPresentUsByKey_.count(old));
    // Window destruction removes all generations, including a retired peer
    // which no longer holds the active window claim.
    bridge.presentBindings_[old] = {{100, 25}};
    bridge.InvalidateBindingsForSurface(100, 25);
    assert(!bridge.presentBindings_.count(old) && !bridge.presentBindings_.count(next));
    assert(!bridge.windowBindings_.count(window));
    assert(bridge.presentBindings_.count(other));
    assert(bridge.windowBindings_.at(otherWindow) == other);

    // A cached window must not make a missing producer resource appear alive.
    SurfaceData data;
    wl_resource windowResource{&data}, producerResource{&data};
    bridge.comp_.tmgr_.resources[window] = &windowResource;
    bridge.presentBindings_[old] = {{100, 25}};
    ZeroCopyLayerInfo layer;
    const char* reason = nullptr;
    assert(!bridge.GetLayerInfo(old, 1, 1200, 800, layer, &reason));
    assert(bridge.resolveCalls == 0);
    bridge.comp_.tmgr_.resources[old] = &producerResource;
    assert(bridge.GetLayerInfo(old, 1, 1200, 800, layer, &reason));
    assert(bridge.resolveCalls == 1);
    data.isSubsurface = true;
    assert(bridge.GetLayerInfo(old, 1, 1200, 800, layer, &reason));
    assert(bridge.resolveCalls == 1);
    data.hasToplevel = true;
    data.isSubsurface = false;
    assert(bridge.GetLayerInfo(old, 1, 1200, 800, layer, &reason));
    assert(bridge.resolveCalls == 1);
    puts("ZC lifecycle: destruction, takeover, foreign peer, missing-resource guard and explicit topology passed");
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline', action='store_true')
    options = parser.parse_args()
    source = (subprocess.check_output(['git', 'show', 'HEAD:' + SOURCE], cwd=ROOT).decode()
              if options.baseline else (ROOT / SOURCE).read_text())
    name = ('InvalidateBindingsForWindow' if options.baseline else 'InvalidateBindingsForSurface')
    start = source.index('void ZcBridge::' + name + '(')
    end = source.index('\n// P0-1 Task A', start)
    cleanup = source[start:end].replace(name, 'InvalidateBindingsForSurface')
    start = source.index('bool ZcBridge::GetLayerInfo(')
    end = source.index('    // -- present surface', start)
    guard = source[start:end] + '\n    return sd != nullptr;\n}\n'
    with tempfile.TemporaryDirectory(prefix='zc-lifecycle-test-') as temp:
        folder = Path(temp)
        (folder / 'test.cpp').write_text(STUB + cleanup + guard + TEST)
        exe = folder / 'test'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-pthread', str(folder / 'test.cpp'),
                        '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], timeout=10)
        if options.baseline:
            assert result.returncode != 0, 'old cleanup must retain a destroyed producer'
            print('baseline reproduced stale binding after producer destruction')
        else:
            result.check_returncode()


if __name__ == '__main__':
    main()
