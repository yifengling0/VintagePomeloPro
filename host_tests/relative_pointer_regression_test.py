"""Replay production relative-pointer routing and motion calculation on the host.

Only platform/resource plumbing is stubbed. The tested C++ methods and motion
block are extracted from production sources, so --baseline proves the lost
protections fail on the pre-fix implementation.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--baseline', action='store_true')
args = parser.parse_args()


def source(path):
    if args.baseline:
        return subprocess.check_output(['git', 'show', 'HEAD:' + path], cwd=ROOT).decode()
    return (ROOT / path).read_text()


def method(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}', start) + 2]


cpp = 'entry/src/main/cpp/'
extras = source(cpp + 'input/pointer_extras.cpp')
manager = source(cpp + 'input/input_manager.cpp')
injector = source(cpp + 'compositor/input/input_injector.cpp')
start = manager.index('    // 相对指针增量')
motion = manager[start:manager.index('    // MOVE 是高频路径', start)]
start = extras.index('    const bool desktopNotReady')
shell_guard = extras[start:extras.index('    // IPC 挪入', start)]
fixed = 'HasRelativePointerForSurface(' in extras
relative_type = ('struct RelativePointer {wl_resource* resource;wl_resource* pointer;wl_client* client;};'
                 if fixed else '')
relative_vector = 'RelativePointer' if fixed else 'wl_resource*'
surface_decl = 'wl_resource* surface, ' if fixed else ''
methods = [method(extras, 'bool PointerExtras::HasRelativePointer('),
           method(extras, 'void PointerExtras::SendRelativeMotion(')]
if fixed:
    methods.append(method(extras, 'bool PointerExtras::HasRelativePointerForSurface('))
methods.append(method(injector, 'void InputInjector::InjectRelativeMotion('))
fixture = r'''
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>
#include "compositor/input/input_state_tracker.h"
#define OH_LOG_INFO(...) ((void)0)
using wl_fixed_t = int32_t;
wl_fixed_t wl_fixed_from_double(double x) {return static_cast<int32_t>(std::lround(x * 256));}
double wl_fixed_to_double(wl_fixed_t x) {return x / 256.0;}
struct wl_client {int id;};
struct wl_resource {wl_client* client;bool alive=true;};
wl_client* wl_resource_get_client(wl_resource* r) {return r->client;}
struct Resolver {bool IsSurfaceAlive(wl_resource* r) const {return r && r->alive;}};
struct Delivery {wl_resource* resource;double x,y;};
std::vector<Delivery> deliveries;
void zwp_relative_pointer_v1_send_relative_motion(wl_resource* r,uint32_t,uint32_t,
    wl_fixed_t x,wl_fixed_t y,wl_fixed_t ux,wl_fixed_t uy) {
    if(x!=ux || y!=uy) std::abort();
    deliveries.push_back({r,wl_fixed_to_double(x),wl_fixed_to_double(y)});
}
class PointerExtras {
public:
    RELATIVE_TYPE
    mutable std::mutex mutex_;
    std::vector<RELATIVE_VECTOR> relativePointers_;
    Resolver* resolver_=nullptr;
    static PointerExtras* GetInstance() {static PointerExtras instance;return &instance;}
    bool HasRelativePointer() const;
    SURFACE_QUERY
    void SendRelativeMotion(SURFACE_DECL double dx,double dy);
};
class InputInjector {
public:
    Resolver* resolver_=nullptr;
    void InjectRelativeMotion(SURFACE_DECL wl_fixed_t dx,wl_fixed_t dy);
};
struct InputQueue {
    struct Event {enum {REL_MOTION};};
    struct Motion {wl_resource* surface;double x,y;};
    std::vector<Motion> events;
    void Enqueue(int,uint32_t,wl_resource* s,wl_fixed_t x,wl_fixed_t y,uint32_t,uint32_t) {
        events.push_back({s,wl_fixed_to_double(x),wl_fixed_to_double(y)});
    }
};
struct Manager {
    InputStateTracker tracker_;
    InputQueue queue_;
    struct Toplevels {wl_resource* surface=nullptr;
        wl_resource* GetSurfaceForToplevel(uint32_t) {return surface;}} tmgr;
    Toplevels* tmgr_=&tmgr;
    static constexpr int ACT_MOVE=3;
    void Move([[maybe_unused]] uint32_t tl,[[maybe_unused]] wl_resource* targetSurf,
              double x,double y,double rawDx=0,double rawDy=0,
              [[maybe_unused]] FitRect inputFit={},[[maybe_unused]] FitRect displayFit={}) {
        int action=ACT_MOVE;
        auto wx=wl_fixed_from_double(x),wy=wl_fixed_from_double(y);
        MOTION_BLOCK
    }
};
METHODS
bool SkipHostLock(uint32_t toplevelId,const uint32_t* desktopRootToplevelId_,const bool* desktopMode_) {
    SHELL_GUARD
    return isShell;
}
int failures=0,checks=0;
#define CHECK(cond,msg) do {++checks;if(!(cond)){++failures;std::printf("FAIL: %s\n",msg);}} while(0)
int main() {
    uint32_t root=1;
    bool desktop=true;
    CHECK(SkipHostLock(0,&root,&desktop),"unknown window identity cannot freeze the system pointer");
    CHECK(SkipHostLock(1,&root,&desktop),"desktop shell cannot freeze the system pointer");
    CHECK(!SkipHostLock(2,&root,&desktop),"known game window can still request pointer lock");
    root=0;
    CHECK(SkipHostLock(2,&root,&desktop),"desktop startup cannot freeze the system pointer");
    wl_client game{1},steam{2},other{3};
    wl_resource gameSurf{&game},otherSurf{&other},gameSub{&game};
    wl_resource gameRelative{&game},gameRelative2{&game},steamRelative{&steam};
    Resolver resolver;
    auto* extras=PointerExtras::GetInstance();extras->resolver_=&resolver;
    REGISTER_RESOURCES
    InputInjector inject;inject.resolver_=&resolver;
    inject.InjectRelativeMotion(GAME_SURFACE_ARG wl_fixed_from_double(2.5),wl_fixed_from_double(-1.25));
    CHECK(deliveries.size()==2,"relative motion reaches only the game's two protocol resources");
    CHECK(std::all_of(deliveries.begin(),deliveries.end(),[&](const auto& d){return d.resource->client==&game;}),
          "game relative motion must not reach the shared Steam client");
    CHECK(!deliveries.empty() && deliveries[0].x==2.5 && deliveries[0].y==-1.25,
          "hardware delta is preserved without an extra speed multiplier");
    deliveries.clear();gameSurf.alive=false;
    inject.InjectRelativeMotion(GAME_SURFACE_ARG 256,256);
    CHECK(deliveries.empty(),"queued motion for a destroyed surface is discarded");
    gameSurf.alive=true;
    QUERY_CHECKS
    FitRect fit;fit.srcW=800;fit.srcH=600;fit.offX=20;fit.scale=1;
    FitRect display;display.srcW=1920;display.srcH=1080;display.dstW=960;display.dstH=540;display.scale=.5;
    Manager m;
    m.Move(10,&gameSurf,100,100,0,0,fit,display);
    CHECK(m.queue_.events.empty(),"first synthetic move establishes a baseline");
    m.Move(10,&gameSurf,103,98,0,0,fit,display);
    CHECK(m.queue_.events.size()==1 && m.queue_.events.back().x==3 && m.queue_.events.back().y==-2,
          "continuous movement in the same window keeps its real delta");
    m.queue_.events.clear();
    m.Move(11,&gameSurf,400,300,0,0,fit,display);
    CHECK(m.queue_.events.empty(),"switching toplevel cannot create a synthetic jump");
    m.queue_.events.clear();
    m.Move(11,&gameSub,5,5,0,0,fit,display);
    CHECK(m.queue_.events.empty(),"switching subsurface cannot reuse the previous local coordinates");
    m.queue_.events.clear();fit.scale=2;
    m.Move(11,&gameSub,250,250,0,0,fit,display);
    CHECK(m.queue_.events.empty(),"content scaling invalidates the coordinate baseline");
    m.queue_.events.clear();display.offX=100;
    m.Move(11,&gameSub,150,250,0,0,fit,display);
    CHECK(m.queue_.events.empty(),"display letterboxing invalidates the coordinate baseline");
    m.queue_.events.clear();
    INVALIDATE_BASELINE
    m.Move(11,&gameSub,200,250,0,0,fit,display);
    CHECK(m.queue_.events.empty(),"mode change cannot retain an old differential baseline");
    m.queue_.events.clear();
    m.Move(11,&otherSurf,220,260,0,0,fit,display);
    CHECK(m.queue_.events.empty(),"a different client without relative mode receives no relative motion");
    m.queue_.events.clear();
    m.Move(11,&gameSub,400,400,0,0,fit,display);
    CHECK(m.queue_.events.empty(),"returning from absolute mode establishes a fresh baseline");
    m.queue_.events.clear();
    m.Move(12,&gameSurf,900,900,2.5,-.625,fit,display);
    CHECK(m.queue_.events.size()==1 && m.queue_.events[0].x==2.5 && m.queue_.events[0].y==-.625,
          "valid raw hardware movement survives a coordinate-space change");
    TARGET_CHECK
    m.queue_.events.clear();
    m.Move(12,&gameSurf,901,901,1000,-1000,fit,display);
    CHECK(m.queue_.events.size()==1 && m.queue_.events[0].x==512 && m.queue_.events[0].y==-512,
          "the existing abnormal raw delta clamp remains in effect");
    std::printf("relative_pointer_regression: %d checks / %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
'''
replacements = {
    'RELATIVE_TYPE': relative_type,
    'RELATIVE_VECTOR': relative_vector,
    'SURFACE_QUERY': 'bool HasRelativePointerForSurface(wl_resource*) const;' if fixed else '',
    'SURFACE_DECL': surface_decl,
    'MOTION_BLOCK': motion,
    'METHODS': '\n'.join(methods),
    'SHELL_GUARD': shell_guard,
    'REGISTER_RESOURCES': ('extras->relativePointers_={{&gameRelative,nullptr,&game},{&gameRelative2,nullptr,&game},{&steamRelative,nullptr,&steam}};'
                           if fixed else 'extras->relativePointers_={&gameRelative,&gameRelative2,&steamRelative};'),
    'GAME_SURFACE_ARG': '&gameSurf, ' if fixed else '',
    'QUERY_CHECKS': ('''CHECK(extras->HasRelativePointerForSurface(&gameSub),"same-client subsurface supports relative input");
        CHECK(!extras->HasRelativePointerForSurface(&otherSurf),"relative mode is client-specific");
        CHECK(!extras->HasRelativePointerForSurface(nullptr),"missing surface never enters relative mode");'''
                     if fixed else ''),
    'INVALIDATE_BASELINE': 'm.tracker_.InvalidateRelativeBaseline();' if fixed else '',
    'TARGET_CHECK': ('CHECK(m.queue_.events[0].surface==&gameSurf,"queued relative motion retains its target surface");'
                     if fixed else ''),
}
for key, value in replacements.items():
    fixture = fixture.replace(key, value)
with tempfile.TemporaryDirectory(prefix='vp-relative-pointer-') as folder:
    folder = Path(folder)
    test = folder / 'test.cpp'
    test.write_text(fixture)
    binary = folder / 'test'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-I', str(ROOT / cpp), str(test),
                    str(ROOT / cpp / 'compositor/input/input_state_tracker.cpp'),
                    '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    if args.baseline:
        assert result.returncode == 1 and 'game relative motion must not reach' in result.stdout, result.stdout + result.stderr
        assert 'switching toplevel cannot create' in result.stdout, result.stdout
        print('BASELINE RED: cross-client broadcast and stale coordinate jumps reproduced')
    else:
        assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout.strip())
