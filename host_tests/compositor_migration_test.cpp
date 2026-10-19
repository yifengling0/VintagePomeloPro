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
    void sub(int x, int y, int w, int h, uint32_t color, int damageW = 0) {
        { auto lk = tm.Lock();
        childData.w = w; childData.h = h;
        childData.vpDstW = w; childData.vpDstH = h;
        childData.subsurfaceX = x; childData.subsurfaceY = y;
        DesktopCompositor::SubsurfaceLayer s;
        s.surface = &child; s.surfaceKey = 3; s.parentToplevel = 2;
        s.x = s.localX = x; s.y = s.localY = y; s.w = w; s.h = h;
        s.vpDstW = w; s.vpDstH = h; s.opaque = true;
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
    std::printf("compositor migration: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
