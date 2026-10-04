"""Exercise production Venus routing, binding transitions and scene snapshot code.

Only platform objects are stand-ins. Use --baseline to prove the old branches
fail these assertions; this is not a GPU/device rendering or FPS measurement.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--baseline', action='store_true')
parser.add_argument('--baseline-ref', default='960bf939835e53494118a6be592bf68bd5f638de',
                    help='Git revision used by --baseline (defaults to the reviewed pre-fix commit)')
options = parser.parse_args()


def read(path):
    return (subprocess.check_output(['git', 'show', options.baseline_ref + ':' + path], cwd=ROOT).decode()
            if options.baseline else (ROOT / path).read_text())


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


COMMON = r'''
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
template<class... T> void Log(T&&...) {}
#define LOG_APP 0
#define OH_LOG_INFO(...) Log(__VA_ARGS__)
#define OH_LOG_WARN(...) Log(__VA_ARGS__)
static uint64_t Key(uint32_t pid,uint32_t id) { return (uint64_t(pid)<<32)|id; }
'''

VENUS = COMMON + r'''
namespace winehua { namespace virgl_ipc { constexpr unsigned kSurfaceAttached=1,kSurfaceVulkan=2; } }
constexpr int kPresentInvalid=-22;
static uint64_t NowUs() { return 1; }
struct Target {
    unsigned calls=0; bool vulkan=true;
    bool IsVulkan() { return vulkan; }
    template<class... T> int PresentVenus(T&&...) { ++calls; return 0; }
};
struct Entry { std::shared_ptr<Target> target; bool missingTargetLogged=false;
    struct { uint64_t surfaceKey=0; uint32_t clientPid=0,surfaceId=0,width=0,height=0,serial=0,flags=0; } info;
    uint64_t lastPresentUs=0;
};
struct Presenter {
    std::mutex mutex_; std::unordered_map<uint64_t,Entry> surfaces_;
'''
VENUS_END = r'''
};
static int present(Presenter& p,uint32_t pid) {
    return p.PresentVenus(1,1,1,1,1,1,0,100,100,0,0,pid,37,1,nullptr,nullptr,nullptr);
}
int main() {
    Presenter p;
    auto a=std::make_shared<Target>(),b=std::make_shared<Target>();
    p.surfaces_[Key(100,37)].target=a; p.surfaces_[Key(100,37)].info.flags=3;
    p.surfaces_[Key(200,37)].target=b; p.surfaces_[Key(200,37)].info.flags=3;
    assert(present(p,300)==-EAGAIN); assert(a->calls==0 && b->calls==0);
    assert(p.surfaces_.at(Key(100,37)).info.clientPid==0);
    assert(present(p,100)==0 && a->calls==1 && b->calls==0);
    p.surfaces_.erase(Key(100,37)); // detached generation cannot route to peer
    assert(present(p,100)==-EAGAIN && b->calls==0);
    auto replacement=std::make_shared<Target>();
    p.surfaces_[Key(100,37)].target=replacement;p.surfaces_[Key(100,37)].info.flags=3;
    assert(present(p,100)==0 && replacement->calls==1 && a->calls==1 && b->calls==0);
    replacement->vulkan=false; assert(present(p,100)==kPresentInvalid);
    puts("Venus exact identity: PID collisions, missing target, detach/reuse, backend guard passed");
}
'''

BIND = COMMON + r'''
#include "compositor/frame/zc_bridge.h"
struct Manager {
    std::mutex mutex; std::unordered_set<uint64_t> resources;
    auto Lock() { return std::unique_lock<std::mutex>(mutex); }
    void* FindSurfaceResource(uint64_t key) { return resources.count(key)?this:nullptr; }
};
struct SurfaceData { uint32_t clientPid=100,protocolId=25,toplevelId=5; };
struct Bridge {
    struct { Manager tmgr_; } comp_;
    std::unordered_map<uint64_t,WineHuaPresentBinding> presentBindings_;
    std::unordered_map<uint64_t,uint64_t> windowBindings_,lastPresentUsByKey_;
    struct Diag { bool bound=false; char lastReject[48]={}; };
    std::unordered_map<uint64_t,Diag> bindDiagProducers_;
    uint64_t nextBindingGeneration_=0;
    std::unordered_set<uint64_t> bindingRejectedLogged_;
    std::mutex presentLivenessMutex_;
    uint64_t LastPresentUs(uint64_t) { return 0; }
    bool NoteLayerConsumed(uint64_t,uint64_t,uint64_t);
    void InvalidateBindingsForSurface(uint32_t,uint32_t);
    bool Create(uint64_t surfaceKey, bool takeoverCandidate, WineHuaPresentBinding* outBinding);
    bool Resolve(uint64_t surfaceKey,uint32_t frameWidth,uint32_t frameHeight,WineHuaPresentBinding* outBinding) {
'''
BIND_END = r'''
int main() {
    Bridge b; const auto old=Key(200,37),next=Key(200,164),window=Key(100,25);
    b.comp_.tmgr_.resources={old,next,window};
    WineHuaPresentBinding binding;binding.window={100,25,0};binding.bindGeneration=1;
    b.presentBindings_[old]=binding;binding.bindGeneration=2;binding.pending=true;b.presentBindings_[next]=binding;
    b.windowBindings_[window]=next;WineHuaPresentBinding out;
    for(int i=0;i<3;++i) { assert(b.Resolve(next,1200,800,&out));assert(out.pending && !b.presentBindings_[old].retired); }
    b.NoteLayerConsumed(next,20,1);assert(b.presentBindings_[next].pending);
    b.comp_.tmgr_.resources.erase(next);b.NoteLayerConsumed(next,20,2);assert(b.presentBindings_[next].pending);
    b.comp_.tmgr_.resources.insert(next);b.comp_.tmgr_.resources.erase(window);
    b.NoteLayerConsumed(next,20,2);assert(b.presentBindings_[next].pending);
    b.comp_.tmgr_.resources.insert(window);b.NoteLayerConsumed(next,20,2);
    assert(!b.presentBindings_[next].pending && b.presentBindings_[old].retired);
    assert(b.presentBindings_[next].lastDrawUs==20);
    for(int i=0;i<3;++i) { assert(!b.Resolve(old,1200,800,&out));assert(b.presentBindings_[old].retired); }
    assert(b.windowBindings_[window]==next);
    b.InvalidateBindingsForSurface(200,164);assert(!b.presentBindings_.count(next));
    binding.bindGeneration=3;binding.pending=true;b.presentBindings_[next]=binding;b.windowBindings_[window]=next;
    b.NoteLayerConsumed(next,30,2);assert(b.presentBindings_[next].pending); // stale generation
    b.NoteLayerConsumed(next,30,3);assert(!b.presentBindings_[next].pending);
    b.InvalidateBindingsForSurface(100,25);assert(b.presentBindings_.empty());
    Bridge fresh;
    assert(fresh.Create(next,true,&out) && out.pending && out.bindGeneration!=0);
    const auto generation=out.bindGeneration;fresh.InvalidateBindingsForSurface(200,164);
    assert(fresh.Create(next,true,&out) && out.pending && out.bindGeneration>generation);
    puts("Binding: zero-activity queries, first consume, retirement tombstone, disconnect and generation reuse passed");
}
'''

SCENE = COMMON + r'''
#include "compositor/frame/gpu_desktop_scene.h"
#include "compositor/frame/geometry.h"
#include "compositor/toplevel/zorder_policy.h"
struct SurfaceData {
    bool hasToplevel=false,isSubsurface=false,inputRegionEmpty=false;uint32_t toplevelId=0;
    void* parentSurface=nullptr;void* surface=nullptr;int subsurfaceX=0,subsurfaceY=0;
    DirectViewportState directViewport;bool isExternal=false;uint64_t surfaceKey=0,shmCommitSerial=0;
    int w=0,h=0,vpDstW=0,vpDstH=0,shmFormat=1;std::vector<uint8_t> pixels;
};
static void* wl_resource_get_user_data(void* r) { return r; }
struct State {
    bool frame=true,hidden=false,minimized=false;
    int Width() const {return 1;} int Height() const {return 1;} int X() const {return 0;} int Y() const {return 0;}
    uint64_t FrameSerial() const {return 1;} int ShmFormat() const {return 1;}
    bool HasFrame() const {return frame;} bool IsBackground() const {return hidden;} bool IsMinimized() const {return minimized;}
    const std::vector<uint8_t>& Pixels() const {static std::vector<uint8_t> p={0,0,0,255};static std::vector<uint8_t> empty;return frame?p:empty;}
};
struct Manager {
    std::mutex m;State root,parent;bool hasParent=false,inZOrder=false;std::unordered_map<uint64_t,void*> resources;
    auto Lock(){return std::unique_lock<std::mutex>(m);}
    const State* FindToplevelLocked(uint32_t id){return id==1?&root:(id==5 && hasParent?&parent:nullptr);}
    void* FindSurfaceResource(uint64_t key){const auto i=resources.find(key);return i==resources.end()?nullptr:i->second;}
    auto& SurfaceResources(){return resources;}bool IsInZOrder(uint32_t id){return id==5 && inZOrder;}
};
struct CompositorLayer {
    enum class Type{Root,Toplevel,Subsurface};Type type=Type::Root;bool visible=true,zcActive=false;
    uint32_t toplevelId=1;int x=0,y=0,w=1,h=1;SurfaceData* sub=nullptr;
};
static bool ShouldSkipFullscreenCascade(const CompositorLayer& l,uint32_t id,bool,Manager&){return id && l.type!=CompositorLayer::Type::Root && l.toplevelId!=id;}
struct DesktopCompositor {
    Manager tmgr_;uint32_t desktopRootToplevelId_=1,fullscreen=0;int outputW_=1200,outputH_=800;
    struct{bool RootCompositing(){return true;}}policy_;std::unordered_map<uint32_t,std::pair<int,int>> directDesktopContentSizes_;
    std::vector<CompositorLayer> BuildLayerListLocked(int,int){
        std::vector<CompositorLayer> out(1);if(tmgr_.inZOrder && tmgr_.hasParent){CompositorLayer p;p.type=CompositorLayer::Type::Toplevel;p.toplevelId=5;
            p.visible=tmgr_.parent.frame && !tmgr_.parent.hidden && !tmgr_.parent.minimized;out.push_back(p);}return out;
    }
    uint32_t PickFullscreenLayerLocked(const std::vector<CompositorLayer>&){return fullscreen;}
    void ComputeFullscreenFitLocked(uint32_t,int,int,FitRect&){}
    bool SnapshotGpuDesktopScene(const std::vector<GpuDesktopDirectSource>&,GpuDesktopSnapshotCache&,GpuDesktopScene&,const std::vector<GpuDesktopLayer>&);
};
'''
SCENE_END = r'''
int main(){
    DesktopCompositor c;GpuDesktopSnapshotCache cache;GpuDesktopScene out;
    SurfaceData parent,child;parent.hasToplevel=true;parent.toplevelId=5;parent.surfaceKey=Key(100,5);
    child.isSubsurface=true;child.parentSurface=&parent;child.surfaceKey=Key(100,25);
    c.tmgr_.resources[parent.surfaceKey]=&parent;c.tmgr_.resources[child.surfaceKey]=&child;
    GpuDesktopLayer source;source.parentToplevel=5;source.zeroCopyKey=Key(200,163);source.ownerSurfaceKey=child.surfaceKey;
    source.subsurface=true;source.w=source.sourceW=1200;source.h=source.sourceH=800;
    const auto includes=[&](){assert(c.SnapshotGpuDesktopScene({},cache,out,{source}));
        return std::any_of(out.layers.begin(),out.layers.end(),[&](const auto& l){return l.zeroCopyKey==source.zeroCopyKey;});};
    assert(includes());assert(out.layers.back().zeroCopyKey==source.zeroCopyKey); // real TopAnchored policy
    c.tmgr_.resources.erase(parent.surfaceKey);assert(!includes());c.tmgr_.resources[parent.surfaceKey]=&parent;
    source.ownerSurfaceKey=Key(300,25);assert(!includes());source.ownerSurfaceKey=child.surfaceKey;
    c.tmgr_.hasParent=true;c.tmgr_.parent.frame=false;c.tmgr_.inZOrder=true;assert(includes());
    c.tmgr_.parent.hidden=true;assert(!includes());c.tmgr_.parent.hidden=false;
    c.tmgr_.parent.minimized=true;assert(!includes());c.tmgr_.parent.minimized=false;
    c.fullscreen=9;assert(!includes());c.fullscreen=0;
    c.tmgr_.hasParent=false;c.tmgr_.inZOrder=false;c.fullscreen=9;assert(!includes());
    puts("Scene: protocol-only/frameless parent, authoritative anchor, foreign identity, hidden/minimized and fullscreen passed");
}
'''


def main():
    presenter = read('entry/src/main/cpp/graphics/virgl_surface_presenter.cpp')
    bridge = read('entry/src/main/cpp/compositor/frame/zc_bridge.cpp')
    scene = read('entry/src/main/cpp/compositor/toplevel/desktop_compositor.cpp')
    start = bridge.index('    // L0：已有绑定')
    end = bridge.index('\n    SurfaceData* candidate', start)
    consume = function(bridge, ('void' if options.baseline else 'bool') + ' ZcBridge::NoteLayerConsumed(').replace('ZcBridge::','Bridge::')
    if options.baseline:
        consume = consume.replace('uint64_t nowUs)', 'uint64_t nowUs, uint64_t bindingGeneration)')
        consume = consume.replace('void Bridge::', 'bool Bridge::').replace('if (!surfaceKey) return;', 'if (!surfaceKey) return false;')
        consume = consume.replace('{', '{\n    (void)bindingGeneration;', 1)
        consume = consume[:-1] + 'return true;\n}'
    cleanup = function(bridge, 'void ZcBridge::InvalidateBindingsForSurface(').replace('ZcBridge::','Bridge::')
    resolve = function(bridge, 'bool ZcBridge::ResolvePresentBinding(')
    creation = resolve[resolve.index('    WineHuaPresentBinding binding;'):]
    create = '''bool Bridge::Create(uint64_t surfaceKey,bool takeoverCandidate,WineHuaPresentBinding* outBinding) {
        SurfaceData owner; SurfaceData* candidate=&owner;
        const uint32_t presenterHostPid=surfaceKey>>32,presentSurfaceId=surfaceKey;
        const uint32_t frameWidth=1200,frameHeight=800;
        const char* reason="test"; bool* outNewlyBound=nullptr;
        const auto windowKeyOf=[](const SurfaceData* sd){return Key(sd->clientPid,sd->protocolId);};
    ''' + creation

    tests = {
        'venus': VENUS + function(presenter, '    int PresentVenus(uint32_t contextId,') + VENUS_END,
        'binding': BIND + bridge[start:end] + '\nreturn false;\n}\n};\n' + consume + cleanup + create + BIND_END,
        'scene': SCENE + function(scene, 'bool DesktopCompositor::SnapshotGpuDesktopScene(') + SCENE_END,
    }
    expected_assertions = {
        'venus': 'present(p,300)==-EAGAIN',
        'binding': 'out.pending && !b.presentBindings_[old].retired',
        'scene': 'includes()',
    }
    with tempfile.TemporaryDirectory(prefix='steam-gpu-contract-') as tmp:
        for name, source in tests.items():
            cpp = Path(tmp) / (name + '.cpp'); cpp.write_text(source)
            exe = cpp.with_suffix('')
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread',
                            '-I',str(ROOT/'entry/src/main/cpp'),str(cpp),'-o',str(exe)],check=True)
            result = subprocess.run([str(exe)],capture_output=True,text=True)
            if options.baseline:
                assert result.returncode != 0 and expected_assertions[name] in result.stderr, name + ': ' + result.stderr
                print(name + ': baseline assertion reproduced')
            else:
                print(result.stdout,end='')
                assert result.returncode == 0, result.stderr


if __name__ == '__main__':
    main()
