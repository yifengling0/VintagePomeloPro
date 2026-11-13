from pathlib import Path
import argparse
import subprocess
import tempfile

parser = argparse.ArgumentParser(description='Reproduce weak owner matching with the complete production resolver')
parser.add_argument('--repo', type=Path)
args = parser.parse_args()
REPO = args.repo.resolve() if args.repo else next(
    parent for parent in Path(__file__).resolve().parents if (parent / '.git').exists())
text = (REPO / 'entry/src/main/cpp/compositor/frame/zc_bridge.cpp').read_text()
start = text.index('bool ZcBridge::ResolvePresentBinding(')
brace = text.index('{', start)
depth, end = 1, brace + 1
while depth:
    depth += (text[end] == '{') - (text[end] == '}')
    end += 1
production = text[start:end]
stub = r'''
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
template<class... T> void Log(T&&...) {}
#define LOG_APP 0
#define OH_LOG_INFO(...) Log(__VA_ARGS__)
#define OH_LOG_WARN(...) Log(__VA_ARGS__)
struct SurfaceData {uint32_t clientPid=100,protocolId=151,toplevelId=5;
    bool hasToplevel=true,isSubsurface=false;void* parentSurface=nullptr;int w=101,h=398;};
void* wl_resource_get_user_data(void* p){return p;}
struct State {bool IsMinimized()const{return false;}bool IsBackground()const{return false;}
    int Width()const{return 101;}int Height()const{return 398;}};
struct Manager {State state;std::unordered_map<uint64_t,void*> resources;
    void* FindSurfaceResource(uint64_t k){auto it=resources.find(k);return it==resources.end()?nullptr:it->second;}
    const auto& SurfaceResources(){return resources;}State* FindToplevelLocked(uint32_t id){return id==5?&state:nullptr;}};
struct Comp {Manager tmgr_;uint32_t desktopRootToplevelId_=1;};
struct WineHuaPresentBinding {struct{uint32_t presenterHostPid=0,presentSurfaceId=0,producerGeneration=0;}producer;
    struct{uint32_t ownerHostPid=0,wlSurfaceId=0,windowGeneration=0;}window;
    uint64_t bindGeneration=0,lastProducerUs=0,lastDrawUs=0;const char* reason=nullptr;
    uint32_t frameWidth=0,frameHeight=0;bool retired=false,pending=false;};
struct Diag{uint64_t firstUs=0,lastUs=0;uint32_t frames=0,width=0,height=0;bool bound=false;char lastReject[128]{};};
class ZcBridge{public:Comp comp_;uint64_t nextBindingGeneration_=0;
    std::unordered_map<uint64_t,WineHuaPresentBinding> presentBindings_;
    std::unordered_map<uint64_t,uint64_t> windowBindings_;std::unordered_set<uint64_t> bindingRejectedLogged_;
    std::unordered_map<uint64_t,Diag> bindDiagProducers_;
    void PruneStaleWindowBindings(){}uint64_t LastPresentUs(uint64_t){return 0;}
    void BindDiagPass(uint64_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t){}
    bool ResolvePresentBinding(uint64_t,uint32_t,uint32_t,WineHuaPresentBinding*,bool*);
};
void BindDiagEmit(const char*){}
'''
case = r'''
int main(){ZcBridge b;SurfaceData owner;
    const uint64_t ownerKey=(uint64_t(100)<<32)|151,producerKey=(uint64_t(100)<<32)|154;
    b.comp_.tmgr_.resources[ownerKey]=&owner;
    WineHuaPresentBinding result;bool fresh=false;
    assert(b.ResolvePresentBinding(producerKey,1,1,&result,&fresh));
    assert(fresh && result.window.ownerHostPid==100 && result.window.wlSurfaceId==151);
    assert(result.frameWidth==1 && result.frameHeight==1);
    puts("REPRODUCED: production ResolvePresentBinding accepts 1x1 producer for a 101x398 same-process sole owner.");
}
'''
with tempfile.TemporaryDirectory(prefix='weak-owner-review-') as tmp:
    source=Path(tmp)/'repro.cpp';source.write_text(stub+production+case)
    exe=source.with_suffix('')
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(source),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
