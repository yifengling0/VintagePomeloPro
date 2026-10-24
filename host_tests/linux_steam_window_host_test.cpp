// Reuse platform fixtures, exercise the actual compositor/resolver implementations.
#define main ExistingSceneRegressionMain
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#include "gpu_scene_input_test.cpp"
#pragma GCC diagnostic pop
#undef main
#include "compositor/wayland_server.h"

int main()
{
    // Every edge starts from the original rectangle; repeated pointer frames
    // cannot compound motion. Left/top edges must keep their opposite fixed.
    const ExternalResizeGeometry original{100,60,640,400};
    for (uint32_t edge : {1u,2u,4u,5u,6u,8u,9u,10u}) {
        const auto a=original.Drag(edge,25,35), b=original.Drag(edge,25,35);
        assert(a.x==b.x&&a.y==b.y&&a.width==b.width&&a.height==b.height);
        if(edge&4)assert(a.x+a.width==original.x+original.width&&a.width==615);
        if(edge&8)assert(a.x==original.x&&a.width==665);
        if(edge&1)assert(a.y+a.height==original.y+original.height&&a.height==365);
        if(edge&2)assert(a.y==original.y&&a.height==435);
        for(int delta:{-100000,100000}) {
            const auto r=original.Drag(edge,delta,delta);
            assert(r.width>=320&&r.height>=200&&r.width<=4096&&r.height<=4096);
            assert(int64_t(r.width)*r.height<=8388608);
        }
    }
    assert(original.Drag(3,100,100).width==640);
    assert(original.Drag(10,100000,100000).width==4096);
    Fixture f;
    SurfaceData hostData; wl_resource host{&hostData};
    f.window(hostData, host, 7, true);
    {
        auto lock = f.tm.Lock();
        f.tm.HideToplevelLocked(5);
        f.tm.FindToplevelLocked(4)->ApplyFullscreen(false);
        auto* window = f.tm.FindToplevelLocked(7);
        window->ApplyFullscreen(false); window->SetPosition(20, 10);
        window->SetContentSize(100, 72);
    }
    assert(!f.comp.BindExternalToplevel(4, 7, 999, 100, 4, 20, 92, 48));
    assert(!f.comp.BindExternalToplevel(4, 4, 100, 100, 4, 20, 92, 48));
    assert(!f.comp.BindExternalToplevel(4, 1, 100, 100, 4, 20, 92, 48));
    assert(!f.comp.BindExternalToplevel(4, 7, 100, 100, -1, 20, 92, 48));
    assert(f.comp.BindExternalToplevel(4, 7, 100, 100, 4, 20, 92, 48));
    InputTarget hit;
    assert(f.input.FindInputTargetAt(60, 50, hit));
    assert(hit.toplevelId == 7 && hit.surface == &f.steam);
    assert(hit.localX > 0 && hit.localX < 160 && hit.localY > 0 && hit.localY < 100);
    assert(f.input.ResolveKeyboardFocusSurface(7, &host) == &f.steam);
    assert(f.input.ResolveKeyboardFocusSurface(7, &f.steam) == &f.steam);
    assert(f.input.ResolveKeyboardFocusSurface(7, &f.game) == nullptr);
    assert(f.input.FindInputTargetAt(35, 15, hit) && hit.toplevelId == 7 && hit.surface == &host);

    GpuDesktopScene scene;
    assert(f.comp.SnapshotGpuDesktopScene({}, f.cache, scene));
    auto image = std::find_if(scene.layers.begin(), scene.layers.end(),
        [](const auto& layer) { return layer.key == ((1ULL << 63) | 4); });
    assert(image != scene.layers.end() && image->x > 20 && image->y == 30);
    assert(image->w == 77 && image->h == 48 && image->sourceW == 160 && image->sourceH == 100);
    assert(std::count_if(scene.layers.begin(), scene.layers.end(),
        [](const auto& layer) { return layer.key == ((1ULL << 63) | 4); }) == 1);
    const int oldX = image->x;
    {
        auto lock = f.tm.Lock(); f.tm.FindToplevelLocked(7)->SetPosition(30, 10);
    }
    assert(f.comp.SnapshotGpuDesktopScene({}, f.cache, scene));
    image = std::find_if(scene.layers.begin(), scene.layers.end(),
        [](const auto& layer) { return layer.key == ((1ULL << 63) | 4); });
    assert(image != scene.layers.end() && image->x == oldX + 10);
    // Fullscreen anchors the host at zero. On exit, restore the compositor's
    // dragged position even when Wine republishes its unchanged old position.
    {
        auto lock = f.tm.Lock();
        auto* window = f.tm.FindToplevelLocked(7);
        window->SetWinePosition(20, 10);
        f.comp.TrackExternalHostFullscreenLocked(7, true);
        window->ApplyFullscreen(true);
        assert(window->X() == 0 && window->Y() == 0);
        f.tm.SyncDesktopPositionLocked(7, 0, 0, false);
    }
    // A resize rebind must preserve the original windowed position.
    assert(f.comp.BindExternalToplevel(4, 7, 100, 100, 4, 20, 92, 48));
    {
        auto lock = f.tm.Lock();
        auto* window = f.tm.FindToplevelLocked(7);
        f.comp.TrackExternalHostFullscreenLocked(7, true); // repeated request
        f.comp.TrackExternalHostFullscreenLocked(7, false);
        window->ApplyFullscreen(false);
        f.tm.SyncDesktopPositionLocked(7, 20, 10, false);
        assert(window->X() == 30 && window->Y() == 10);
        window->SetPosition(35, 12);
        f.comp.TrackExternalHostFullscreenLocked(7, false);
        assert(window->X() == 35 && window->Y() == 12);
        window->SetPosition(30, 10);
        // Unbound game windows retain their existing fullscreen semantics.
        auto* game = f.tm.FindToplevelLocked(5);
        game->ApplyFullscreen(false); game->SetPosition(60, 20);
        f.comp.TrackExternalHostFullscreenLocked(5, true);
        game->ApplyFullscreen(true);
        f.comp.TrackExternalHostFullscreenLocked(5, false);
        game->ApplyFullscreen(false);
        assert(game->X() == 0 && game->Y() == 0);
    }
    // A scroll updates the real X11 cursor, retains fractional/accelerated
    // deltas, and never becomes a mouse-button click or reaches Wine.
    f.steam.client=Client(1);host.client=Client(2);
    wl_resource nativePointer{nullptr,Client(1)},winePointer{nullptr,Client(2)};
    pointers={&nativePointer,&winePointer};
    MoveGrabHandler grab;auto* manager=InputManager::GetInstance();
    manager->BindCompositorDeps(f.tm,f.input,grab,f.policy,f.root);
    manager->Initialize(reinterpret_cast<wl_display*>(1));
    manager->SendScrollEvent(7,0,0.25,40,70,50);DrainInput();
    assert(axes.size()==1&&axes[0].resource==&nativePointer&&!axes[0].discrete);
    assert(axes[0].value==wl_fixed_from_double(2.5));
    assert(std::none_of(events.begin(),events.end(),[](const auto& e){return e.name=="pointer.button";}));
    // Repeating a wheel at a stationary cursor must not inject another
    // absolute motion (which switches XI devices and resets CEF's baseline).
    const unsigned stationary = motionCount;
    manager->SendScrollEvent(7,0,1,40,70,50);DrainInput();
    manager->SendScrollEvent(7,0,-1,40,70,50);DrainInput();
    assert(motionCount==stationary);
    const double oldMotion=motionX;
    axes.clear();manager->SendScrollEvent(7,0,3,40,75,50);DrainInput();
    assert(axes.size()==1&&axes[0].value==wl_fixed_from_double(30)&&motionX>oldMotion);
    axes.clear();manager->SendScrollEvent(7,1,-15,-1,75,50);DrainInput();
    assert(axes.size()==1&&axes[0].axis==1&&axes[0].value==wl_fixed_from_double(-15));
    axes.clear();manager->SendScrollEvent(7,0,0,40,75,50);DrainInput();assert(axes.empty());
    // The overlay can still target the desktop root after focus/session
    // changes. The pointer position, rather than keyboard focus, owns scroll.
    axes.clear();manager->SendScrollEvent(f.root,0,1,40,75,50);DrainInput();
    assert(axes.size()==1&&axes[0].resource==&nativePointer&&axes[0].value==wl_fixed_from_double(10));
    assert(!manager->RequestExternalHostMove(4,7));
    manager->SendPointerEvent(7,1,75,50,0x110);DrainInput();
    assert(!manager->RequestExternalHostMove(5,7));
    assert(manager->RequestExternalHostMove(4,7));DrainInput();
    auto* server=WaylandServer::GetInstance();
    assert(server->movedHost==7);
    manager->SendPointerEvent(7,3,80,55,0);DrainInput();
    assert(server->moveX==wl_fixed_from_double(80)&&server->moveY==wl_fixed_from_double(55));
    manager->SendPointerEvent(7,2,75,50,0x110);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));DrainInput();
    // Request + motion in the SAME batch: producer has not observed an active
    // grab yet, so the motion has local coordinates as well as global ones.
    manager->SendPointerEvent(7,1,75,50,0x110);DrainInput();
    assert(manager->RequestExternalHostMove(4,7));
    manager->SendPointerEvent(7,3,85,55,0);DrainInput();
    assert(server->movedHost==7&&server->moveX==wl_fixed_from_double(85)&&
        server->moveY==wl_fixed_from_double(55));
    manager->SendPointerEvent(7,2,75,50,0x110);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));DrainInput();
    // Button-up before dequeue invalidates a pending move request.
    manager->SendPointerEvent(7,1,75,50,0x110);DrainInput();
    assert(manager->RequestExternalHostMove(4,7));
    manager->SendPointerEvent(7,2,75,50,0x110);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));DrainInput();
    assert(server->movedHost==0);
    assert(!manager->RequestExternalHostMove(4,7));
    assert(!manager->RequestExternalHostMove(4,7,10));
    manager->SendPointerEvent(7,1,75,50,0x110);DrainInput();
    assert(!manager->RequestExternalHostMove(4,7,3));
    assert(manager->RequestExternalHostMove(4,7,10));
    assert(manager->IsExternalHostResizing(7));
    DrainInput();
    assert(server->movedHost==7&&server->resizeEdges==10);
    manager->SendPointerEvent(7,2,75,50,0x110);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));DrainInput();
    assert(server->movedHost==0&&!manager->IsExternalHostResizing(7));
    // A delayed X11 request that reaches the queue after release cannot grab.
    manager->SendPointerEvent(7,1,75,50,0x110);DrainInput();
    assert(manager->RequestExternalHostMove(4,7,5));
    manager->SendPointerEvent(7,2,75,50,0x110);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));DrainInput();
    assert(server->movedHost==0&&!manager->IsExternalHostResizing(7));
    manager->Shutdown();
    {
        auto lock=f.tm.Lock();auto* window=f.tm.FindToplevelLocked(7);
        window->SetPosition(100,60);window->SetContentSize(640,400);
    }
    MoveGrabHandler resizing;
    resizing.StartExternalResizeGrab(f.tm,7,1,100,60,5);
    assert(resizing.ResizeTarget()==7);
    assert(resizing.ProcessMoveGrabMotion(f.tm,125,95));
    assert(resizing.ResizeBounds().x==125&&resizing.ResizeBounds().y==95);
    assert(resizing.ResizeBounds().width==615&&resizing.ResizeBounds().height==365);
    assert(resizing.ProcessMoveGrabMotion(f.tm,125,95));
    assert(resizing.ResizeBounds().x==125&&resizing.ResizeBounds().width==615);
    resizing.EndMoveGrab(f.tm);assert(!resizing.IsActive()&&!resizing.ResizeTarget());
    {
        auto lock=f.tm.Lock();auto* window=f.tm.FindToplevelLocked(7);
        window->SetPosition(30,10);window->SetContentSize(100,72);
    }
    {
        auto lock = f.tm.Lock(); f.tm.FindToplevelLocked(7)->SetMinimized(true);
    }
    assert(f.comp.SnapshotGpuDesktopScene({}, f.cache, scene));
    assert(std::none_of(scene.layers.begin(), scene.layers.end(),
        [](const auto& layer) { return layer.key == ((1ULL << 63) | 4); }));
    {
        auto lock = f.tm.Lock(); f.tm.FindToplevelLocked(7)->SetMinimized(false);
    }
    assert(f.input.FindInputTargetAt(70, 50, hit) && hit.toplevelId == 7);
    f.tm.UnregisterSurfaceResource(Key(4));
    { auto lock = f.tm.Lock(); f.tm.EraseToplevelLocked(4); }
    assert(f.input.ResolveKeyboardFocusSurface(7, &host) == &host);
    assert(f.comp.SnapshotGpuDesktopScene({}, f.cache, scene));
    assert(std::none_of(scene.layers.begin(), scene.layers.end(),
        [](const auto& layer) { return layer.key == ((1ULL << 63) | 4); }));
    f.comp.UnbindExternalToplevel(7);
    std::puts("PASS real Wine host: peer identity, shared GPU/input geometry, move, minimize, restore, retired surface");
    return 0;
}
