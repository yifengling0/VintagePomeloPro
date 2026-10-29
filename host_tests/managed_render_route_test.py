"""Run the real NAPI renderer callbacks with platform I/O mocked.

Direct capability must not bypass GDI rendering or split output ownership.
"""
from pathlib import Path
import subprocess
R = Path(__file__).resolve().parents[1]
out = R/'build/host_tests/managed-render-route'
out.mkdir(parents=True, exist_ok=True)
s = (R/'entry/src/main/cpp/bridge/napi_init.cpp').read_text()
callbacks = s[s.index('static napi_value CreateRenderer('):s.index('// -- NAPI: setOutputSize --')]
stubs = r'''#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstdio>
using napi_env = void*;
using napi_value = int64_t*;
struct Callback { size_t argc; napi_value args[3]; };
using napi_callback_info = Callback*;
using napi_status = int;
constexpr int napi_ok = 0;
int napi_get_cb_info(napi_env, Callback* info, size_t* count, napi_value* args, void*, void*) {
    size_t n = *count < info->argc ? *count : info->argc;
    for (size_t i=0; i<n; ++i) args[i]=info->args[i];
    *count = n; return 0;
}
int napi_get_value_uint32(napi_env, napi_value v, uint32_t* out) { *out=*v; return 0; }
int napi_get_value_int32(napi_env, napi_value v, int32_t* out) { *out=*v; return 0; }
int napi_get_value_bigint_int64(napi_env, napi_value v, int64_t* out, bool* lossless) {
    *out=*v; *lossless=true; return 0;
}
#define OH_LOG_INFO(...) ((void)0)
#define OH_LOG_ERROR(...) ((void)0)
// If these obsolete routes are reintroduced, the test must fail to link.
bool DirectWineSurfaceBindOutput(uint32_t, uint64_t);
bool DirectWineSurfaceResizeOutput(uint32_t, int32_t, int32_t);
bool DirectWineSurfaceUnbindOutput(uint32_t);
struct PluginManager {
    int creates=0, resizes=0, destroys=0;
    uint32_t top=0; int64_t surface=0; int w=0, h=0;
    static PluginManager* GetInstance() { static PluginManager p; return &p; }
    void CreateRenderer(uint32_t t, int64_t s) { ++creates;top=t;surface=s; }
    void ResizeRenderer(uint32_t t, int x, int y) { ++resizes;top=t;w=x;h=y; }
    void DestroyToplevel(uint32_t t) { ++destroys;top=t; }
};
'''
tests = r'''
int main() {
    auto* p=PluginManager::GetInstance();
    int64_t top=17, surface=10000000003LL, height=720;
    Callback c{2,{&top,&surface,nullptr}};
    CreateRenderer(nullptr,&c);
    assert(p->creates==1 && p->top==17 && p->surface==surface);
    int64_t width=1280;
    c={3,{&top,&width,&height}};
    ResizeRenderer(nullptr,&c);
    assert(p->resizes==1 && p->top==17 && p->w==1280 && p->h==720);
    c={1,{&top,nullptr,nullptr}};
    DestroyRenderer(nullptr,&c);
    assert(p->destroys==1 && p->top==17);
    // Two windows keep distinct IDs; no global pending renderer identity.
    top=18; surface=10000000004LL;
    c={2,{&top,&surface,nullptr}}; CreateRenderer(nullptr,&c);
    assert(p->creates==2 && p->top==18 && p->surface==surface);
    c.argc=0; CreateRenderer(nullptr,&c); ResizeRenderer(nullptr,&c);
    assert(p->creates==2 && p->resizes==1);
    std::puts("PASS production managed renderer lifecycle: one compositor for GDI/GPU-capable windows");
}
'''
(out/'callbacks.cpp').write_text(stubs+callbacks+tests)
common=['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined']
subprocess.run([*common,str(out/'callbacks.cpp'),'-o',str(out/'callbacks')],check=True)
subprocess.run([str(out/'callbacks')],check=True,timeout=20)
subprocess.run([*common,'-I',str(R/'entry/src/main/cpp'),str(R/'host_tests/direct_consumer_scope_test.cpp'),'-o',str(out/'scopes')],check=True)
subprocess.run([str(out/'scopes')],check=True,timeout=20)

# Exercise the real managed/root move dispatcher: WMS-owned motion must not
# modify virtual geometry, while root moves and native resizes still do.
s=(R/'entry/src/main/cpp/compositor/wayland_server.cpp').read_text()
move=s[s.index('bool WaylandServer::ProcessMoveGrabMotion('):s.index('void WaylandServer::PostToplevelEvent(')]
stub=r'''#include <cassert>
#include <cstdio>
using wl_fixed_t=int;
int wl_fixed_to_int(int x) { return x/256; }
struct WaylandServer {
    struct PolicyState { bool managed=true; bool OhosWindowPerToplevel() const { return managed; } } policy;
    struct Grab {
        bool active=true; unsigned resize=0; int motions=0;
        bool IsActive() const { return active; }
        unsigned ResizeTarget() const { return resize; }
        struct Bounds { int width=640,height=480; };
        Bounds ResizeBounds() const { return {}; }
        bool ProcessMoveGrabMotion(int&,int,int) { ++motions; return active; }
    } moveGrab_;
    int toplevelMgr_=0, dirty=0, resizes=0;
    PolicyState& Policy() { return policy; }
    void NotifyToplevelResize(unsigned,int,int,bool) { ++resizes; }
    void MarkDesktopRootDirtyLocked() { ++dirty; }
    bool ProcessMoveGrabMotion(wl_fixed_t,wl_fixed_t);
};
'''
test=r'''
int main() {
    WaylandServer host;
    for (int i=0;i<100;++i) assert(host.ProcessMoveGrabMotion(i*256,i*256));
    assert(host.moveGrab_.motions==0 && host.dirty==0);
    host.policy.managed=false;
    assert(host.ProcessMoveGrabMotion(256,512));
    assert(host.moveGrab_.motions==1 && host.dirty==1);
    host.policy.managed=true; host.moveGrab_.resize=2;
    assert(host.ProcessMoveGrabMotion(256,512));
    assert(host.moveGrab_.motions==2 && host.resizes==1);
    host.moveGrab_.active=false;host.moveGrab_.resize=0;
    assert(!host.ProcessMoveGrabMotion(256,512));
    std::puts("PASS production move dispatch: stable WMS origin, desktop moves and resize preserved");
}
'''
(out/'move.cpp').write_text(stub+move+test)
subprocess.run([*common,str(out/'move.cpp'),'-o',str(out/'move')],check=True)
subprocess.run([str(out/'move')],check=True,timeout=20)
