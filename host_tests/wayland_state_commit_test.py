"""Replay buffer and state-only commits through the production protocol handlers.

The boundary stubs record releases, frame acknowledgements and layer removal;
the actual SurfaceData and handler bodies are compiled without the OHOS host.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'entry/src/main/cpp/compositor/wl_core.cpp').read_text()
def handler(signature):
    start = source.index(signature)
    end = source.index('\n}', start) + 2
    return source[start:end]

stub = r'''
#include "compositor/frame/surface_data.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <mutex>
struct wl_resource { void* data=nullptr; unsigned released=0, done=0, destroyed=0; };
void* wl_resource_get_user_data(wl_resource* r) { assert(r); return r->data; }
void wl_resource_destroy(wl_resource* r) { assert(r && !r->destroyed); r->destroyed++; }
static void wl_buffer_send_release(wl_resource* r) { assert(r); r->released++; }
static void wl_callback_send_done(wl_resource* r, uint32_t) { assert(r); r->done++; }
#define OH_LOG_INFO(...) ((void)0)
struct DisplayPolicy { enum class SubsurfaceRoute { DesktopLayer }; };
struct ToplevelEventType { static constexpr int PopupHide=1; };
struct ToplevelEventBus { static int JsonPopupHide(uint32_t) { return 1; } };
struct Manager { std::mutex m; auto Lock() { return std::unique_lock<std::mutex>(m); } };
struct Desktop {
    bool layer=true; unsigned removals=0;
    bool RemoveSubsurfaceLayer(wl_resource*, uint32_t& parent, DisplayPolicy::SubsurfaceRoute&) {
        removals++; parent=1; bool old=layer; layer=false; return old;
    }
};
struct Popup {
    unsigned removals=0;
    uint32_t RemovePopupBySurfaceKeyLocked(uint64_t, uint32_t& removed) { removals++; removed=0; return 0; }
};
struct WaylandServer {
    Manager toplevelMgr_; Desktop desktopCompositor_; Popup popupMgr_; unsigned firstFrames=0;
    void MarkLayerHostDirtyLocked(uint32_t, DisplayPolicy::SubsurfaceRoute) {}
    void PostToplevelEvent(uint32_t, int, int) {}
    void TryBeginSessionFirstFrame(uint32_t, wl_resource*) { firstFrames++; }
    static void surface_attach(wl_client*, wl_resource*, wl_resource*, int32_t, int32_t);
    bool HandleNullBufferCommit(SurfaceData*, wl_resource*);
    void FinishCommit(SurfaceData*, wl_resource*);
};
'''
main = r'''
int main() {
    WaylandServer server; SurfaceData data; data.isSubsurface=true;
    wl_resource surface{&data}, buffer, first;
    WaylandServer::surface_attach(nullptr,&surface,&buffer,0,0);
    data.frameCallbacks.push_back(&first);
    assert(!server.HandleNullBufferCommit(&data,&surface));
    server.FinishCommit(&data,&surface);
    assert(buffer.released==1 && first.done==1 && first.destroyed==1);
    assert(!data.pendingBufferAttached && server.firstFrames==1);
    // GPU swaps have no new pixel buffer: acknowledge each commit and preserve
    // the current layer. Never re-release the previous SHM geometry buffer.
    for(unsigned n=0;n<240;n++) {
        wl_resource callback; data.frameCallbacks.push_back(&callback);
        assert(server.HandleNullBufferCommit(&data,&surface));
        assert(callback.done==1 && callback.destroyed==1 && data.frameCallbacks.empty());
        assert(server.desktopCompositor_.layer && server.desktopCompositor_.removals==0);
        assert(server.popupMgr_.removals==0 && buffer.released==1 && server.firstFrames==1);
    }
    // Explicit attach(NULL) still unmaps exactly once; a later state-only
    // commit must not unmap again or manufacture a new buffer release.
    WaylandServer::surface_attach(nullptr,&surface,nullptr,0,0);
    assert(server.HandleNullBufferCommit(&data,&surface));
    assert(!server.desktopCompositor_.layer && server.desktopCompositor_.removals==1);
    assert(server.HandleNullBufferCommit(&data,&surface));
    assert(server.desktopCompositor_.removals==1 && buffer.released==1);
    // Last pending attach wins before commit; ordinary SHM resumes unchanged.
    WaylandServer::surface_attach(nullptr,&surface,nullptr,0,0);
    WaylandServer::surface_attach(nullptr,&surface,&buffer,0,0);
    assert(!server.HandleNullBufferCommit(&data,&surface));
    server.FinishCommit(&data,&surface);
    assert(buffer.released==2 && server.firstFrames==2 && !data.pendingBufferAttached);
    puts("PASS: 240 GPU state commits, callbacks, explicit unmap, SHM release and reattach");
}
'''
with tempfile.TemporaryDirectory(prefix='vp-state-commit-') as directory:
    cpp = Path(directory) / 'test.cpp'
    binary = Path(directory) / 'test'
    cpp.write_text(stub + '\n'.join(handler(signature) for signature in [
        'void WaylandServer::surface_attach(',
        'bool WaylandServer::HandleNullBufferCommit(',
        'void WaylandServer::FinishCommit(']) + main)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(root / 'entry/src/main/cpp'), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=15,
                   env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
