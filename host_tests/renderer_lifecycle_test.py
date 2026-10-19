"""Execute production renderer management and background release code.
Only platform/window/Vulkan calls are faked. This is not device acceptance.
"""
from pathlib import Path
import subprocess, tempfile, sys
R=Path(__file__).resolve().parents[1]
def block(s, marker):
    a=s.index(marker);b=s.index('{',a)+1;depth=1;e=b
    while depth:
        depth+=(s[e]=='{')-(s[e]=='}');e+=1
    return s[a:e]
plugin=(R/'entry/src/main/cpp/bridge/plugin_manager.cpp').read_text()
vk=(R/'entry/src/main/cpp/direct/direct_vulkan_desktop_compositor.cpp').read_text()
prefix=r"""
#include <cassert>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <utility>
#define OH_LOG_INFO(...)
#define OH_LOG_WARN(...)
#define OH_LOG_ERROR(...)
static unsigned checks, shutdowns;
#define CHECK(x) do {++checks; assert(x);} while(0)
struct OHNativeWindow {};
static OHNativeWindow window;
static int windowResult;
static int OH_NativeWindow_CreateNativeWindowFromSurfaceId(uint64_t, OHNativeWindow** out){*out=&window;return windowResult;}
static void OH_NativeWindow_DestroyNativeWindow(OHNativeWindow*){}
struct EglRenderer {
    unsigned id=0;bool paused=false;
    explicit EglRenderer(int){}
    void SetToplevelId(unsigned x){id=x;}
    void SetPaused(bool x){paused=x;}
    bool Init(OHNativeWindow*,int,int){return true;}
    void Shutdown(){++shutdowns;}
};
struct WaylandServer {
    static WaylandServer* GetInstance(){static WaylandServer s;return &s;}
    int GetDesktopCompositor(){return 0;}
};
struct PluginManager {
    std::unordered_map<uint32_t,std::unique_ptr<EglRenderer>> toplevelRenderers_;
    std::unordered_map<uint32_t,bool> rendererPaused_;
    void CreateRenderer(uint32_t,int64_t);
    void DestroyToplevel(uint32_t);
    void MoveRendererToToplevel(uint32_t,uint32_t);
    void SetRendererPaused(uint32_t,bool);
};
"""
methods='\n'.join(block(plugin,'void PluginManager::'+n+'(') for n in
    ['CreateRenderer','DestroyToplevel','MoveRendererToToplevel','SetRendererPaused'])
management=r"""
static void management(){
 PluginManager p;
 p.SetRendererPaused(7,true);p.CreateRenderer(7,1);CHECK(p.toplevelRenderers_[7]->paused);
 p.SetRendererPaused(7,false);CHECK(!p.toplevelRenderers_[7]->paused);
 auto *original=p.toplevelRenderers_[7].get();
 p.MoveRendererToToplevel(7,99);CHECK(p.toplevelRenderers_.count(7)==0);
 CHECK(p.toplevelRenderers_[99].get()==original&&original->id==99);
 p.MoveRendererToToplevel(99,99);CHECK(p.toplevelRenderers_[99].get()==original);
 p.MoveRendererToToplevel(8,10);CHECK(p.toplevelRenderers_.count(10)==0);
 p.SetRendererPaused(100,true);p.CreateRenderer(100,2);unsigned before=shutdowns;
 p.MoveRendererToToplevel(99,100);CHECK(shutdowns==before+1);
 CHECK(p.toplevelRenderers_[100].get()==original&&original->id==100&&original->paused);
 p.MoveRendererToToplevel(100,200);CHECK(p.toplevelRenderers_[200]->paused);
 p.DestroyToplevel(200);CHECK(!p.rendererPaused_.count(200));
 p.CreateRenderer(200,3);CHECK(!p.toplevelRenderers_[200]->paused);
 // Force repeated unordered_map growth, the former source iterator bug.
 for(unsigned id=300;id<550;++id)p.CreateRenderer(id,id);
 for(unsigned id=300;id<550;++id)p.MoveRendererToToplevel(id,id+1000);
 for(unsigned id=300;id<550;++id)CHECK(p.toplevelRenderers_[id+1000]->id==id+1000);
 windowResult=1;p.SetRendererPaused(900,true);p.CreateRenderer(900,9);
 CHECK(!p.toplevelRenderers_.count(900));
 windowResult=0;p.CreateRenderer(900,9);CHECK(p.toplevelRenderers_[900]->paused);
}
"""
vkstub=r"""
using VkResult=int;using VkCommandBuffer=int;
constexpr int VK_TRUE=1,VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO=1,
 VK_STRUCTURE_TYPE_SUBMIT_INFO=2,VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT=1;
struct VkCommandBufferBeginInfo {int sType;int flags=0;};
struct VkSubmitInfo {int sType;unsigned commandBufferCount=0;const int *pCommandBuffers=nullptr;};
static unsigned waits,submits,barriers,returns,unrefs,closes;
static bool completed, injectSubmitFailure;
static int vkResetCommandBuffer(int,int){return 0;}
static int vkBeginCommandBuffer(int,const VkCommandBufferBeginInfo*){return 0;}
static int vkEndCommandBuffer(int){return 0;}
static int vkResetFences(int,int,const int*){completed=false;return 0;}
static int vkQueueSubmit(int,int,const VkSubmitInfo* info,int){CHECK(info->commandBufferCount==1);++submits;return injectSubmitFailure?-1:0;}
static int vkWaitForFences(int,int,const int*,int,uint64_t){++waits;completed=true;return 0;}
static int close(int){++closes;return 0;}
static void OH_NativeWindow_NativeObjectUnreference(void*){++unrefs;}
struct DirectImageQueue {
 enum class ReleaseStatus {Ok,RetiredByProducer,Error};
 int lastFence=-2;
 ReleaseStatus ReleaseWithStatus(void*,int fd){++returns;lastFence=fd;return ReleaseStatus::Ok;}
};
struct Buffer {void* windowBuffer=nullptr;bool owned=false;int acquireFence=-1;
 struct {std::shared_ptr<DirectImageQueue> queue;} source;
};
struct Core {
 struct Slot{int command=1,fence=2;} slot;
 int Device(){return 1;}int Queue(){return 2;}Slot& Submission(int){return slot;}
};
struct GraphicsBroker {
 static GraphicsBroker& GetInstance(){static GraphicsBroker b;return b;}
 unsigned changes=0;uint64_t period=0;
 void SetZeroCopyFramePeriod(uint64_t,uint64_t p){++changes;period=p;}
};
struct VulkanDesktopRenderer {
 Core core;Core* core_=&core;
 struct Frame{bool inFlight=false;std::vector<int> references;}frames_[2];
 struct Layer {Buffer current;};
 std::unordered_map<int,Layer> layers_;
 std::unordered_map<int,std::unique_ptr<Layer>> glLayers_;
 bool background_=false;
 int scene_=17,renderedExtent_=23;
 struct Cadence{uint64_t PeriodNs(){return 16666667;}}cadence_;
 Buffer incoming;bool remove=false,updateOk=true;
 bool UpdateSources(){
   if(!updateOk)return false;
   if(remove){for(auto& [id,l]:layers_)Retire(l.current);layers_.clear();}
   if(incoming.windowBuffer){Retire(layers_[1].current);layers_[1].current=std::move(incoming);incoming={};}
   return true;
 }
 void UpdateGlSources(){}
 void Retire(Buffer&);bool Step(bool paused);
 std::vector<Buffer> retired_;
 unsigned producerRetired_=0,releaseErrors_=0,releaseFences_=0;
 bool Check(int rc,const char*){return rc==0;}
 void Barrier(int,Buffer& b,bool acquire){CHECK(!acquire&&b.owned);++barriers;}
 bool DrainBackgroundBuffers();void Return(Buffer&,int);
};
"""
vkmethods='\n'.join(block(vk,'    '+kind+' '+name+'(').replace(name+'(', 'VulkanDesktopRenderer::'+name+'(',1)
    for kind,name in [('bool','DrainBackgroundBuffers'),('void','Return'),('void','Retire')])
# Execute the production Render background/resume preamble; all output work is
# deliberately outside this test. No replacement policy is recreated here.
preamble=vk[vk.index('    bool Render(int expectedW, int expectedH, bool paused) {'):vk.index('        PublishFrameRates();')]
vkmethods+='\n'+preamble.replace('bool Render(int expectedW, int expectedH, bool paused)', 'bool VulkanDesktopRenderer::Step(bool paused)',1)+'return true; }\n'
vktest=r"""
static void release(){
 VulkanDesktopRenderer v;auto q=std::make_shared<DirectImageQueue>();
 v.retired_.push_back({(void*)1,true,-1,{q}});
 CHECK(v.DrainBackgroundBuffers());CHECK(submits==1&&waits==1&&barriers==1);
 CHECK(returns==1&&unrefs==1&&completed&&!v.frames_[0].inFlight&&q->lastFence==-1);
 v.retired_.push_back({(void*)2,false,71,{q}});
 CHECK(v.DrainBackgroundBuffers());CHECK(submits==1&&waits==1&&barriers==1);
 CHECK(returns==2&&unrefs==2&&q->lastFence==71);CHECK(closes==0);
 CHECK(v.DrainBackgroundBuffers());CHECK(returns==2&&submits==1);
 // On failure retain ownership and buffer references for safe shutdown.
 v.retired_.push_back({(void*)3,true,-1,{q}});injectSubmitFailure=true;
 CHECK(!v.DrainBackgroundBuffers());CHECK(returns==2&&v.retired_.size()==1);
 injectSubmitFailure=false;CHECK(v.DrainBackgroundBuffers());CHECK(returns==3&&v.retired_.empty());
}
static void retainedFrame(){
 auto q=std::make_shared<DirectImageQueue>();
 VulkanDesktopRenderer v;
 v.layers_[1].current={(void*)101,true,-1,{q}};
 v.glLayers_[2]=std::make_unique<VulkanDesktopRenderer::Layer>();
 v.glLayers_[2]->current={(void*)102,true,-1,{q}};
 v.frames_[0].inFlight=true;v.frames_[1].inFlight=true;
 v.frames_[0].references={1};v.frames_[1].references={2};
 unsigned baseReturns=returns,baseBarriers=barriers,baseWaits=waits;
 CHECK(v.Step(true));
 CHECK(v.background_&&waits>=baseWaits+2);
 CHECK(v.frames_[0].references.empty()&&v.frames_[1].references.empty());
 // Static producers submit nothing on resume: keep both current images.
 for(unsigned i=0;i<30;++i)CHECK(v.Step(true));
 CHECK(returns==baseReturns&&barriers==baseBarriers);
 CHECK(v.layers_[1].current.windowBuffer==(void*)101);
 CHECK(v.glLayers_[2]->current.windowBuffer==(void*)102);
 CHECK(v.Step(false));CHECK(!v.background_&&v.scene_==0&&v.renderedExtent_==0);
 CHECK(GraphicsBroker::GetInstance().period==16666667);
 CHECK(v.layers_[1].current.windowBuffer==(void*)101&&returns==baseReturns);
 // A queue of three can advance while one latest image remains acquired.
 // Every replaced buffer must be returned exactly once. The first old image
 // was GPU-owned; newer hidden images retain their producer acquire fences.
 for(unsigned i=0;i<90;++i){
   v.incoming={(void*)(uintptr_t)(201+i%3),false,(int)(500+i),{q}};
   CHECK(v.Step(true));CHECK(returns==baseReturns+i+1);
   CHECK(v.layers_[1].current.windowBuffer==(void*)(uintptr_t)(201+i%3));
   CHECK(v.retired_.empty());
 }
 CHECK(barriers==baseBarriers+1);CHECK(q->lastFence==588);
 CHECK(v.Step(false));CHECK(v.layers_[1].current.windowBuffer==(void*)203);
 // Source destruction still returns the retained buffer with its readiness
 // fence; retaining an idle image must not leak the producer's queue.
 v.remove=true;CHECK(v.Step(true));
 CHECK(v.layers_.empty()&&returns==baseReturns+91&&q->lastFence==589);
 v.Retire(v.glLayers_[2]->current);v.glLayers_.clear();
 CHECK(v.DrainBackgroundBuffers());CHECK(returns==baseReturns+92);
 // Failed source updates retain the last valid frame for safe cleanup.
 v.layers_[1].current={(void*)401,false,777,{q}};v.updateOk=false;
 CHECK(!v.Step(true));CHECK(v.layers_[1].current.windowBuffer==(void*)401);
 v.Retire(v.layers_[1].current);CHECK(v.DrainBackgroundBuffers());
 CHECK(q->lastFence==777&&returns==baseReturns+93);
}
int main(){management();release();retainedFrame();printf("production renderer lifecycle: %u checks PASS\n",checks);}
"""
with tempfile.TemporaryDirectory(prefix='renderer-lifecycle-') as d:
 d=Path(d);unit=d/'test.cpp';unit.write_text(prefix+methods+management+vkstub+vkmethods+vktest)
 command=['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror']
 if '--asan' in sys.argv:command+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g','-no-pie']
 subprocess.run(command+[str(unit),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True,timeout=20)
# Pause gating must occur after draining, before any output snapshot/upload.
egl=(R/'entry/src/main/cpp/graphics/egl_renderer.cpp').read_text()
loop=egl[egl.index('    bool wasPaused = false;'):]
assert loop.index('UpdateZeroCopyFrame(consumer') < loop.index('        if (paused) {\n            std::unique_lock') < loop.index('SnapshotZeroCopyScene()') < loop.index('glTexSubImage2D')
paused=block(vk,'        if (paused) {')
assert 'UpdateSources()' in paused and 'UpdateGlSources()' in paused and 'DrainBackgroundBuffers()' in paused
assert all(x not in paused for x in ['vkAcquireNextImageKHR','vkQueuePresentKHR','UploadUi','vkDeviceWaitIdle'])
assert 'return !running_ || !renderPaused_.load' in loop
print('background drain/output gating and wakeup ordering PASS')
