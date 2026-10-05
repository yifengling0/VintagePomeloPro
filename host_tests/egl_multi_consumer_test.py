"""Run production NativeImage queue ownership and ordered mixed-layer drawing.

Platform stand-ins validate lifecycle, callbacks, alpha and upload reuse;
real NativeImage/driver behavior still requires tablet verification.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CPP = (ROOT / 'entry/src/main/cpp/graphics/egl_renderer.cpp').read_text()
HEADER = (ROOT / 'entry/src/main/cpp/graphics/egl_renderer.h').read_text()

def method(signature):
    start = CPP.index(signature)
    brace = CPP.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (CPP[end] == '{') - (CPP[end] == '}')
        end += 1
    return CPP[start:end]

state_start = HEADER.index('    struct ZeroCopyConsumer {')
state_end = HEADER.index('\n    std::vector<std::unique_ptr<ZeroCopyConsumer>>', state_start)
state = HEADER[state_start:state_end]

STUB = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <memory>
#include <unordered_set>
#include "compositor/frame/geometry.h"
#include "common/texture_upload_stats.h"
#include "common/frame_loop_diagnostics.h"
#include "compositor/frame/gpu_desktop_scene.h"
#include "compositor/frame/zc_bridge.h"
using GLuint=unsigned; using GLint=int;
constexpr int GL_TEXTURE_EXTERNAL_OES=1, GL_TEXTURE_2D=2, GL_LINEAR=3,
GL_CLAMP_TO_EDGE=4, GL_TEXTURE_MIN_FILTER=5, GL_TEXTURE_MAG_FILTER=6,
GL_TEXTURE_WRAP_S=7, GL_TEXTURE_WRAP_T=8, GL_ONE=9, GL_ONE_MINUS_SRC_ALPHA=10,
GL_SCISSOR_TEST=11, GL_BLEND=12, GL_ARRAY_BUFFER=13, GL_FLOAT=14,
GL_FALSE=0, GL_TRIANGLES=15, GL_RGBA=16, GL_UNSIGNED_BYTE=17, GL_COLOR_BUFFER_BIT=18,
NATIVEBUFFER_USAGE_HW_RENDER=1, NATIVEBUFFER_USAGE_HW_TEXTURE=2, GET_BUFFERQUEUE_SIZE=0;
struct OHNativeWindow {};
struct OH_OnFrameAvailableListener { void* context=nullptr; void (*onFrameAvailable)(void*)=nullptr; };
struct OH_NativeImage { OH_OnFrameAvailableListener listener; OHNativeWindow window; unsigned updates=0; int result=0,transformResult=0; };
static unsigned nextTexture=100, boundTexture=0, boundTarget=0, uploads=0;
static bool blend=false; static float forceOpaque=0;
static std::array<float,4> framebuffer{};
static std::array<float,4> clearColor{};
static bool raster=false, scissored=false;
static std::array<int,4> viewport{}, scissor{};
static std::array<std::array<float,4>,160> rasterPixels;
static unsigned clears=0;
static std::unordered_map<unsigned,std::array<float,4>> colors;
static void glGenTextures(int n,GLuint* p) { while(n--) *p++=nextTexture++; }
static void glDeleteTextures(int n,const GLuint* p) { while(n--) colors.erase(*p++); }
static void glBindTexture(int t,GLuint p) { boundTarget=t; boundTexture=p; }
static void glTexParameteri(int,int,int) {}
static void glBindBuffer(int,GLuint) {}
static void glEnableVertexAttribArray(int) {}
static void glVertexAttribPointer(int,int,int,int,int,const void*) {}
static void glViewport(int x,int y,int w,int h) { viewport={x,y,w,h}; }
static void glScissor(int x,int y,int w,int h) { scissor={x,y,w,h}; }
static void glBlendFunc(int a,int b) { assert(a==GL_ONE && b==GL_ONE_MINUS_SRC_ALPHA); }
static void glEnable(int c) { if(c==GL_BLEND) blend=true; if(c==GL_SCISSOR_TEST)scissored=true; }
static void glDisable(int c) { if(c==GL_BLEND) blend=false; if(c==GL_SCISSOR_TEST)scissored=false; }
static bool inside(const std::array<int,4>& rect,int x,int y) {
    return x>=rect[0]&&y>=rect[1]&&x<rect[0]+rect[2]&&y<rect[1]+rect[3];
}
[[maybe_unused]] static void glClearColor(float r,float g,float b,float a) { clearColor={r,g,b,a}; }
[[maybe_unused]] static void glClear(int mask) {
    assert(mask==GL_COLOR_BUFFER_BIT);++clears;framebuffer=clearColor;
    if(raster)for(int y=0;y<10;y++)for(int x=0;x<16;x++)
        if(!scissored||inside(scissor,x,y))rasterPixels[y*16+x]=clearColor;
}
static void glUseProgram(GLuint) {}
static GLint glGetUniformLocation(GLuint,const char*) { return 0; }
static void glUniform1i(int,int) {}
static void glUniform1f(int,float f) { forceOpaque=f; }
static void glUniformMatrix4fv(int,int,int,const float*) {}
static void upload(const void* p) { auto* b=static_cast<const uint8_t*>(p); colors[boundTexture]={b[2]/255.f,b[1]/255.f,b[0]/255.f,b[3]/255.f}; ++uploads; }
static void glTexImage2D(int,int,int,int,int,int,int,int,const void* p) { upload(p); }
static void glTexSubImage2D(int,int,int,int,int,int,int,int,const void* p) { upload(p); }
static void glDrawArrays(int,int,int) {
    auto c=colors.at(boundTexture);
    if(boundTarget==GL_TEXTURE_2D && forceOpaque>0.5f) c[3]=1.f;
    for(int i=0;i<4;++i) framebuffer[i]=c[i]+(blend ? framebuffer[i]*(1.f-c[3]) : 0.f);
    if(raster)for(int y=0;y<10;y++)for(int x=0;x<16;x++)
        if(inside(viewport,x,y)&&(!scissored||inside(scissor,x,y)))
            for(int i=0;i<4;i++)rasterPixels[y*16+x][i]=c[i]+(blend?rasterPixels[y*16+x][i]*(1.f-c[3]):0.f);
}
static OH_NativeImage* OH_NativeImage_Create(GLuint,int) { return new OH_NativeImage; }
static int OH_ConsumerSurface_SetDefaultSize(OH_NativeImage*,int,int) { return 0; }
static int OH_ConsumerSurface_SetDefaultUsage(OH_NativeImage*,int) { return 0; }
static int OH_NativeImage_SetDropBufferMode(OH_NativeImage*,bool) { return 0; }
static int OH_NativeImage_SetOnFrameAvailableListener(OH_NativeImage* i,OH_OnFrameAvailableListener l) { i->listener=l; return 0; }
static int OH_NativeImage_UnsetOnFrameAvailableListener(OH_NativeImage* i) { i->listener={}; return 0; }
static void OH_NativeImage_Destroy(OH_NativeImage** i) { assert(!(*i)->listener.context); delete *i; *i=nullptr; }
static OHNativeWindow* OH_NativeImage_AcquireNativeWindow(OH_NativeImage* i) { return &i->window; }
static void OH_NativeWindow_NativeWindowHandleOpt(OHNativeWindow*,int,int* s) { *s=3; }
static int OH_NativeImage_UpdateSurfaceImage(OH_NativeImage* i) { ++i->updates; return i->result; }
static int OH_NativeImage_GetTransformMatrixV2(OH_NativeImage* i,float*) { return i->transformResult; }
static int64_t OH_NativeImage_GetTimestamp(OH_NativeImage* i) { return i->updates*16666667LL; }
static uint64_t clockUs=1000000;
static unsigned clockCalls=0;
static uint64_t PerfNowUs() { ++clockCalls; return clockUs++; }
static bool FrameTraceEnabled() { return false; }
template<class... T> void TestLog(T&&...) {}
#define OH_LOG_INFO(...) TestLog(__VA_ARGS__)
#define OH_LOG_WARN(...) TestLog(__VA_ARGS__)
#define LOG_APP 0
struct Bridge {
    std::unordered_map<uint64_t,uint64_t> signals;
    std::unordered_set<uint64_t> active;
    unsigned consumes=0; bool acceptConsume=true;
    void NoteProducerPresent(uint64_t k,uint64_t t) { signals[k]=t; }
    void BindSurface(uint64_t,uint64_t) {}
    bool ConfirmFallback(uint64_t,uint64_t) { return false; }
    bool IsReadyPublished(uint64_t k) { return active.count(k); }
    bool IsFallbackPending(uint64_t) { return false; }
    void BeginFallback(uint64_t,uint64_t,bool,uint32_t) {}
    uint64_t GetFallbackShmSerial(uint64_t) { return 0; }
    bool NoteLayerConsumed(uint64_t,uint64_t,uint64_t,int,int) { ++consumes; return acceptConsume; }
    void CancelFallback(uint64_t) {}
    void Activate(uint64_t k,uint32_t) { active.insert(k); }
    void Release(uint64_t k,uint32_t) { active.erase(k); }
};
struct DesktopCompositor {
    Bridge bridge; std::unordered_map<uint64_t,ZeroCopyLayerInfo> geometry;
    bool GetZeroCopyLayerInfo(uint64_t k,uint32_t,int,int,ZeroCopyLayerInfo& out) {
        if(!geometry.count(k)) return false;
        out=geometry.at(k); return true;
    }
    struct PolicyValue { bool RootCompositing() const { return true; } } policy;
    PolicyValue& Policy() { return policy; }
    uint32_t DesktopRootToplevelId() { return 1; }
    Bridge& zc() { return bridge; }
};
namespace winehua {
struct ZeroCopySurfaceInfo { uint64_t surfaceKey=0; uint32_t clientPid=1,surfaceId=0,width=1200,height=800; bool attached=false,vulkan=true; };
struct GraphicsBroker {
    std::vector<ZeroCopySurfaceInfo> candidates; std::unordered_set<uint64_t> attached; uint64_t reject=0;
    static GraphicsBroker& GetInstance() { static GraphicsBroker b; return b; }
    bool QueryZeroCopySurfaces(std::vector<ZeroCopySurfaceInfo>& out) { out=candidates; for(auto& s:out) s.attached=attached.count(s.surfaceKey); return true; }
    bool AttachZeroCopyTarget(uint64_t k,OHNativeWindow*,uint64_t,bool) { if(k==reject) return false; return attached.insert(k).second; }
    void DetachZeroCopyTarget(uint64_t k) { attached.erase(k); }
};
}
class EglRenderer {
public:
STATE
    DesktopCompositor compositor_; uint32_t toplevelId_=1;
    std::atomic<long long> vsyncPeriodNs_{16666667}; bool zeroCopySceneDirty_=false;
    GLuint zeroCopyProgram_=2,program_=1,vbo_=1; GLint zeroCopyTransformLocation_=0;
    uint64_t zeroCopyLastQueryUs_=0,zeroCopyDiagLastUs_=0;
    std::vector<std::unique_ptr<ZeroCopyConsumer>> zeroCopyConsumers_;
    GpuDesktopScene zeroCopyScene_; FitRect letterbox_; int frameH_=800;
    struct ShmLayerTexture { GLuint texture=0; std::shared_ptr<const std::vector<uint8_t>> pixels; int width=0,height=0; };
    std::unordered_map<uint64_t,ShmLayerTexture> zeroCopyShmTextures_;
    static void OnZeroCopyFrameAvailable(void*);
    bool TryAttachZeroCopySurface(uint32_t);
    bool UpdateZeroCopyFrame(ZeroCopyConsumer&,int&,int&);
    void ReleaseZeroCopyBinding(ZeroCopyConsumer&);
    void DrawZeroCopyScene(winehua::TextureUploadStats* uploads=nullptr); void ClearZeroCopyShmTextures();
};
'''

TEST = r'''
int main() {
    EglRenderer r; auto& broker=winehua::GraphicsBroker::GetInstance();
    for(uint64_t k : {37,163,200}) {
        winehua::ZeroCopySurfaceInfo s; s.surfaceKey=k; s.surfaceId=k; broker.candidates.push_back(s);
        ZeroCopyLayerInfo info; info.surfaceKey=k; info.width=1200; info.height=800; r.compositor_.geometry[k]=info;
    }
    broker.reject=200;
    assert(r.TryAttachZeroCopySurface(1)); assert(r.zeroCopyConsumers_.size()==2 && broker.attached.size()==2);
    auto& a=*r.zeroCopyConsumers_[0]; auto& b=*r.zeroCopyConsumers_[1];
    assert(a.image!=b.image && a.texture!=b.texture);
    assert(a.image->listener.context==&a && b.image->listener.context==&b);
    a.image->listener.onFrameAvailable(a.image->listener.context);
    assert(a.frameSignals==1 && b.frameSignals==0 && !b.frameAvailable);
    b.image->listener.onFrameAvailable(b.image->listener.context);
    assert(r.compositor_.bridge.signals.count(37) && r.compositor_.bridge.signals.count(163));
    int w=0,h=0; assert(r.UpdateZeroCopyFrame(a,w,h)); assert(r.UpdateZeroCopyFrame(b,w,h));
    assert(a.frames==1 && b.frames==1 && !a.frameAvailable && !b.frameAvailable);
    assert(!r.UpdateZeroCopyFrame(a,w,h));
    assert(r.compositor_.bridge.consumes==2);
    a.image->transformResult=-1; EglRenderer::OnZeroCopyFrameAvailable(&a);
    assert(!r.UpdateZeroCopyFrame(a,w,h) && r.compositor_.bridge.consumes==2);
    a.image->transformResult=0; EglRenderer::OnZeroCopyFrameAvailable(&a);
    assert(r.UpdateZeroCopyFrame(a,w,h) && r.compositor_.bridge.consumes==3);
    auto* survivor=b.image; r.compositor_.geometry.erase(37); clockUs+=200000; r.TryAttachZeroCopySurface(1);
    assert(r.zeroCopyConsumers_.size()==1 && r.zeroCopyConsumers_[0]->image==survivor);
    assert(!broker.attached.count(37) && broker.attached.count(163));
    auto& alive=*r.zeroCopyConsumers_[0]; alive.image->result=-1;
    for(int i=0;i<2;++i) { EglRenderer::OnZeroCopyFrameAvailable(&alive); assert(!r.UpdateZeroCopyFrame(alive,w,h)); }
    assert(!alive.registered && !alive.image && !broker.attached.count(163));
    assert(r.compositor_.bridge.consumes==3);
    clockUs+=200000; r.TryAttachZeroCopySurface(1); assert(r.zeroCopyConsumers_.size()==1 && r.zeroCopyConsumers_[0]->registered);
    auto& content=*r.zeroCopyConsumers_[0]; content.hasFrame=true; colors[content.texture]={1,0,0,1};
    GpuDesktopLayer base; base.key=1; base.parentToplevel=1; base.w=base.h=base.sourceW=base.sourceH=1;
    base.pixels=std::make_shared<const std::vector<uint8_t>>(std::initializer_list<uint8_t>{0,0,0,0});
    auto win=base; win.key=2; win.parentToplevel=5;
    auto menu=base; menu.key=3; menu.parentToplevel=5; menu.subsurface=true; menu.ownerSurfaceKey=300; menu.opaque=false;
    menu.pixels=std::make_shared<const std::vector<uint8_t>>(std::initializer_list<uint8_t>{0,128,0,128});
    auto upper=base; upper.key=4; upper.parentToplevel=6;
    upper.pixels=std::make_shared<const std::vector<uint8_t>>(std::initializer_list<uint8_t>{255,0,0,0});
    auto gpu=base; gpu.pixels.reset(); gpu.zeroCopyKey=content.surfaceKey; gpu.parentToplevel=5; gpu.subsurface=true; gpu.ownerSurfaceKey=163;
    r.zeroCopyScene_.layers={base,win,menu}; MergeZeroCopySceneLayers(r.zeroCopyScene_,{gpu});
    assert(r.zeroCopyScene_.layers.size()==4 && r.zeroCopyScene_.layers[2].zeroCopyKey==163 && r.zeroCopyScene_.layers[3].key==3);
    winehua::TextureUploadStats measured;
    const auto clocksBefore=clockCalls;
    r.DrawZeroCopyScene(&measured); assert(measured.calls==3 && measured.bytes==12 && measured.cpuUs==0);
    assert(clockCalls==clocksBefore);
    assert(framebuffer[0]>0.49f && framebuffer[0]<0.5f && framebuffer[1]>0.5f && framebuffer[2]==0);
    assert(uploads==3 && !blend); measured={};
    r.DrawZeroCopyScene(&measured); assert(uploads==3 && measured.calls==0 && measured.bytes==0);
    assert(clockCalls==clocksBefore);
    // Changed pixels issue one subimage; resizing issues one allocation.
    r.zeroCopyScene_.layers.back().pixels=std::make_shared<const std::vector<uint8_t>>(
        std::initializer_list<uint8_t>{0,128,0,128});
    measured.timeEnabled=true; r.DrawZeroCopyScene(&measured);
    assert(measured.calls==1 && measured.bytes==4 && measured.cpuUs==1 && clockCalls==clocksBefore+2);
    measured={}; r.zeroCopyScene_.layers.back().sourceW=2;
    r.zeroCopyScene_.layers.back().pixels=std::make_shared<const std::vector<uint8_t>>(8,128);
    r.DrawZeroCopyScene(&measured);assert(measured.calls==1 && measured.bytes==8 && measured.cpuUs==0);
    measured={};auto noPixels=base;noPixels.pixels.reset();r.zeroCopyScene_.layers.push_back(noPixels);
    r.DrawZeroCopyScene(&measured);assert(measured.calls==0 && measured.bytes==0);
    r.zeroCopyScene_.layers.pop_back();
    auto same=r.zeroCopyScene_; assert(SameGpuDesktopScene(same,r.zeroCopyScene_)); same.layers.back().x=7; assert(!SameGpuDesktopScene(same,r.zeroCopyScene_));
    r.zeroCopyScene_.layers.push_back(upper); r.DrawZeroCopyScene(); assert(framebuffer[2]==1 && framebuffer[0]==0 && framebuffer[3]==1);
    auto popup=gpu; popup.ownerSurfaceKey=300; GpuDesktopScene exact; exact.layers={base,win,menu,upper}; MergeZeroCopySceneLayers(exact,{popup});
    assert(exact.layers[2].zeroCopyKey==163 && exact.layers[3].key==4);
    popup.external=true; popup.ownerSurfaceKey=301; GpuDesktopScene external; external.layers={base,win,upper}; MergeZeroCopySceneLayers(external,{popup});
    assert(external.layers.back().zeroCopyKey==163);
    // Query discovery order must not put the full native window over its child.
    auto nativeWindow=gpu; nativeWindow.subsurface=false; nativeWindow.zeroCopyKey=37;
    GpuDesktopScene siblings; siblings.layers={base,win,menu,upper};
    MergeZeroCopySceneLayers(siblings,{gpu,nativeWindow});
    assert(siblings.layers[2].zeroCopyKey==37 && siblings.layers[3].zeroCopyKey==163);
    assert(siblings.layers[4].key==3 && siblings.layers[5].key==4);
    auto mixed = r.zeroCopyScene_.layers;
    r.zeroCopyScene_.layers={gpu}; measured={};
    r.DrawZeroCopyScene(&measured);assert(measured.calls==0 && measured.bytes==0);
    r.zeroCopyScene_.layers=mixed;
    // Execute the real EGL layer loop against spatial GL boundaries. Clearing
    // honors scissor, not viewport; both black bars and upper menus are checked.
    raster=true;r.frameH_=8;
    r.letterbox_={};r.letterbox_.srcW=r.letterbox_.dstW=12;
    r.letterbox_.srcH=r.letterbox_.dstH=8;r.letterbox_.offX=2;r.letterbox_.offY=1;r.letterbox_.scale=1;
    auto desktop=base;desktop.w=12;desktop.h=8;
    desktop.pixels=std::make_shared<const std::vector<uint8_t>>(std::initializer_list<uint8_t>{255,0,0,255});
    GpuDesktopLayer black;black.solidBlack=true;black.w=12;black.h=8;
    auto fitted=gpu;fitted.x=1;fitted.y=0;fitted.w=10;fitted.h=8;
    auto popupMenu=menu;popupMenu.x=5;popupMenu.y=3;popupMenu.w=2;popupMenu.h=2;
    r.zeroCopyScene_.layers={desktop,black,fitted,popupMenu};
    rasterPixels.fill({1,0,1,1});measured={};
    const auto clearsBefore=clears;r.DrawZeroCopyScene(&measured);
    assert(clears==clearsBefore+1&&"EGL must draw solidBlack layers without a texture upload");
    assert((rasterPixels[4*16+2]==std::array<float,4>{0,0,0,1})); // left bar
    assert((rasterPixels[4*16+13]==std::array<float,4>{0,0,0,1})); // right bar
    assert((rasterPixels[2*16+4]==std::array<float,4>{1,0,0,1})); // game
    assert(rasterPixels[4*16+7][0]>0.49f&&rasterPixels[4*16+7][1]>0.5f); // menu blends above
    assert((rasterPixels[0]==std::array<float,4>{1,0,1,1})); // outer display border retained
    assert(!scissored&&!blend);
    content.hasFrame=false;r.DrawZeroCopyScene();
    assert((rasterPixels[2*16+4]==std::array<float,4>{0,0,0,1})); // no frame leaks desktop
    content.hasFrame=true;r.zeroCopyScene_.layers={desktop,fitted};r.DrawZeroCopyScene();
    assert((rasterPixels[4*16+2]==std::array<float,4>{0,0,1,1})); // leaving fullscreen restores desktop
    raster=false;
    // The generation captured at attach cannot consume a recreated binding.
    r.compositor_.geometry[163].bindingGeneration=2;
    EglRenderer::OnZeroCopyFrameAvailable(&content);
    assert(!r.UpdateZeroCopyFrame(content,w,h) && !content.registered);
    assert(r.compositor_.bridge.consumes==3);
    clockUs+=200000; assert(r.TryAttachZeroCopySurface(1));
    auto& reused=*r.zeroCopyConsumers_[0];assert(reused.layer.bindingGeneration==2);
    r.compositor_.bridge.acceptConsume=false;EglRenderer::OnZeroCopyFrameAvailable(&reused);
    assert(!r.UpdateZeroCopyFrame(reused,w,h) && !reused.registered && !reused.hasFrame);
    r.ReleaseZeroCopyBinding(reused); r.ClearZeroCopyShmTextures();
    puts("Production multi-consumer: attach isolation, independent signals, retirement, failure recovery, mixed alpha composition and texture reuse passed");
}
'''

with tempfile.TemporaryDirectory(prefix='vp-egl-multi-') as folder:
    folder = Path(folder)
    source = folder / 'test.cpp'
    signatures = ['static void ComposeZeroCopySamplingTransform(', 'void EglRenderer::OnZeroCopyFrameAvailable(',
                  'bool EglRenderer::TryAttachZeroCopySurface(', 'bool EglRenderer::UpdateZeroCopyFrame(',
                  'void EglRenderer::ReleaseZeroCopyBinding(', 'void EglRenderer::DrawZeroCopyScene(',
                  'void EglRenderer::ClearZeroCopyShmTextures(']
    source.write_text(STUB.replace('STATE', state) + '\n'.join(method(s) for s in signatures) + TEST)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread', '-I',
                    str(ROOT / 'entry/src/main/cpp'), str(source), '-o', str(folder / 'test')], check=True)
    subprocess.run([str(folder / 'test')], check=True)
