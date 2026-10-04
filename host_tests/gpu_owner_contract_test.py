"""Full production owner-chain regression. Platform resources only are substitutes."""
import argparse
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--baseline', action='store_true')
parser.add_argument('--baseline-ref', default='87b0c5c1ec44a9700faed44217b078ea67de8533')
args=parser.parse_args()
path='entry/src/main/cpp/compositor/frame/zc_bridge.cpp'
source=(subprocess.check_output(['git','show',args.baseline_ref+':'+path],cwd=ROOT).decode() if args.baseline else (ROOT/path).read_text())
def function(signature):
 start=source.index(signature); brace=source.index('{',start); depth=1; end=brace+1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]

STUB = r'''
#include <cassert>
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
template<class... T> void Log(T&&...) {}
#define LOG_APP 0
#define OH_LOG_INFO(...) Log(__VA_ARGS__)
#define OH_LOG_WARN(...) Log(__VA_ARGS__)
struct SurfaceData;struct wl_resource{SurfaceData* data;};
struct SurfaceData{uint32_t clientPid=100,protocolId=151,toplevelId=5;uint64_t surfaceKey=0;bool hasToplevel=true,isSubsurface=false;
wl_resource* parentSurface=nullptr;int w=101,h=398,vpDstW=0,vpDstH=0,subsurfaceX=0,subsurfaceY=0;
struct{struct{int x=0,y=0;}contentRect;}committed;std::atomic<uint64_t> shmCommitSerial{0};};
void* wl_resource_get_user_data(wl_resource* p){return p?p->data:nullptr;}
struct State{bool IsMinimized()const{return false;}bool IsBackground()const{return false;}bool IsFullscreen()const{return false;}
int Width()const{return 101;}int Height()const{return 398;}int X()const{return 0;}int Y()const{return 0;}int WineX()const{return 0;}int WineY()const{return 0;}};
struct Manager{State state;std::mutex m;std::unordered_map<uint64_t,wl_resource*>resources;
auto Lock(){return std::unique_lock<std::mutex>(m);}wl_resource* FindSurfaceResource(uint64_t k){auto it=resources.find(k);return it==resources.end()?nullptr:it->second;}
const auto& SurfaceResources(){return resources;}State* FindToplevelLocked(uint32_t id){return id==5?&state:nullptr;}
bool IsToplevelVisibleLocked(uint32_t id,uint32_t){return id==5;}};
struct Layer{wl_resource* surface=nullptr;int vpDstW=0,vpDstH=0,w=0,h=0;uint64_t shmCommitSerial=0;uint32_t parentToplevel=5;bool isExternal=false;};
struct Comp{Manager tmgr_;uint32_t desktopRootToplevelId_=1;struct{bool RootCompositing(){return true;}}policy_;std::vector<Layer>subsurfaceLayers_;
void ResolveSubsurfaceLayerPositionLocked(const Layer&,int&,int&){};};
enum class ZeroCopySource{ShmLayer,ProtocolOnly};
struct ZeroCopyLayerInfo{uint64_t surfaceKey=0,bindingGeneration=0,shmCommitSerial=0;uint32_t clientPid=0,surfaceId=0,parentToplevel=0;
int width=0,height=0,x=0,y=0;bool subsurface=false,desktopCoordinates=false,fullscreen=false,external=false;ZeroCopySource source=ZeroCopySource::ShmLayer;};
int DisplaySizeAfterViewport(int vp,int d){return vp>0?vp:d;}void CompensateMinimizedSubsurfaceOffset(const State*,int&,int&){}
auto ComputePopupOffset(int x,int y,int px,int py){return std::pair<int,int>{x-px,y-py};}
struct WineHuaPresentBinding{struct{uint32_t presenterHostPid=0,presentSurfaceId=0,producerGeneration=0;}producer;
struct{uint32_t ownerHostPid=0,wlSurfaceId=0,windowGeneration=0;}window;uint64_t bindGeneration=0,lastProducerUs=0,lastDrawUs=0;const char* reason=nullptr;uint32_t frameWidth=0,frameHeight=0;bool retired=false,pending=false;};
struct Diag{uint64_t firstUs=0,lastUs=0;uint32_t frames=0,width=0,height=0;bool bound=false;char lastReject[128]{};};
class ZcBridge{public:Comp comp_;uint64_t nextBindingGeneration_=0;
std::unordered_map<uint64_t,WineHuaPresentBinding>presentBindings_;std::unordered_map<uint64_t,uint64_t>windowBindings_;std::unordered_set<uint64_t>bindingRejectedLogged_;std::unordered_map<uint64_t,Diag>bindDiagProducers_;
std::unordered_map<uint64_t,uint64_t> lastPresentUsByKey_;std::mutex presentLivenessMutex_;
struct ConsumedContent{int width,height;uint64_t bindingGeneration;};std::unordered_map<uint64_t,ConsumedContent> consumedSizes_;
uint64_t LastPresentUs(uint64_t k){return lastPresentUsByKey_[k];}
void PruneStaleWindowBindings();
bool NoteLayerConsumed(uint64_t,uint64_t,uint64_t,int=0,int=0);
void InvalidateBindingsForSurface(uint32_t,uint32_t);
void BindDiagPass(uint64_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t){}
bool ResolvePresentBinding(uint64_t,uint32_t,uint32_t,WineHuaPresentBinding*,bool*);
bool GetLayerInfo(uint64_t,uint32_t,int,int,ZeroCopyLayerInfo&,const char**);};
void BindDiagEmit(const char*){}
static uint64_t Key(uint32_t pid,uint32_t id){return(uint64_t(pid)<<32)|id;}
'''

CASE = r'''
int main(int argc,char** argv){
 const int mode=argc>1?atoi(argv[1]):0;
 ZcBridge b;SurfaceData owner,producer,peer,menu;
 producer.protocolId=154;producer.toplevelId=0;producer.hasToplevel=false;producer.w=producer.h=1;
 wl_resource ownerRes{&owner},producerRes{&producer},peerRes{&peer},menuRes{&menu};
 const auto ownerKey=Key(100,151),producerKey=Key(100,154),oldKey=Key(100,201);
 b.comp_.tmgr_.resources[ownerKey]=&ownerRes;b.comp_.tmgr_.resources[producerKey]=&producerRes;
 ZeroCopyLayerInfo info;const char* reason=nullptr;
 if(mode==0){assert(!b.GetLayerInfo(producerKey,1,1,1,info,&reason) && "small role-less producer rejected");assert(b.presentBindings_.empty());}
 if(mode==1){WineHuaPresentBinding old;old.window.ownerHostPid=100;old.window.wlSurfaceId=151;old.bindGeneration=42;b.presentBindings_[oldKey]=old;b.windowBindings_[ownerKey]=oldKey;
 assert(!b.GetLayerInfo(producerKey,1,101,398,info,&reason) && "active owner rejection is terminal");assert(b.windowBindings_[ownerKey]==oldKey);assert(!b.presentBindings_.count(producerKey));
 // Only real stale producer activity permits pending takeover; query alone does not retire.
 b.lastPresentUsByKey_[oldKey]=1;assert(b.GetLayerInfo(producerKey,1,101,398,info,&reason));const auto gen=info.bindingGeneration;
 assert(gen && b.presentBindings_[producerKey].pending && !b.presentBindings_[oldKey].retired);
 assert(b.GetLayerInfo(producerKey,1,101,398,info,&reason));assert(!b.presentBindings_[oldKey].retired);
 assert(!b.NoteLayerConsumed(producerKey,10,gen+1));assert(b.presentBindings_[producerKey].pending);
 assert(b.NoteLayerConsumed(producerKey,11,gen));assert(b.presentBindings_[oldKey].retired);
 assert(!b.GetLayerInfo(oldKey,1,101,398,info,&reason));
 b.InvalidateBindingsForSurface(100,154);assert(b.GetLayerInfo(producerKey,1,101,398,info,&reason));
 assert(info.bindingGeneration>gen);assert(!b.NoteLayerConsumed(producerKey,12,gen));}
 if(mode==2){producer.isSubsurface=true;producer.parentSurface=&ownerRes;assert(b.GetLayerInfo(producerKey,1,1,1,info,&reason));assert(info.subsurface&&info.width==1&&info.height==1&&info.bindingGeneration==0);assert(b.presentBindings_.empty());}
 if(mode==3){peer.protocolId=152;b.comp_.tmgr_.resources[Key(100,152)]=&peerRes;
 WineHuaPresentBinding old;old.window.ownerHostPid=100;old.window.wlSurfaceId=151;old.bindGeneration=42;b.presentBindings_[oldKey]=old;b.windowBindings_[ownerKey]=oldKey;
 assert(!b.GetLayerInfo(producerKey,1,101,398,info,&reason) && "claimed peer cannot hide ambiguity");}
 if(mode==4){assert(b.GetLayerInfo(producerKey,1,101,398,info,&reason));const auto gen=info.bindingGeneration;
 assert(gen && info.surfaceId==151);owner.w=202;owner.h=796;
 assert(b.GetLayerInfo(producerKey,1,202,796,info,&reason));assert(info.bindingGeneration==gen && info.width==202);
 assert(b.GetLayerInfo(producerKey,1,0,0,info,&reason));assert(info.bindingGeneration==gen);}
 if(mode==5){menu.protocolId=160;menu.hasToplevel=false;menu.isSubsurface=true;menu.parentSurface=&ownerRes;menu.w=31;menu.h=78;menu.vpDstW=62;menu.vpDstH=156;b.comp_.tmgr_.resources[Key(100,160)]=&menuRes;
 assert(b.GetLayerInfo(producerKey,1,31,78,info,&reason));assert(info.subsurface&&info.surfaceId==160&&info.width==62&&info.height==156);}
 if(mode==6){peer.clientPid=200;peer.protocolId=152;b.comp_.tmgr_.resources[Key(200,152)]=&peerRes;
 assert(b.GetLayerInfo(producerKey,1,101,398,info,&reason));assert(info.clientPid==100);}
 if(mode==7){owner.clientPid=200;b.comp_.tmgr_.resources.erase(ownerKey);b.comp_.tmgr_.resources[Key(200,151)]=&ownerRes;
 assert(b.GetLayerInfo(producerKey,1,101,398,info,&reason));assert(info.clientPid==200&&info.bindingGeneration);}
 if(mode==8){owner.clientPid=200;peer.clientPid=300;peer.protocolId=152;b.comp_.tmgr_.resources.erase(ownerKey);b.comp_.tmgr_.resources[Key(200,151)]=&ownerRes;b.comp_.tmgr_.resources[Key(300,152)]=&peerRes;
 assert(!b.GetLayerInfo(producerKey,1,101,398,info,&reason));}
 puts("full GetLayerInfo owner contract passed");
}
'''

functions=''.join(function('bool ZcBridge::'+name+'(') for name in ['ResolvePresentBinding','GetLayerInfo','NoteLayerConsumed'])
functions+=''.join(function('void ZcBridge::'+name+'(') for name in ['PruneStaleWindowBindings','InvalidateBindingsForSurface'])
if args.baseline:
 functions=functions.replace('uint64_t bindingGeneration)\n{','uint64_t bindingGeneration, int sourceW, int sourceH)\n{\n (void)sourceW; (void)sourceH;')
with tempfile.TemporaryDirectory(prefix='owner-contract-') as d:
 p=Path(d)/'owner.cpp';p.write_text(STUB+functions+CASE)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(p.with_suffix(''))],check=True)
 for mode in range(9):
  result=subprocess.run([str(p.with_suffix('')),str(mode)],capture_output=True,text=True)
  if args.baseline and mode in (0,1,3):
   expected={0:'small role-less producer rejected',1:'active owner rejection is terminal',3:'claimed peer cannot hide ambiguity'}[mode]
   assert result.returncode!=0 and expected in result.stderr, result.stdout+result.stderr
   print('BASELINE RED (expected assertion):',expected)
  else:
   assert result.returncode==0,result.stdout+result.stderr
   print('PASS owner case',mode)
