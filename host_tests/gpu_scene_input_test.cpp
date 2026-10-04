#include "compositor/toplevel/desktop_compositor.h"
#include "compositor/input/input_resolver.h"
#include "compositor/toplevel/toplevel_manager.h"
#include "compositor/frame/surface_data.h"
#include "compositor/frame/input_target_probe.h"
#include "common/frame_loop_diagnostics.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>

static uint64_t Key(uint32_t id) { return (uint64_t(100) << 32) | id; }
struct Fixture {
    ToplevelManager tm;
    DisplayPolicy policy{true};
    uint32_t root = 1;
    int32_t width = 160, height = 100;
    DesktopCompositor comp{tm, policy, root, width, height};
    InputResolver input{tm, comp, root, width, height};
    SurfaceData rootData, steamData, gameData, childData, menuData, modalData, modalChildData;
    wl_resource rootRes{&rootData}, steam{&steamData}, game{&gameData}, child{&childData},
        menu{&menuData}, modal{&modalData}, modalChild{&modalChildData};
    GpuDesktopLayer gpu;
    GpuDesktopSnapshotCache cache;
    Fixture() {
        window(rootData, rootRes, 1, true); window(steamData, steam, 4, true);
        window(gameData, game, 5, false);
        childData.surface = &child; childData.surfaceKey = Key(25); childData.protocolId = 25;
        childData.clientPid = 100; childData.isSubsurface = true; childData.parentSurface = &game;
        childData.inputRegionEmpty = true;
        tm.RegisterSurfaceResource(Key(25), &child);
        gpu.zeroCopyKey = gpu.ownerSurfaceKey = Key(25); gpu.parentToplevel = 5;
        gpu.subsurface = true; gpu.w = gpu.sourceW = width; gpu.h = gpu.sourceH = height;
        consume(Key(25), width, height); comp.zc().Activate(Key(25), 1);
    }
    void window(SurfaceData& sd, wl_resource& res, uint32_t id, bool frame) {
        sd.surface = &res; sd.surfaceKey = Key(id); sd.protocolId = id;
        sd.clientPid = 100; sd.hasToplevel = true; sd.toplevelId = id;
        tm.RegisterSurfaceResource(Key(id), &res); tm.MapToplevelSurface(id, &res);
        auto lock = tm.Lock(); auto& st = tm.EnsureToplevelLocked(id);
        st.SetContentSize(width, height); st.MarkFirstCommit(0, 0); st.ApplyFullscreen(id != root);
        if (frame) st.FrameData().resize(width * height * 4, 255);
        tm.AddToZOrder(id);
    }
    void consume(uint64_t key, int w, int h) {
#ifdef GPU_FOLLOWUP_BASELINE
        assert(comp.zc().NoteLayerConsumed(key, 1, 0));
        (void)w; (void)h;
#else
        assert(comp.zc().NoteLayerConsumed(key, 1, 0, w, h));
#endif
    }
    void check(uint32_t target, bool hasGpu) {
        GpuDesktopScene scene;
        assert(comp.SnapshotGpuDesktopScene({}, cache, scene, {gpu}));
        InputTarget hit;
        assert(input.FindInputTargetAt(width / 2, height / 2, hit));
        assert(hit.toplevelId == target && "render and real input share GPU-only candidate eligibility");
        const bool found = std::any_of(scene.layers.begin(), scene.layers.end(),
            [&](const auto& layer) { return layer.zeroCopyKey == gpu.zeroCopyKey; });
        assert(found == hasGpu);
    }
};
int main(int argc, char** argv) {
    const int mode = argc > 1 ? std::atoi(argv[1]) : 0;
    Fixture f;
    if (mode == 0) {
        f.check(5, true);
        { auto lock=f.tm.Lock(); f.tm.RaiseToplevel(4); f.tm.BumpFsPriorityLocked(4); }
        f.check(4, false);
        { auto lock=f.tm.Lock(); f.tm.RaiseToplevel(5); f.tm.BumpFsPriorityLocked(5); }
        f.check(5, true);
        { auto lock=f.tm.Lock(); f.tm.HideToplevelLocked(5); } f.check(4, false);
        { auto lock=f.tm.Lock(); f.tm.ShowToplevelLocked(5); f.tm.FindToplevelLocked(5)->SetMinimized(true); } f.check(4, false);
        { auto lock=f.tm.Lock(); f.tm.FindToplevelLocked(5)->SetMinimized(false); } f.check(5, true);
        f.comp.zc().Release(Key(25), 1); f.check(4, false);
        f.consume(Key(25),160,100); f.comp.zc().Activate(Key(25),1); f.check(5,true);
        f.childData.parentSurface=&f.steam; f.check(4,false);
        f.childData.parentSurface=&f.game; f.check(5,true);
        f.tm.UnregisterSurfaceResource(Key(5)); f.check(4,false);
        f.tm.RegisterSurfaceResource(Key(5),&f.game); f.check(5,true);
        { auto lock=f.tm.Lock(); f.tm.FindToplevelLocked(5)->FrameData().resize(160*100*4,255); }
        f.comp.zc().Release(Key(25),1); f.check(5,false);
    }
    if (mode == 1) {
        f.menuData.surface=&f.menu; f.menuData.surfaceKey=Key(26); f.menuData.protocolId=26;
        f.menuData.clientPid=100; f.menuData.isSubsurface=true; f.menuData.parentSurface=&f.game;
        f.menuData.w=40; f.menuData.h=20;
        f.tm.RegisterSurfaceResource(Key(26),&f.menu);
        SubsurfaceLayer menu; menu.surface=&f.menu; menu.surfaceKey=Key(26); menu.parentToplevel=5;
        menu.x=menu.localX=60; menu.y=menu.localY=40; menu.w=40; menu.h=20;
        menu.vpDstW=40; menu.vpDstH=20; menu.shmCommitSerial=1;
        { auto lock=f.tm.Lock(); f.comp.UpsertSubsurfaceLayer(std::move(menu),std::vector<uint8_t>(40*20*4,255)); }
        InputTarget hit; assert(f.input.FindInputTargetAt(80,50,hit));
        assert(hit.surface==&f.menu && "SHM menu on GPU-only parent is visible and hittable");
        GpuDesktopScene scene; assert(f.comp.SnapshotGpuDesktopScene({},f.cache,scene,{f.gpu}));
        auto gpu=std::find_if(scene.layers.begin(),scene.layers.end(),[&](auto& l){return l.zeroCopyKey==f.gpu.zeroCopyKey;});
        auto shm=std::find_if(scene.layers.begin(),scene.layers.end(),[&](auto& l){return l.ownerSurfaceKey==Key(26);});
        assert(gpu!=scene.layers.end() && shm!=scene.layers.end() && gpu<shm);
        {auto lock=f.tm.Lock();f.tm.HideToplevelLocked(5);} f.check(4,false);
    }
    if (mode == 2) {
        f.window(f.modalData,f.modal,6,false);
        { auto lock=f.tm.Lock(); auto* st=f.tm.FindToplevelLocked(6);st->SetContentSize(40,40);st->SetPosition(60,30);f.tm.SetModalLocked(6,5,true); }
        f.modalChildData.surface=&f.modalChild;f.modalChildData.surfaceKey=Key(27);f.modalChildData.protocolId=27;
        f.modalChildData.clientPid=100;f.modalChildData.isSubsurface=true;f.modalChildData.parentSurface=&f.modal;
        f.tm.RegisterSurfaceResource(Key(27),&f.modalChild);f.consume(Key(27),40,40);f.comp.zc().Activate(Key(27),1);
        InputTarget hit;assert(f.input.FindInputTargetAt(80,50,hit));assert(hit.toplevelId==6);
        assert(f.input.FindInputTargetAt(10,50,hit));assert(hit.toplevelId==5&&hit.blockedModalId==6);
    }
#ifndef GPU_FOLLOWUP_BASELINE
    if (mode == 4) {
        { auto lock=f.tm.Lock(); f.tm.HideToplevelLocked(5); }
        SurfaceData producer; wl_resource resource{&producer};
        producer.surface=&resource;producer.surfaceKey=Key(30);producer.clientPid=100;producer.protocolId=30;
        f.tm.RegisterSurfaceResource(Key(30),&resource);
        ZeroCopyLayerInfo info;
        assert(f.comp.zc().GetLayerInfo(Key(30),1,160,100,info));
        const auto generation=info.bindingGeneration;
        assert(generation && info.parentToplevel==4);
        assert(f.comp.zc().NoteLayerConsumed(Key(30),10,generation,160,100));
        f.comp.zc().Activate(Key(30),1);
        uint64_t owner=0;uint32_t top=0;int w=0,h=0;
        { auto lock=f.tm.Lock();assert(f.comp.zc().ActiveOwner(Key(30),owner,top,w,h));
          f.comp.zc().InvalidateBindingsForSurface(100,4); }
        assert(f.comp.zc().GetLayerInfo(Key(30),1,160,100,info));
        assert(info.bindingGeneration>generation);
        { auto lock=f.tm.Lock();assert(!f.comp.zc().ActiveOwner(Key(30),owner,top,w,h)); }
        assert(!f.comp.zc().NoteLayerConsumed(Key(30),11,generation,160,100));
        assert(f.comp.zc().NoteLayerConsumed(Key(30),12,info.bindingGeneration,160,100));
        { auto lock=f.tm.Lock();assert(f.comp.zc().ActiveOwner(Key(30),owner,top,w,h)); }
    }
    if (mode == 3) {
        winehua::NoteInputTargetToplevel(4, 7, 9);
        const auto before=winehua::LastInputTarget();
        winehua::SetFrameLoopDiagnostics(true);
        GpuDesktopScene scene;assert(f.comp.SnapshotGpuDesktopScene({},f.cache,scene,{f.gpu}));
        assert(scene.diagnosticSerial && scene.diagnosticUs);
        const auto after=winehua::LastInputTarget();
        assert(after.toplevelId==4&&after.eventUs==before.eventUs&&after.desktopX==7&&after.desktopY==9);
        GpuDesktopScene quiet;assert(f.comp.SnapshotGpuDesktopScene({},f.cache,quiet,{f.gpu}));
        assert(!quiet.diagnosticSerial && SameGpuDesktopScene(scene,quiet));
        winehua::SetFrameLoopDiagnostics(false);
        // Explicit protocol resize changes the shared fit, retaining black-border ownership.
        f.childData.w=100;f.childData.h=100;
        f.gpu.w=f.gpu.sourceW=100; f.gpu.h=f.gpu.sourceH=100;
        InputTarget hit;assert(f.input.FindInputTargetAt(2,50,hit));assert(hit.toplevelId==5&&hit.swallow);
        assert(f.input.FindInputTargetAt(80,50,hit));assert(!hit.swallow && hit.localX==50);
    }
#endif
    std::puts("full production scene/input contract passed");
}
