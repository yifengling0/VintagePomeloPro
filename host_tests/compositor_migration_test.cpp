#include "compositor/toplevel/desktop_compositor.h"
#include "compositor/input/input_resolver.h"
#include "compositor/toplevel/toplevel_manager.h"
#include "compositor/frame/surface_data.h"
#include "compositor/toplevel/popup_manager.h"
#include "graphics/graphics_broker.h"
#include "bridge/plugin_manager.h"
#include <cstdio>
#include <cstring>
#include <cmath>

static int checks = 0, failures = 0;
#define CHECK(c, msg) do { ++checks; if (!(c)) { ++failures; std::printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

struct Scene {
    ToplevelManager tm;
    DisplayPolicy policy{true};
    uint32_t root = 1;
    int32_t rw, rh;
    DesktopCompositor comp;
    InputResolver input;
    SurfaceData parentData, childData;
    wl_resource parent{&parentData}, child{&childData};
    std::vector<uint8_t> pixels;
    int width = 0, height = 0;
    PresentedFrame frame;

    Scene(int w = 1416, int h = 640) : rw(w), rh(h),
        comp(tm, policy, root, rw, rh), input(tm, comp, root, rw, rh) {
        setWindow(1, rw, rh, 0xff111111);
        parentData.hasToplevel = true;
        parentData.toplevelId = 2;
        parentData.surfaceKey = 2;
        parentData.surface = &parent;
        childData.surface = &child;
        childData.surfaceKey = 3;
        childData.isSubsurface = true;
        childData.inputRegionEmpty = true;
        childData.parentSurface = &parent;
        tm.MapToplevelSurface(2, &parent);
        tm.RegisterSurfaceResource(2, &parent);
        tm.RegisterSurfaceResource(3, &child);
    }
    void setWindow(uint32_t id, int w, int h, uint32_t color) {
        auto lk = tm.Lock();
        auto& s = tm.EnsureToplevelLocked(id);
        s.SetContentSize(w, h);
        s.FrameData().resize(size_t(w) * h * 4);
        for (size_t p = 0; p < s.FrameData().size(); p += 4)
            std::memcpy(s.FrameData().data() + p, &color, 4);
        s.BumpFrameSerial();
        s.MarkFirstCommit(0, 0);
        s.MarkDirty();
        if (!tm.IsInZOrder(id)) tm.AddToZOrder(id);
        comp.MarkDesktopRootDirtyLocked();
    }
    void fullscreen(bool on) {
        auto lk = tm.Lock();
        tm.EnsureToplevelLocked(2).ApplyFullscreen(on);
        comp.MarkDesktopRootDirtyLocked();
    }
    void sub(int x, int y, int w, int h, uint32_t color, int damageW = 0, bool opaque = true) {
        { auto lk = tm.Lock();
        childData.w = w; childData.h = h;
        childData.vpDstW = w; childData.vpDstH = h;
        childData.subsurfaceX = x; childData.subsurfaceY = y;
        DesktopCompositor::SubsurfaceLayer s;
        s.surface = &child; s.surfaceKey = 3; s.parentToplevel = 2;
        s.route = policy.RouteForSubsurface(childData.inputRegionEmpty, childData.vpSrcW);
        s.x = s.localX = x; s.y = s.localY = y; s.w = w; s.h = h;
        s.vpDstW = w; s.vpDstH = h; s.opaque = opaque; s.shmFormat = opaque ? 1 : 0;
        s.dmgW = damageW; s.dmgH = damageW;
        s.shmCommitSerial = ++childData.shmCommitSerial;
        std::vector<uint8_t> p(size_t(w) * h * 4);
        for (size_t i = 0; i < p.size(); i += 4) std::memcpy(p.data() + i, &color, 4);
        comp.UpsertSubsurfaceLayer(std::move(s), std::move(p));
        comp.MarkDesktopRootDirtyLocked();
        }
        if (comp.zc().IsActive(3)) comp.zc().NoteLayerConsumed(3, 2, 0, w, h);
    }
    bool take() {
        const bool ready = comp.TakeToplevelFrame(root, pixels, frame);
        if (ready) { width = frame.w; height = frame.h; }
        return ready;
    }
    uint32_t pixel(int x, int y) {
        uint32_t p = 0;
        if (x < width && y < height) std::memcpy(&p, pixels.data() + (size_t(y) * width + x) * 4, 4);
        return p;
    }
};


static GpuDesktopScene Snapshot(Scene& s) {
    s.childData.protocolId = 3;
    ZeroCopyLayerInfo info;
    CHECK(s.comp.GetZeroCopyLayerInfo(3, 1, s.childData.w, s.childData.h, info), "resolve live child");
    auto native = MakeNativeWindowLayer(3, info, s.childData.w, s.childData.h);
    GpuDesktopSnapshotCache cache;
    GpuDesktopScene result;
    CHECK(s.comp.SnapshotGpuDesktopScene({}, cache, result, {native}), "snapshot production GPU scene");
    return result;
}
static GpuDesktopLayer Native(const GpuDesktopScene& scene) {
    for (auto layer : scene.layers) if (layer.zeroCopyKey == 3) return layer;
    return {};
}
static uint32_t BackgroundPixel(const GpuDesktopScene& scene, int x, int y) {
    uint32_t value = 0;
    for (const auto& layer : scene.layers) {
        if (x < layer.x || y < layer.y || x >= layer.x+layer.w || y >= layer.y+layer.h) continue;
        if (layer.solidBlack) value = 0xff000000;
        else if (layer.pixels && layer.sourceW > 0 && layer.sourceH > 0) {
            const int sx = (x-layer.x)*layer.sourceW/layer.w;
            const int sy = (y-layer.y)*layer.sourceH/layer.h;
            std::memcpy(&value, layer.pixels->data()+(size_t(sy)*layer.sourceW+sx)*4, 4);
        }
    }
    return value;
}
static void Activate(Scene& s) {
    CHECK(s.comp.zc().NoteLayerConsumed(3, 1, 0, s.childData.w, s.childData.h), "publish consumed extent");
    s.comp.zc().Activate(3, 1);
}
static bool Rect(const GpuDesktopLayer& layer, int x, int y, int w, int h) {
    std::printf("actual native rect=(%d,%d,%d,%d), expected=(%d,%d,%d,%d)\n",
                layer.x,layer.y,layer.w,layer.h,x,y,w,h);
    return layer.x==x && layer.y==y && layer.w==w && layer.h==h;
}
static GpuDesktopScene DirectSnapshot(Scene& s, int w, int h) {
    constexpr uint64_t key = (100ull << 32) | 3;
    s.childData.surfaceKey = key;
    s.childData.directOwnerKey = 2;
    s.childData.directVisible = true;
    s.childData.directGeneration = 10;
    s.tm.RegisterSurfaceResource(key, &s.child);
    GpuDesktopScene scene; GpuDesktopSnapshotCache cache;
    CHECK(s.comp.SnapshotGpuDesktopScene({{100,2,3,w,h,10}},cache,scene), "Direct scene snapshot");
    return scene;
}
static GpuDesktopLayer DirectLayer(const GpuDesktopScene& scene) {
    for (const auto& layer : scene.layers) if (layer.directSurfaceKey) return layer;
    return {};
}
int main() {
    { // A translucent GL child must not be replaced by its black parent.
        Scene s(80, 60); s.setWindow(2, 40, 30, 0xff000000); s.fullscreen(true);
        s.sub(0, 0, 40, 30, 0x80800000, 0, false);
        CHECK(s.take() && s.frame.kind == PresentedFrame::Kind::Composed,
              "ARGB child requires composition instead of parent direct pass");
        CHECK((s.pixel(s.width/2, s.height/2) & 0x00ff0000) != 0,
              "visible child color reaches fullscreen output");
        s.sub(0, 0, 40, 30, 0x80008000, 0, false);
        CHECK(s.take() && (s.pixel(s.width/2, s.height/2) & 0x0000ff00) != 0,
              "new ARGB child frame updates fullscreen output");
        s.sub(0, 0, 40, 30, 0xff123456, 0, false);
        CHECK(s.take() && s.pixel(s.width/2, s.height/2) == 0xff123456,
              "unclassified opaque ARGB reaches output on its first frame");
        CHECK(s.frame.kind == PresentedFrame::Kind::DirectPass,
              "opaque ARGB child uses GPU scaling after classification");
        s.sub(0, 0, 40, 30, 0x80000080, 0, false);
        CHECK(s.take() && s.frame.kind == PresentedFrame::Kind::Composed,
              "next transparent commit invalidates cached opacity");
        CHECK((s.pixel(s.width/2, s.height/2) & 0x000000ff) != 0,
              "transparent update remains visible after an opaque direct frame");
    }

    { // Managed WSI binds the GPU queue to the toplevel, while the client
      // SHM surface can retain a full-window white frame. GPU must cover it,
      // just as a whole-window NativeImage layer covers inline SHM children.
      Scene s(800,600); s.policy = DisplayPolicy{false}; s.root = 0;
      s.setWindow(2,408,334,0xffffffff); s.sub(4,30,400,300,0xffffffff);
      // The parent's viewport crops the decorated SHM buffer. It is not a
      // viewport of the smaller native swapchain image (Edge: 968x674/960x640).
      s.parentData.directViewport.width=408; s.parentData.directViewport.height=334;
      s.parentData.directViewport.destinationW=408; s.parentData.directViewport.destinationH=334;
      s.childData.directViewport.destinationW=400; s.childData.directViewport.destinationH=300;
      constexpr uint64_t parentKey = (100ull << 32) | 2;
      s.parentData.surfaceKey = parentKey;
      s.tm.RegisterSurfaceResource(parentKey, &s.parent);
      DirectDesktopLayout layout;
      CHECK(s.comp.GetDirectDesktopLayout(100,2,2,400,300,10,layout,2),
            "managed whole-window Direct resolves its own output");
      CHECK(layout.x==4 && layout.y==30 && layout.w==400 && layout.h==300,
            "managed Direct uses client coordinates rather than decorated SHM viewport");
      CHECK(layout.occluders.empty(),
            "managed client SHM does not occlude its own GPU content");
      s.childData.directViewport.destinationW=200; s.childData.directViewport.destinationH=150;
      CHECK(s.comp.GetDirectDesktopLayout(100,2,2,400,300,10,layout,2) &&
            layout.x==4 && layout.y==30 && layout.w==200 && layout.h==150,
            "managed Direct follows client viewport resize independently of parent SHM");
      SurfaceData sibling; wl_resource siblingResource{&sibling};
      sibling.surface=&siblingResource; sibling.isSubsurface=true;
      sibling.parentSurface=&s.parent; sibling.inputRegionEmpty=false;
      s.tm.RegisterSurfaceResource(9, &siblingResource);
      CHECK(s.comp.GetDirectDesktopLayout(100,2,2,400,300,10,layout,2),
            "popup input role cannot replace the managed GPU client");
      sibling.inputRegionEmpty=true;
      const char* rejection=nullptr;
      CHECK(!s.comp.GetDirectDesktopLayout(100,2,2,400,300,10,layout,2,&rejection) &&
            rejection && std::strcmp(rejection,"ambiguous_managed_client")==0,
            "ambiguous legacy client identity is rejected instead of borrowing another drawable");
      { auto lk=s.tm.Lock(); s.tm.UnregisterSurfaceResource(9); }
      CHECK(s.comp.GetDirectDesktopLayout(100,2,2,400,300,10,layout,2),
            "managed Direct recovers after a competing client retires");
      CHECK(!s.comp.GetDirectDesktopLayout(100,2,2,400,300,10,layout,4),
            "managed Direct cannot draw into another window");
      CHECK(!s.comp.GetDirectDesktopLayout(101,2,2,400,300,10,layout,2),
            "managed Direct cannot borrow another process surface");
      { auto lk=s.tm.Lock(); s.tm.EnsureToplevelLocked(2).SetMinimized(true); }
      CHECK(!s.comp.GetDirectDesktopLayout(100,2,2,400,300,10,layout,2),
            "managed Direct honors minimization"); }
    { // A producer bound directly to an undecorated window has no child.
      Scene s(800,600); s.policy=DisplayPolicy{false}; s.root=0;
      { auto lk=s.tm.Lock(); s.tm.UnregisterSurfaceResource(3); }
      s.setWindow(2,400,300,0xff334455);
      constexpr uint64_t parentKey=(100ull<<32)|2;
      s.parentData.surfaceKey=parentKey; s.tm.RegisterSurfaceResource(parentKey,&s.parent);
      s.parentData.directViewport.width=512; s.parentData.directViewport.height=512;
      DirectDesktopLayout layout;
      CHECK(s.comp.GetDirectDesktopLayout(100,2,2,400,300,10,layout,2) &&
            layout.x==0 && layout.y==0 && layout.w==400 && layout.h==300,
            "whole-window native image does not inherit an unrelated SHM crop");
      const char* rejection=nullptr;
      CHECK(!s.comp.GetDirectDesktopLayout(100,2,2,320,240,10,layout,2,&rejection) &&
            rejection && std::strcmp(rejection,"managed_client_pending")==0,
            "smaller image waits for its client identity instead of drawing over decorations"); }
    { // Independently declared child drawables still obey sibling stacking.
      Scene s(800,600); s.policy = DisplayPolicy{false}; s.root = 0;
      s.setWindow(2,400,300,0xff334455); s.sub(20,30,200,100,0xff778899);
      constexpr uint64_t childKey = (100ull << 32) | 3;
      s.childData.surfaceKey=childKey; s.childData.directOwnerKey=2;
      s.childData.directVisible=true; s.childData.directGeneration=10;
      s.tm.RegisterSurfaceResource(childKey, &s.child);
      SurfaceData menuData; wl_resource menu{&menuData};
      DesktopCompositor::SubsurfaceLayer layer;
      layer.surface=&menu; layer.surfaceKey=5; layer.parentToplevel=2;
      layer.route=DisplayPolicy::SubsurfaceRoute::InlineClient;
      layer.localX=50; layer.localY=60; layer.w=layer.h=40;
      { auto lk=s.tm.Lock(); s.comp.UpsertSubsurfaceLayer(std::move(layer), std::vector<uint8_t>(40*40*4,255)); }
      DirectDesktopLayout layout;
      CHECK(s.comp.GetDirectDesktopLayout(100,2,3,200,100,10,layout,2),
            "managed child Direct resolves its declared owner");
      CHECK(layout.x==20 && layout.y==30 && layout.occluders.size()==1 &&
            layout.occluders[0].x==50 && layout.occluders[0].y==60,
            "managed child Direct keeps later sibling occlusion");
      CHECK(!s.comp.GetDirectDesktopLayout(100,2,3,200,100,11,layout,2),
            "managed child Direct rejects stale generations"); }
    { Scene s(800,600);s.setWindow(2,400,300,0xff334455);s.fullscreen(true);
      s.childData.subsurfaceX=100;s.childData.subsurfaceY=50;
      auto scene=DirectSnapshot(s,200,100);
      CHECK(Rect(DirectLayer(scene),200,100,400,200), "Direct video keeps parent canvas and offset");
      CHECK(BackgroundPixel(scene,20,20)==0xff334455, "Direct video retains parent surroundings");
      double x=0,y=0; CHECK(s.input.SurfaceLocalToDesktop(&s.parent,100,50,x,y) &&
                             x==200 && y==100, "Direct video and input share parent fit"); }
    { Scene s(800,600);s.setWindow(2,400,300,0xff334455);s.fullscreen(true);
      auto scene=DirectSnapshot(s,400,300);
      CHECK(Rect(DirectLayer(scene),0,0,800,600), "Direct full canvas fit");
      bool parentPixels=false;for(const auto& layer:scene.layers)
          if(layer.key==((1ull<<63)|2) && layer.pixels)parentPixels=true;
      CHECK(!parentPixels, "Direct full canvas skips redundant parent pixel snapshot"); }
    { Scene s(800,600);s.setWindow(2,400,300,0xff334455);s.fullscreen(true);
      s.childData.directViewport.destinationW=200;
      s.childData.directViewport.destinationH=100;
      auto scene=DirectSnapshot(s,400,300);
      CHECK(Rect(DirectLayer(scene),0,0,400,200), "Direct viewport preserves parent scale");
      CHECK(BackgroundPixel(scene,700,500)==0xff334455, "Direct viewport preserves outside pixels"); }
    { Scene s; s.setWindow(2,800,600,0xff334455); s.fullscreen(true);
      s.sub(0,0,800,600,0xff778899); Activate(s);
      FitRect fit; ComputeFitRect(s.rw,s.rh,800,600,fit);
      CHECK(Rect(Native(Snapshot(s)),fit.offX,fit.offY,fit.dstW,fit.dstH), "final scene fullscreen native fit"); }
    { Scene s(800,600); s.setWindow(2,400,300,0xff334455); s.fullscreen(true);
      s.sub(100,50,200,100,0xff778899); Activate(s);
      auto scene=Snapshot(s);
      CHECK(Rect(Native(scene),200,100,400,200), "final scene video retains parent offset and scale");
      CHECK(BackgroundPixel(scene,20,20)==0xff334455, "final scene retains parent pixels outside video"); }
    { Scene s(800,600); s.setWindow(2,400,300,0xff334455); s.fullscreen(true);
      s.childData.w=s.childData.vpDstW=400; s.childData.h=s.childData.vpDstH=300; Activate(s);
      CHECK(Rect(Native(Snapshot(s)),0,0,800,600), "final scene protocol-only native fit"); }
    { Scene s(800,600); s.setWindow(2,400,300,0xff334455); s.fullscreen(true);
      s.sub(0,0,400,300,0xff778899); Activate(s);
      s.childData.vpDstW=200; s.childData.vpDstH=100;
      auto scene=Snapshot(s);
      CHECK(Rect(Native(scene),0,0,400,200), "final scene shrinking viewport preserves parent scale");
      CHECK(BackgroundPixel(scene,700,500)==0xff334455, "final scene restores surroundings after viewport shrink"); }
    { Scene s(800,600); s.setWindow(2,400,300,0xff334455); s.fullscreen(true);
      s.sub(0,0,400,300,0xff778899); Activate(s);
      SurfaceData menuData; wl_resource menu{&menuData};
      DesktopCompositor::SubsurfaceLayer layer;
      layer.surface=&menu; layer.surfaceKey=5; layer.parentToplevel=2;
      layer.localX=layer.x=100; layer.localY=layer.y=50;
      layer.w=layer.h=layer.vpDstW=layer.vpDstH=100;
      { auto lk=s.tm.Lock(); s.comp.UpsertSubsurfaceLayer(std::move(layer), std::vector<uint8_t>(100*100*4,255)); }
      auto scene=Snapshot(s); bool menuOk=false;
      for (const auto& item:scene.layers) if (item.ownerSurfaceKey==5)
          menuOk=item.x==200 && item.y==100 && item.w==200 && item.h==200;
      CHECK(menuOk,"final scene menu uses displayed parent coordinates");
      s.setWindow(4,400,300,0xffabcdef);
      {auto lk=s.tm.Lock();s.tm.EnsureToplevelLocked(4).ApplyFullscreen(true);}
      CHECK(Native(Snapshot(s)).zeroCopyKey==0,"final scene excludes non-selected fullscreen producer"); }
    { // Whole-frame rebuild must replay retained pixels outside latest damage.
        Scene s(100, 100); s.setWindow(2, 100, 100, 0xff000000);
        s.sub(0, 0, 80, 80, 0xff123456, 5);
        CHECK(s.take() && s.pixel(70, 70) == 0xff123456,
              "first composite includes pixels outside last damage");
        s.setWindow(2, 100, 100, 0xffabcdef);
        CHECK(s.take() && s.pixel(70, 70) == 0xff123456,
              "lower-layer damage replays the entire overlapping static child");
    }
    { // Partial snapshot intersection uses displayed, not logical, bounds.
        Scene s(400, 100); s.setWindow(2, 50, 100, 0xff345678); s.fullscreen(true);
        s.setWindow(4, 1, 1, 0xff000000); // force composition, no direct shortcut
        CHECK(s.take(), "initial letterboxed composite");
        s.setWindow(2, 50, 100, 0xffabcdef);
        CHECK(s.take() && s.pixel(190, 50) == 0xffabcdef,
              "partial fullscreen redraw includes a layer outside its logical rectangle");
    }
    { // Same-sized direct output is NOT a cached composed output.
        Scene s(80, 60); s.setWindow(2, 80, 60, 0xff123456);
        s.sub(0, 0, 80, 60, 0xff654321);
        CHECK(s.take(), "initial windowed composite");
        s.fullscreen(true); CHECK(s.take(), "direct fullscreen frame");
        s.fullscreen(false);
        s.sub(0, 0, 80, 60, 0xffabcdef, 2);
        CHECK(s.take() && s.pixel(70, 50) == 0xffabcdef,
              "direct-to-composite invalidates whole frame even with equal byte count");
    }
    { // Popup window size must not overwrite its smaller GL pixel buffer.
        ToplevelManager tm;
        int32_t outputW = 800, outputH = 600;
        PopupManager popups(tm);
        const uint32_t parentId = tm.AllocateToplevelId();
        { auto lk = tm.Lock();
          auto& st = tm.EnsureToplevelLocked(parentId);
          st.SetContentSize(400, 300); st.MarkFirstCommit(0, 0); st.ApplyFullscreen(true); }
        SurfaceData parent, child;
        parent.toplevelId = parentId;
        wl_resource resource{&child};
        child.surface = &resource; child.surfaceKey = 900;
        child.w = 400; child.h = 300; child.pixels.resize(400 * 300 * 4, 128);
        ShmCommitInfo fi;
        auto first = popups.UpdatePopupOnCommit(&child, &resource, &parent, fi);
        CHECK(first.isNew && !first.sizeChanged && first.winW == 400 && first.dispW == 400,
              "popup reports content size (fullscreen-parent output heuristic retired)");
        { auto lk = tm.Lock(); auto* st = tm.FindToplevelLocked(first.popupId);
          CHECK(st && st->Width() == 400 && st->Pixels().size() == 400 * 300 * 4,
                "popup frame keeps actual pixel dimensions"); }
        child.pixels.resize(400 * 300 * 4, 128);
        auto repeat = popups.UpdatePopupOnCommit(&child, &resource, &parent, fi);
        CHECK(!repeat.isNew && !repeat.sizeChanged && repeat.popupId == first.popupId,
              "same-sized popup commit does not duplicate show/resize");
        parent.committed.contentRect.x = 5; parent.committed.contentRect.y = 7;
        child.subsurfaceX = 25; child.subsurfaceY = 37;
        child.pixels.resize(400 * 300 * 4, 128);
        auto moved = popups.UpdatePopupOnCommit(&child, &resource, &parent, fi);
        CHECK(moved.posChanged && moved.offX == 20 && moved.offY == 30,
              "popup offset consumes named parent content geometry");
        uint32_t removed = 0;
        { auto lk = tm.Lock();
          CHECK(popups.RemovePopupBySurfaceKeyLocked(900, removed) == parentId &&
                removed == first.popupId && !tm.FindToplevelLocked(first.popupId),
                "popup unmap removes both key and frame state"); }
        CHECK(tm.GetSurfaceForToplevel(first.popupId) == nullptr,
              "popup unmap also clears input surface association");
    }
    { // A restore frame must keep the compositor position of a dragged window.
        Scene s; s.setWindow(2, 400, 300, 0xff123456);
        auto lk = s.tm.Lock(); auto& st = s.tm.EnsureToplevelLocked(2);
        st.SetPosition(80, 90); st.SetWinePosition(-32000, -32000); st.SetMinimized(true);
        const bool restored = s.tm.TryAutoRestoreLocked(2, 400, 300);
        s.tm.SyncDesktopPositionLocked(2, 0, 0, restored);
        CHECK(restored && !st.IsMinimized() && st.X() == 80 && st.Y() == 90,
              "restore ignores stale Wine geometry after a compositor drag");
    }
    { // Game fit is local presentation, never a Wine fullscreen configure.
        Scene s(1600, 900); s.setWindow(2, 400, 300, 0xff123456);
        {
            auto lk = s.tm.Lock(); auto& st = s.tm.EnsureToplevelLocked(2);
            st.SetPosition(80, 90); st.SetWinePosition(80, 90);
            st.SetGamePresentation(true);
            CHECK(st.IsPresentationFullscreen() && !st.IsFullscreen(), "host fit does not change guest state");
            CHECK(st.X() == 80 && st.Y() == 90, "host fit preserves windowed position");
            CHECK(s.tm.HandleCommittedSizeLocked(2, 1, 400, 300, 1600, 900) ==
                  ToplevelManager::SizeCommitEffect::ResizeEvent, "host fit never reasserts guest fullscreen size");
        }
        const auto scene = DirectSnapshot(s, 400, 300);
        CHECK(Rect(DirectLayer(scene), 200, 0, 1200, 900), "Game GPU output fits original content");
        double x = 0, y = 0;
        CHECK(s.input.SurfaceLocalToDesktop(&s.parent, 200, 150, x, y) && x == 800 && y == 450,
              "Game pointer warp shares the GPU fit");
        {
            auto lk = s.tm.Lock(); auto& st = s.tm.EnsureToplevelLocked(2);
            st.SetGamePresentation(false);
            CHECK(!st.IsPresentationFullscreen() && st.Width() == 400 && st.Height() == 300 &&
                  st.X() == 80 && st.Y() == 90, "leaving Game restores exact guest geometry");
            st.ApplyFullscreen(true); st.SetGamePresentation(true); st.SetGamePresentation(false);
            CHECK(st.IsFullscreen() && st.IsPresentationFullscreen(), "leaving Game preserves real fullscreen");
        }
    }
    { // Decorated game: crop by the live inline client, not a guessed border width.
        Scene s(1600, 900); s.setWindow(2, 806, 656, 0xffffffff);
        s.sub(3, 53, 800, 600, 0xff123456);
        { auto lk = s.tm.Lock(); s.tm.EnsureToplevelLocked(2).SetGamePresentation(true); }
        Activate(s);
        const auto scene = Snapshot(s);
        CHECK(Rect(Native(scene), 200, 0, 1200, 900), "Game presents the client without Wine decoration");
        CHECK(BackgroundPixel(scene, 100, 50) == 0xff000000,
              "Game letterbox contains no title or retained desktop pixels");
        InputTarget hit;
        CHECK(s.input.FindInputTargetAt(800, 450, hit) && hit.surface == &s.parent &&
              hit.localX == 403 && hit.localY == 353, "client center maps to Wine window coordinates");
        CHECK(s.input.FindInputTargetAt(0, 450, hit) && hit.swallow && hit.localX == 3,
              "black bar clamps at the client origin, outside decoration");
        double x = 0, y = 0;
        CHECK(s.input.SurfaceLocalToDesktop(&s.parent, 403, 353, x, y) && x == 800 && y == 450,
              "decorated client warp and hit mapping are inverse");
        { auto lk = s.tm.Lock(); s.tm.EnsureToplevelLocked(2).SetGamePresentation(false); }
        CHECK(s.tm.FindToplevelLocked(2)->Width() == 806 &&
              s.tm.FindToplevelLocked(2)->Height() == 656, "Game leaves the Wine outer geometry intact");
    }
    { // The same client geometry applies to bounded SHM upload and Direct.
        Scene s(1600, 900); s.setWindow(2, 806, 656, 0xffffffff);
        s.sub(3, 53, 800, 600, 0xff123456);
        { auto lk = s.tm.Lock(); s.tm.EnsureToplevelLocked(2).SetGamePresentation(true); }
        CHECK(s.take() && s.frame.kind == PresentedFrame::Kind::DirectPass &&
              s.width == 800 && s.height == 600 && s.pixel(0,0) == 0xff123456,
              "SHM client bypass keeps only original game pixels");
        CHECK(Rect(DirectLayer(DirectSnapshot(s, 800, 600)), 200, 0, 1200, 900),
              "Direct shares the client fit used by VirGL/Venus");
        // A second declared client is ambiguous; do not select a video by area.
        SurfaceData extra; wl_resource resource{&extra};
        extra.isSubsurface = true; extra.parentSurface = &s.parent;
        extra.inputRegionEmpty = true; extra.surfaceKey = 99;
        s.tm.RegisterSurfaceResource(99, &resource);
        FitRect fit;
        { auto lk = s.tm.Lock();
          CHECK(s.comp.ComputeFullscreenFitLocked(2,1600,900,fit) && !fit.clientOnly,
                "ambiguous clients retain whole-window fallback"); }
        s.tm.UnregisterSurfaceResource(99);
    }
    {
        Scene s(120, 100);
        s.setWindow(2, 30, 24, 0xffcccccc);
        CHECK(s.take() && s.frame.baseSerial == 0, "initial composition has no baseline");
        const auto firstSerial = s.frame.serial;
        const auto commitPixel = [&](uint32_t id, int x, int y, uint32_t value) {
            auto lock = s.tm.Lock();
            auto& state = s.tm.EnsureToplevelLocked(id);
            std::memcpy(state.FrameData().data() + (x + y * state.Width()) * 4, &value, 4);
            PixelDamage damage;
            damage.AddRequest(x, y, 1, 1);
            state.BumpFrameSerial(damage);
            if (id == s.root) s.comp.IncrementDesktopRootFrameSerial();
            s.comp.MarkDesktopRootDirtyLocked();
        };
        commitPixel(2, 5, 4, 0xff123456);
        commitPixel(1, 50, 41, 0xffabcdef);
        CHECK(s.take() && s.frame.baseSerial == firstSerial && s.frame.damage.Bounds().left == 5 &&
              s.frame.damage.Bounds().right == 51 && s.frame.damage.end() - s.frame.damage.begin() == 2,
              "desktop upload keeps child and backdrop updates in disjoint regions");
        CHECK(s.pixel(5, 4) == 0xff123456 && s.pixel(50, 41) == 0xffabcdef,
              "the composition reflects both updated sources");

        commitPixel(2, 5, 4, 0xff123457);
        commitPixel(2, 8, 14, 0xff123458);
        CHECK(s.take() && s.frame.damage.Bounds().Width() == 30 &&
              s.pixel(5, 4) == 0xff123457 && s.pixel(8, 14) == 0xff123458,
              "skipped child commits cause a full child repaint");
        {
            auto lock = s.tm.Lock();
            s.tm.EnsureToplevelLocked(2).SetPosition(20, 25);
            s.comp.MarkDesktopRootDirtyLocked();
        }
        CHECK(s.take() && s.frame.baseSerial == 0 && s.pixel(5, 4) == 0xff111111 &&
              s.pixel(25, 29) == 0xff123457, "moving a layer clears its old pixels");
    }
    std::printf("compositor migration: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
