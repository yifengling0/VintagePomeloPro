#include "compositor/toplevel/desktop_compositor.h"
#include "compositor/input/input_resolver.h"
#include "compositor/toplevel/toplevel_manager.h"
#include "compositor/frame/surface_data.h"
#include "compositor/frame/input_target_probe.h"
#include "common/frame_loop_diagnostics.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#ifndef GPU_FOLLOWUP_BASELINE
#include "compositor/input/input_injector.h"
#include "compositor/input/input_queue.h"
#include "input/seat.h"
#include "input/text_input.h"
#include "input/input_manager.h"
#include "input/pointer_extras.h"
#include "compositor/toplevel/move_grab.h"
#include <string>
struct ProtocolEvent {
    std::string name;
    wl_resource* resource;
    wl_resource* surface;
};
static std::vector<ProtocolEvent> events;
static std::vector<wl_resource*> keyboards, pointers;
static wl_client* Client(uintptr_t id) { return reinterpret_cast<wl_client*>(id); }
void HostTestProtocolEvent(wl_resource* r, const char* name, wl_resource* s) {
    events.push_back({name, r, s});
}
Seat* Seat::GetInstance() {
    static Seat seat;
    seat.ptrCount_.store(pointers.size());seat.kbdCount_.store(keyboards.size());
    return &seat;
}
std::vector<wl_resource*> Seat::GetAllKeyboardResources() { return keyboards; }
std::vector<wl_resource*> Seat::GetAllPointerResources() { return pointers; }
wl_resource* Seat::GetKeyboardResource() { return keyboards.empty()?nullptr:keyboards.front(); }
PointerExtras* PointerExtras::GetInstance() { static PointerExtras extras; return &extras; }
bool PointerExtras::HasRelativePointer() const { return false; }
void PointerExtras::SendRelativeMotion(double, double) {}
InputSpaceMapper* InputSpaceMapper::GetInstance() { static InputSpaceMapper mapper;return &mapper; }
void InputSpaceMapper::CoordTransform(double x,double y,uint32_t,wl_fixed_t* sx,wl_fixed_t* sy,FitRect* fit) {
    *sx=wl_fixed_from_double(x);*sy=wl_fixed_from_double(y);
    if(fit) { *fit={};fit->srcW=fit->dstW=160;fit->srcH=fit->dstH=100;fit->scale=1; }
}
void InputSpaceMapper::UpdateGlobalPtr(wl_fixed_t,wl_fixed_t,GlobalPtrState::Space) {}
void InputSpaceMapper::ResetGlobalPtr() {}
void wl_display_flush_clients(wl_display*) {}
static int dispatchFd=-1;
static int (*dispatchCallback)(int,uint32_t,void*)=nullptr;
static void* dispatchData=nullptr;
wl_event_loop* wl_display_get_event_loop(wl_display*) { return reinterpret_cast<wl_event_loop*>(1); }
wl_event_source* wl_event_loop_add_fd(wl_event_loop*,int fd,uint32_t,
                                     int (*cb)(int,uint32_t,void*),void* data) {
    dispatchFd=fd;dispatchCallback=cb;dispatchData=data;
    return reinterpret_cast<wl_event_source*>(1);
}
int wl_event_source_remove(wl_event_source*) { dispatchFd=-1;dispatchCallback=nullptr;return 0; }
static void DrainInput() {
    assert(dispatchCallback && dispatchFd>=0);
    assert(dispatchCallback(dispatchFd,WL_EVENT_READABLE,dispatchData)==0);
}
TextInput* TextInput::GetInstance() { static TextInput text; return &text; }
void TextInput::OnKeyboardEnter(wl_resource* s) { events.push_back({"ime.enter", nullptr, s}); }
void TextInput::OnKeyboardLeave(wl_resource* s) { events.push_back({"ime.leave", nullptr, s}); }
static void Expect(const char* name, wl_resource* surface, wl_resource* resource = nullptr) {
    assert(!events.empty());
    const auto e = events.front();
    assert(e.name == name && e.surface == surface && e.resource == resource);
    events.erase(events.begin());
}
#endif

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
    if (mode == 5) {
        f.game.client=f.child.client=f.steam.client=f.rootRes.client=Client(1);
        // Same process / client, different window: it is not an ancestry edge.
        assert(f.input.ResolveKeyboardFocusSurface(5,&f.game)==&f.game);
        assert(f.input.ResolveKeyboardFocusSurface(5,&f.child)==&f.game);
        assert(!f.input.ResolveKeyboardFocusSurface(4,&f.child));
        f.menuData.surface=&f.menu;f.menuData.surfaceKey=Key(26);
        f.menuData.isSubsurface=true;f.menuData.parentSurface=&f.child;f.menu.client=Client(1);
        f.tm.RegisterSurfaceResource(Key(26),&f.menu);
        assert(f.input.ResolveKeyboardFocusSurface(5,&f.menu)==&f.game);
        f.childData.parentSurface=&f.menu;
        assert(!f.input.ResolveKeyboardFocusSurface(5,&f.menu)); // cycle
        f.childData.parentSurface=&f.game;
        f.tm.UnregisterSurfaceResource(Key(25));
        assert(!f.input.ResolveKeyboardFocusSurface(5,&f.menu)); // destroyed intermediate
        f.tm.RegisterSurfaceResource(Key(25),&f.child);
        f.tm.UnregisterSurfaceResource(Key(5));
        assert(!f.input.ResolveKeyboardFocusSurface(5,&f.child)); // destroyed owner
        f.tm.RegisterSurfaceResource(Key(5),&f.game);
        // A mismatched resource registration cannot establish ownership.
        f.tm.RegisterSurfaceResource(Key(25),&f.steam);
        assert(!f.input.ResolveKeyboardFocusSurface(5,&f.child));
        f.tm.RegisterSurfaceResource(Key(25),&f.child);
        // Popup pseudo-toplevel mappings can retain their own focus surface.
        f.tm.MapToplevelSurface(26,&f.menu);
        assert(f.input.ResolveKeyboardFocusSurface(26,&f.menu)==&f.menu);
        f.tm.UnmapToplevelSurface(26);

        InputStateTracker tracker;
        InputInjector inject(&tracker);inject.BindResolvers(&f.input,&f.tm);
        wl_resource kbd{nullptr,Client(1)}, otherKbd{nullptr,Client(2)}, ptr{nullptr,Client(1)};
        keyboards={&kbd,&otherKbd};pointers={&ptr};
        inject.InjectKeyboardEnter(4,&f.steam);
        Expect("keyboard.enter",&f.steam,&kbd);Expect("ime.enter",&f.steam);
        assert(tracker.KeyboardFocusedSurface()==&f.steam);
        inject.InjectKeyboardEnter(5,&f.child);
        // Leave must target the actual OLD owner, not the new game.
        Expect("keyboard.leave",&f.steam,&kbd);Expect("ime.leave",&f.steam);
        Expect("keyboard.enter",&f.game,&kbd);Expect("ime.enter",&f.game);
        assert(tracker.KeyboardFocusedSurface()==&f.game && tracker.KeyboardFocusedToplevel()==5);
        inject.InjectKeyboardEnter(5,&f.menu);
        assert(events.empty()); // child-to-child clicks cannot cause deactivate/reactivate
        inject.InjectPointerEnter(5,&f.child,0,0);
        Expect("pointer.enter",&f.child,&ptr);
        assert(tracker.PointerFocusedSurface()==&f.child); // pointer coordinates remain on child
        inject.InjectKeyboardKey(30,1);
        Expect("keyboard.key",nullptr,&kbd);
        // Bad destinations retain existing focus and send no leave.
        inject.InjectKeyboardEnter(4,&f.child);
        f.tm.UnregisterSurfaceResource(Key(26));inject.InjectKeyboardEnter(5,&f.menu);
        f.steam.client=Client(3);inject.InjectKeyboardEnter(4,&f.steam);
        f.steam.client=Client(1);
        assert(events.empty() && tracker.KeyboardFocusedSurface()==&f.game);
        // Destroyed focus cannot be dereferenced by subsequent keys.
        f.tm.UnregisterSurfaceResource(Key(5));inject.InjectKeyboardKey(30,0);
        assert(events.empty() && !tracker.KeyboardEntered());
        f.tm.RegisterSurfaceResource(Key(5),&f.game);
        // Rapid A -> B -> A requests before a flush must all be respected.
        inject.InjectKeyboardEnter(4,&f.steam);
        Expect("keyboard.enter",&f.steam,&kbd);Expect("ime.enter",&f.steam);
        InputQueue queue;
        queue.Enqueue(InputQueue::Event::KBD_ENTER,5,&f.child,0,0,0,0);
        queue.Enqueue(InputQueue::Event::KBD_ENTER,4,&f.steam,0,0,0,0);
        queue.Enqueue(InputQueue::Event::KBD_ENTER,4,&f.steam,0,0,0,0);
        for(const auto& e:queue.Poll()) inject.InjectKeyboardEnter(e.tl,e.surface);
        Expect("keyboard.leave",&f.steam,&kbd);Expect("ime.leave",&f.steam);
        Expect("keyboard.enter",&f.game,&kbd);Expect("ime.enter",&f.game);
        Expect("keyboard.leave",&f.game,&kbd);Expect("ime.leave",&f.game);
        Expect("keyboard.enter",&f.steam,&kbd);Expect("ime.enter",&f.steam);
        assert(events.empty() && tracker.KeyboardFocusedSurface()==&f.steam);
        // Explicit leave/reset still clears focus, even with no seat resources.
        keyboards.clear();inject.InjectKeyboardLeave();
        Expect("ime.leave",&f.steam);
        assert(events.empty() && !tracker.KeyboardEntered());
    }
    if (mode == 6) {
        f.steam.client=f.game.client=f.child.client=Client(1);
        wl_resource kbd{nullptr,Client(1)},ptr{nullptr,Client(1)};
        keyboards={&kbd};pointers={&ptr};
        DisplayPolicy windowPolicy{false};
        MoveGrabHandler grab;
        auto* manager=InputManager::GetInstance();
        manager->BindCompositorDeps(f.tm,f.input,grab,windowPolicy,f.root);
        manager->Initialize(reinterpret_cast<wl_display*>(1));
        manager->ResetSessionState();
        manager->InjectKeyboardEnter(4,&f.steam);
        events.clear();
        manager->SendPointerEvent(5,1,10,10,0x110);
        // Real NAPI entry must not overwrite delivered focus before dispatch.
        assert(manager->GetKeyboardFocusedToplevel()==4 && events.empty());
        DrainInput();
        auto leave=std::find_if(events.begin(),events.end(),[](auto& e){return e.name=="keyboard.leave";});
        auto enter=std::find_if(events.begin(),events.end(),[](auto& e){return e.name=="keyboard.enter";});
        auto button=std::find_if(events.begin(),events.end(),[](auto& e){return e.name=="pointer.button";});
        assert(leave!=events.end()&&leave->surface==&f.steam);
        assert(enter!=events.end()&&enter->surface==&f.game);
        assert(button!=events.end()&&leave<enter&&enter<button);
        assert(manager->GetKeyboardFocusedToplevel()==5);
        events.clear();
        manager->SendPointerEvent(5,1,10,10,0x110);
        DrainInput();
        for(auto& e:events) assert(e.name!="keyboard.enter"&&e.name!="keyboard.leave");
        events.clear();
        manager->SendPointerEvent(4,1,10,10,0x110);
        manager->SendPointerEvent(5,1,10,10,0x110);
        assert(manager->GetKeyboardFocusedToplevel()==5);
        DrainInput();
        std::vector<wl_resource*> owners;
        for(auto& e:events) if(e.name=="keyboard.enter"||e.name=="keyboard.leave") owners.push_back(e.surface);
        assert((owners==std::vector<wl_resource*>{&f.game,&f.steam,&f.steam,&f.game}));
        assert(manager->GetKeyboardFocusedToplevel()==5);
        manager->ResetSessionState();
        assert(!manager->HasKeyboardFocus());
        manager->Shutdown();
    }
    if (mode == 7) {
        f.steam.client=f.game.client=f.child.client=f.menu.client=Client(1);
        f.menuData.surface=&f.menu;f.menuData.surfaceKey=Key(26);f.menuData.clientPid=100;
        f.menuData.isSubsurface=true;f.menuData.parentSurface=&f.game;
        f.menuData.w=40;f.menuData.h=20;
        f.tm.RegisterSurfaceResource(Key(26),&f.menu);
        SubsurfaceLayer menu;
        menu.surface=&f.menu;menu.surfaceKey=Key(26);menu.parentToplevel=5;
        menu.x=menu.localX=60;menu.y=menu.localY=40;menu.w=40;menu.h=20;
        menu.vpDstW=40;menu.vpDstH=20;menu.shmCommitSerial=1;
        {auto lock=f.tm.Lock();f.comp.UpsertSubsurfaceLayer(std::move(menu),std::vector<uint8_t>(40*20*4,255));}
        f.check(5,true);
        wl_resource kbd{nullptr,Client(1)},ptr{nullptr,Client(1)};
        keyboards={&kbd};pointers={&ptr};
        MoveGrabHandler grab;
        auto* manager=InputManager::GetInstance();
        manager->BindCompositorDeps(f.tm,f.input,grab,f.policy,f.root);
        manager->Initialize(reinterpret_cast<wl_display*>(1));manager->ResetSessionState();
        manager->InjectKeyboardEnter(4,&f.steam);events.clear();
        manager->SendPointerEvent(0,1,80,50,0x110);
        assert(manager->GetKeyboardFocusedToplevel()==4);DrainInput();
        auto pointer=std::find_if(events.begin(),events.end(),[](auto& e){return e.name=="pointer.enter";});
        auto keyboard=std::find_if(events.begin(),events.end(),[](auto& e){return e.name=="keyboard.enter";});
        assert(pointer!=events.end()&&pointer->surface==&f.menu);
        assert(keyboard!=events.end()&&keyboard->surface==&f.game);
        assert(manager->GetKeyboardFocusedToplevel()==5);
        events.clear();
        manager->SendKeyEvent(f.root,30,true);DrainInput();
        assert(manager->GetKeyboardFocusedToplevel()==5);
        for(auto& e:events) assert(e.name!="keyboard.enter"&&e.name!="keyboard.leave");
        assert(std::any_of(events.begin(),events.end(),[](auto& e){return e.name=="keyboard.key";}));
        manager->ResetSessionState();manager->Shutdown();
    }
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
    if (mode == 8) {
        f.childData.w=100;f.childData.h=100;
        f.gpu.w=f.gpu.sourceW=100;f.gpu.h=f.gpu.sourceH=100;
        const auto snapshot=[&]() {
            GpuDesktopScene scene;
            assert(f.comp.SnapshotGpuDesktopScene({},f.cache,scene,{f.gpu}));
            return scene;
        };
        auto scene=snapshot();
        auto black=std::find_if(scene.layers.begin(),scene.layers.end(),[](const auto& l){return l.solidBlack;});
        auto game=std::find_if(scene.layers.begin(),scene.layers.end(),[](const auto& l){return l.zeroCopyKey==Key(25);});
        assert(black!=scene.layers.end() && "fullscreen GPU scene needs an opaque black backing");
        assert(black->x==0&&black->y==0&&black->w==160&&black->h==100&&black->opaque);
        assert(game!=scene.layers.end()&&black<game&&game->x==30&&game->w==100);
        InputTarget hit;assert(f.input.FindInputTargetAt(2,50,hit));assert(hit.toplevelId==5&&hit.swallow);
        assert(f.input.FindInputTargetAt(80,50,hit));assert(hit.toplevelId==5&&!hit.swallow&&hit.localX==50);
        f.window(f.modalData,f.modal,6,true);
        { auto lock=f.tm.Lock();auto* st=f.tm.FindToplevelLocked(6);st->ApplyFullscreen(false);
          st->SetContentSize(40,40);st->SetPosition(60,30);st->FrameData().resize(40*40*4,255); }
        scene=snapshot();
        black=std::find_if(scene.layers.begin(),scene.layers.end(),[](const auto& l){return l.solidBlack;});
        game=std::find_if(scene.layers.begin(),scene.layers.end(),[](const auto& l){return l.zeroCopyKey==Key(25);});
        auto popup=std::find_if(scene.layers.begin(),scene.layers.end(),[](const auto& l){return l.parentToplevel==6;});
        assert(black<game&&game<popup&&popup!=scene.layers.end());
        { auto lock=f.tm.Lock();f.tm.FindToplevelLocked(5)->ApplyFullscreen(false);
          f.tm.FindToplevelLocked(4)->ApplyFullscreen(false); }
        scene=snapshot();
        assert(std::none_of(scene.layers.begin(),scene.layers.end(),[](const auto& l){return l.solidBlack;}));
        { auto lock=f.tm.Lock();f.tm.FindToplevelLocked(5)->ApplyFullscreen(true);
          f.tm.FindToplevelLocked(5)->SetMinimized(true); }
        scene=snapshot();
        assert(std::none_of(scene.layers.begin(),scene.layers.end(),[](const auto& l){return l.solidBlack;}));
    }
    if (mode == 9) {
        // The SHM bootstrap image stays at 160x100 after a native resize.
        f.childData.w=160;f.childData.h=100;
        SubsurfaceLayer bootstrap;bootstrap.surface=&f.child;bootstrap.surfaceKey=Key(25);
        bootstrap.parentToplevel=5;bootstrap.w=160;bootstrap.h=100;
        {auto lock=f.tm.Lock();f.comp.UpsertSubsurfaceLayer(std::move(bootstrap),std::vector<uint8_t>(160*100*4,255));}
        f.consume(Key(25),480,320);
        ZeroCopyLayerInfo info;
        assert(f.comp.zc().GetLayerInfo(Key(25),1,480,320,info));
        assert(info.width==480&&info.height==320);
        uint64_t owner=0;uint32_t top=0;int w=0,h=0;
        {auto lock=f.tm.Lock();assert(f.comp.zc().ActiveOwner(Key(25),owner,top,w,h));
         assert(w==480&&h==320);assert(f.comp.zc().GetContentSize(5,w,h));assert(w==480&&h==320);}
        InputTarget hit;assert(f.input.FindInputTargetAt(80,50,hit));
        assert(hit.toplevelId==5&&!hit.swallow&&hit.localX==240&&hit.localY==160);
        // Explicit viewports are display intent and must survive native resize.
        f.childData.vpDstW=200;f.childData.vpDstH=150;
        {auto lock=f.tm.Lock();assert(f.comp.zc().ActiveOwner(Key(25),owner,top,w,h));assert(w==200&&h==150);}
        assert(f.comp.zc().GetLayerInfo(Key(25),1,480,320,info));assert(info.width==200&&info.height==150);
        f.childData.vpDstW=f.childData.vpDstH=0;
        f.childData.w=240;f.childData.h=160;
        {auto lock=f.tm.Lock();assert(f.comp.zc().ActiveOwner(Key(25),owner,top,w,h));assert(w==240&&h==160);}
        f.consume(Key(25),240,160);
        assert(f.comp.zc().GetLayerInfo(Key(25),1,240,160,info));
        assert(info.width==240&&info.height==160);
    }
    if (mode == 10) {
        GpuDesktopScene scene;
        assert(f.comp.SnapshotGpuDesktopScene({},f.cache,scene,{f.gpu}));
        auto next=scene;
        auto game=std::find_if(next.layers.begin(),next.layers.end(),[](const auto& l){return l.zeroCopyKey==Key(25);});
        assert(game!=next.layers.end());
        // A static NativeBuffer remains valid while its view or protocol owner
        // changes. Both renderers must redraw without waiting for another frame.
        game->sampling.v={1,0,-1,0};
        assert(!SameGpuDesktopScene(scene,next));
        *game=f.gpu; // next geometry is irrelevant to the identity checks below.
        scene=next;
        game->ownerSurfaceKey=Key(26);assert(!SameGpuDesktopScene(scene,next));
        scene=next;
        game->parentToplevel=6;assert(!SameGpuDesktopScene(scene,next));
        scene=next;
        game->external=true;assert(!SameGpuDesktopScene(scene,next));
        scene=next;
        game->subsurface=false;assert(!SameGpuDesktopScene(scene,next));
        scene=next;
        game->directSurfaceKey=5;assert(!SameGpuDesktopScene(scene,next));
        scene=next;
        ++next.diagnosticSerial;++next.diagnosticUs;
        assert(SameGpuDesktopScene(scene,next));
    }
    if (mode == 11) {
        ZeroCopyLayerInfo info;
        info.clientPid=100;info.surfaceId=25;info.parentToplevel=5;info.subsurface=true;
        info.x=0;info.y=0;info.width=160;info.height=100;
        // A completed RGBA game frame may contain zero/fractional alpha from
        // separate D3D9 alpha blending. It is still an opaque WGL window.
        auto native=MakeNativeWindowLayer(Key(25),info,160,100);
        assert(native.opaque && native.ownerSurfaceKey==Key(25));
        GpuDesktopScene scene;
        assert(f.comp.SnapshotGpuDesktopScene({},f.cache,scene,{native}));
        auto game=std::find_if(scene.layers.begin(),scene.layers.end(),[](const auto& l){return l.zeroCopyKey==Key(25);});
        assert(game!=scene.layers.end() && game->opaque);
        // An ARGB menu remains translucent when merged with the opaque game.
        GpuDesktopLayer menu;
        menu.key=Key(90);menu.parentToplevel=5;menu.subsurface=true;
        menu.opaque=false;menu.pixels=std::make_shared<const std::vector<uint8_t>>(4,64);
        scene.layers.push_back(menu);
        MergeZeroCopySceneLayers(scene,{native});
        assert(!scene.layers.back().opaque && scene.layers.back().pixels==menu.pixels);
        auto next=scene;
        next.layers.back().opaque=true;
        assert(!SameGpuDesktopScene(scene,next));
    }
    if (mode == 12) {
        // Device repro: a 103x78 Start menu is stored in a 128x128 SHM buffer.
        // Compare the actual sampled coordinates, not just the output size.
        Fixture menuFixture;
        auto& m=menuFixture;
        m.gameData.hasToplevel=false;
        { auto lock=m.tm.Lock(); m.tm.HideToplevelLocked(4);m.tm.HideToplevelLocked(5); }
        m.menuData.surface=&m.menu;m.menuData.surfaceKey=Key(26);m.menuData.clientPid=100;
        m.menuData.isSubsurface=true;m.menuData.parentSurface=&m.rootRes;
        m.tm.RegisterSurfaceResource(Key(26),&m.menu);
        SubsurfaceLayer menu;menu.surface=&m.menu;menu.surfaceKey=Key(26);menu.parentToplevel=1;
        menu.x=menu.localX=10;menu.y=menu.localY=5;menu.w=menu.h=128;
        menu.vpDstW=103;menu.vpDstH=78;menu.shmCommitSerial=1;
        menu.viewport.width=103;menu.viewport.height=78;
        auto changed=menu;
        {auto lock=m.tm.Lock();m.comp.UpsertSubsurfaceLayer(std::move(menu),std::vector<uint8_t>(128*128*4,255));}
        GpuDesktopScene scene;assert(m.comp.SnapshotGpuDesktopScene({},m.cache,scene));
        auto layer=std::find_if(scene.layers.begin(),scene.layers.end(),[](const auto& l){return l.ownerSurfaceKey==Key(26);});
        assert(layer!=scene.layers.end() && layer->w==103 && layer->h==78);
        assert(layer->sourceW==128 && layer->sourceH==128);
        assert(std::abs(layer->sampling.u[1]-103.f/128)<0.00001f && "padded menu buffer must be cropped before scaling");
        assert(std::abs(layer->sampling.v[2]-78.f/128)<0.00001f);
        InputTarget hit;
        assert(m.input.FindInputTargetAt(112,20,hit) && hit.surface==&m.menu && hit.originX==10);
        assert(m.input.FindInputTargetAt(113,20,hit) && hit.surface==&m.rootRes && hit.originX==0);
        // A viewport-only change needs new sampling, but reuses uploaded pixels.
        auto before=scene;
        changed.viewport.x=4;changed.viewport.y=3;
        changed.viewport.width=99;changed.viewport.height=75;
        {auto lock=m.tm.Lock();m.comp.UpsertSubsurfaceLayer(std::move(changed),std::vector<uint8_t>(128*128*4,255));}
        assert(m.comp.SnapshotGpuDesktopScene({},m.cache,scene));
        assert(!SameGpuDesktopScene(before,scene));
        layer=std::find_if(scene.layers.begin(),scene.layers.end(),[](const auto& l){return l.ownerSurfaceKey==Key(26);});
        auto old=std::find_if(before.layers.begin(),before.layers.end(),[](const auto& l){return l.ownerSurfaceKey==Key(26);});
        assert(layer->pixels==old->pixels);
        assert(std::abs(layer->sampling.u[0]-4.f/128)<0.00001f);
        assert(std::abs(layer->sampling.v[0]-3.f/128)<0.00001f);
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
    if (mode == 13) {
        // Two real GPU drawables under one root, with no parent SHM. Exact
        // protocol identity replaces the previous ambiguous-child rejection.
        f.comp.zc().RemoveKey(Key(25));
        f.childData.directOwnerKey=Key(5); f.childData.directGeneration=40;
        f.childData.directVisible=true;
        SurfaceData second;wl_resource secondRes{&second};
        second.surface=&secondRes;second.surfaceKey=Key(26);second.clientPid=100;
        second.protocolId=26;second.isSubsurface=true;second.parentSurface=&f.game;
        second.directOwnerKey=Key(5);second.directGeneration=41;second.directVisible=true;
        second.subsurfaceX=20;second.subsurfaceY=30;
        f.tm.RegisterSurfaceResource(Key(26),&secondRes);
        {auto lock=f.tm.Lock();f.tm.FindToplevelLocked(4)->ApplyFullscreen(false);
         f.tm.FindToplevelLocked(5)->ApplyFullscreen(false);
         for(auto* sd:{&f.childData,&second}) {
             SubsurfaceLayer layer;layer.surface=sd->surface;layer.surfaceKey=sd->surfaceKey;
             layer.parentToplevel=5;layer.localX=sd->subsurfaceX;layer.localY=sd->subsurfaceY;
             layer.w=160;layer.h=100;f.comp.UpsertSubsurfaceLayer(std::move(layer),{});
         }}
        std::vector<GpuDesktopDirectSource> sources{{100,5,25,160,100,40},{100,5,26,80,60,41}};
        auto snapshot=[&]() {GpuDesktopScene scene;assert(f.comp.SnapshotGpuDesktopScene(sources,f.cache,scene));return scene;};
        auto count=[](const auto& scene){return std::count_if(scene.layers.begin(),scene.layers.end(),[](const auto& l){return l.directSurfaceKey!=0;});};
        auto scene=snapshot();assert(count(scene)==2);
        auto a=std::find_if(scene.layers.begin(),scene.layers.end(),[](const auto& l){return l.directSurfaceKey==Key(25);});
        auto b=std::find_if(scene.layers.begin(),scene.layers.end(),[](const auto& l){return l.directSurfaceKey==Key(26);});
        assert(a<b&&b->x==20&&b->y==30);
        {auto lock=f.tm.Lock();assert(f.comp.ReorderSubsurfaceLayerAbove(&f.child,&secondRes));}
        scene=snapshot();a=std::find_if(scene.layers.begin(),scene.layers.end(),[](const auto& l){return l.directSurfaceKey==Key(25);});
        b=std::find_if(scene.layers.begin(),scene.layers.end(),[](const auto& l){return l.directSurfaceKey==Key(26);});assert(b<a);
        second.directVisible=false;assert(count(snapshot())==1);second.directVisible=true;
        sources[1].generation=40;assert(count(snapshot())==1);sources[1].generation=41;
        sources[1].pid=101;assert(count(snapshot())==1);sources[1].pid=100;
        second.directOwnerKey=Key(4);assert(count(snapshot())==1);second.directOwnerKey=Key(5);
        second.parentSurface=&f.steam;assert(count(snapshot())==1);second.parentSurface=&f.game;
        {auto lock=f.tm.Lock();f.tm.FindToplevelLocked(5)->SetMinimized(true);}
        assert(count(snapshot())==0);
        f.tm.UnregisterSurfaceResource(Key(26));
    }
    if (mode == 14) {
        f.comp.zc().RemoveKey(Key(25));
        f.childData.directOwnerKey=Key(5);f.childData.directGeneration=40;
        f.childData.directVisible=true;
        {auto lock=f.tm.Lock();
         SubsurfaceLayer layer;layer.surface=&f.child;layer.surfaceKey=Key(25);
         layer.parentToplevel=5;layer.w=160;layer.h=100;
         f.comp.UpsertSubsurfaceLayer(std::move(layer),{});}
        GpuDesktopScene scene;
        assert(f.comp.SnapshotGpuDesktopScene({{100,5,25,160,100,40}},f.cache,scene));
        InputTarget hit;
        assert(f.input.FindInputTargetAt(80,50,hit));
        assert(hit.toplevelId==5 && hit.surface==&f.game &&
               "Direct input-transparent drawable must delegate to visible game parent");
        // Pointer and keyboard enter use the same owning window. The older
        // Steam fullscreen must not receive the press or regain priority.
        assert(f.input.ResolveKeyboardFocusSurface(hit.toplevelId,hit.surface)==&f.game);
        assert(f.input.FindToplevelAt(80,50)==5);
        // A real interactive software menu still takes precedence above it.
        f.menuData.surface=&f.menu;f.menuData.surfaceKey=Key(26);f.menuData.clientPid=100;
        f.menuData.isSubsurface=true;f.menuData.parentSurface=&f.game;
        f.tm.RegisterSurfaceResource(Key(26),&f.menu);
        {auto lock=f.tm.Lock();SubsurfaceLayer menu;menu.surface=&f.menu;
         menu.surfaceKey=Key(26);menu.parentToplevel=5;menu.localX=60;menu.localY=30;
         menu.w=40;menu.h=40;
         f.comp.UpsertSubsurfaceLayer(std::move(menu),std::vector<uint8_t>(40*40*4,255));}
        assert(f.input.FindInputTargetAt(80,50,hit) && hit.surface==&f.menu);
        f.menuData.inputRegionEmpty=true;
        assert(f.input.FindInputTargetAt(80,50,hit) && hit.surface==&f.game);
        {auto lock=f.tm.Lock();f.tm.FindToplevelLocked(5)->SetMinimized(true);}
        assert(f.comp.SnapshotGpuDesktopScene({{100,5,25,160,100,40}},f.cache,scene));
        assert(f.input.FindInputTargetAt(80,50,hit) && hit.toplevelId==4 && hit.surface==&f.steam);
    }
    std::puts("full production scene/input contract passed");
}
