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
        auto lk = tm.Lock();
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

int main() {
    { // Fallback keeps GPU ownership until a strictly newer SHM frame exists.
        Scene s;
        auto& zc = s.comp.zc();
        zc.BindSurface(3, 10); zc.Activate(3, 1);
        CHECK(zc.IsActive(3) && zc.IsReadyPublished(3) &&
              winehua::GraphicsBroker::GetInstance().ready[3], "GPU activation publishes readiness");
        zc.BeginFallback(3, 20, true, 1);
        CHECK(zc.IsActive(3) && !zc.IsReadyPublished(3) &&
              !winehua::GraphicsBroker::GetInstance().ready[3], "fallback revokes guest ready before CPU ownership");
        CHECK(!zc.ConfirmFallback(3, 20) && zc.IsActive(3), "stale SHM cannot confirm fallback");
        CHECK(zc.ConfirmFallback(3, 21) && !zc.IsActive(3), "fresh SHM restores CPU ownership");
        zc.Activate(3, 1); zc.BeginFallback(3, 21, true, 1); zc.CancelFallback(3); zc.Activate(3, 1);
        CHECK(zc.IsReadyPublished(3) && !zc.IsFallbackPending(3), "GPU recovery cancels pending fallback");
        zc.Release(3, 1); zc.Release(3, 1);
        CHECK(!zc.IsActive(3) && !zc.IsReadyPublished(3), "release is idempotent");
    }
    { // Missing renderer capabilities must choose composition, preserving pixels.
        Scene s(800, 600); s.setWindow(2, 400, 300, 0xff123456);
        s.sub(0, 0, 400, 300, 0xff654321); s.fullscreen(true);
        PluginManager::GetInstance()->policy.bits = 0;
        CHECK(s.take() && s.frame.kind == PresentedFrame::Kind::Composed && s.width == 800,
              "insufficient direct-pass capabilities select composition");
        PluginManager::GetInstance()->policy.bits = winehua::kDirectPassCapabilitiesAll;
    }
    { // Direct buffer size must never replace desktop input coordinates.
        Scene s(800, 600); s.setWindow(2, 400, 300, 0xff123456);
        s.sub(0, 0, 400, 300, 0xff654321); s.fullscreen(true);
        CHECK(s.take() && s.frame.kind == PresentedFrame::Kind::DirectPass,
              "fullscreen SHM produces a direct frame");
        CHECK(s.frame.w == 400 && s.frame.h == 300 &&
              s.frame.contentW == 800 && s.frame.contentH == 600 &&
              s.frame.baseSpace == PresentedFrame::BaseSpace::Desktop,
              "direct buffer and input coordinate spaces remain distinct");
        CHECK(s.frame.pixels == s.pixels.data() && s.frame.opaque,
              "direct frame publishes caller-owned opaque pixels");
        s.policy.desktop = false;
        CHECK(s.comp.TakeToplevelFrame(2, s.pixels, s.frame) &&
              s.frame.baseSpace == PresentedFrame::BaseSpace::Window &&
              s.frame.contentW == 400 && s.frame.w == 400,
              "window composer acquires its own lock and publishes local coordinates");
    }
    { // War3: temporary window -> fullscreen -> real content -> GPU -> resize.
        Scene s;
        s.setWindow(2, 128, 128, 0xff000000); s.fullscreen(true); s.take();
        s.setWindow(2, 800, 600, 0xff000000); s.sub(0, 0, 800, 600, 0xff334455);
        s.comp.zc().SetEnabled(3, true);
        InputTarget t;
        CHECK(s.input.FindInputTargetAt(1100, 500, t), "War3 input resolves");
        CHECK(t.contentW == 800 && t.contentH == 600, "War3 input never uses temporary 128x128");
        CHECK((1100 - t.originX) / t.scale > 700, "War3 right-side coordinate is not clamped to 127");
        ZeroCopyLayerInfo g;
        CHECK(s.comp.GetZeroCopyLayerInfo(3, 1, 800, 600, g), "War3 GPU geometry resolves");
        FitRect fit; ComputeFitRect(s.rw, s.rh, 800, 600, fit);
        CHECK(g.x == fit.offX && g.y == fit.offY && g.width == fit.dstW && g.height == fit.dstH,
              "GPU and input share the parent fit");
        double dx, dy;
        CHECK(s.input.SurfaceLocalToDesktop(&s.parent, 750, 500, dx, dy), "cursor warp resolves");
        CHECK(std::abs((dx - t.originX) / t.scale - 750) < .001, "warp and input are inverse");
        s.setWindow(2, 1024, 768, 0xff000000); s.sub(0, 0, 1024, 768, 0xff334455);
        CHECK(s.input.FindInputTargetAt(1000, 500, t) && t.contentW == 1024, "live resize updates input");
        s.comp.zc().SetEnabled(3, false);
        CHECK(s.input.FindInputTargetAt(1000, 500, t) && t.contentW == 1024, "SHM fallback preserves logical space");
        s.fullscreen(false);
        CHECK(s.comp.GetZeroCopyLayerInfo(3, 1, 1024, 768, g) && !g.fullscreen && g.width == 1024,
              "windowed GPU placement remains unscaled");
    }
    { // A video child is not the fullscreen window's entire coordinate space.
        Scene s(800, 600); s.setWindow(2, 400, 300, 0xff334455); s.fullscreen(true);
        s.sub(100, 50, 200, 100, 0xff778899); s.comp.zc().SetEnabled(3, true);
        ZeroCopyLayerInfo g;
        CHECK(s.comp.GetZeroCopyLayerInfo(3, 1, 200, 100, g), "video geometry resolves");
        CHECK(g.x == 200 && g.y == 100 && g.width == 400 && g.height == 200,
              "video retains offset and extent within fullscreen parent");
        CHECK(s.take() && s.width == 800 && s.pixel(20, 20) == 0xff334455,
              "video does not black out or direct-present the rest of its parent");
    }
    { // Private Vulkan present can exist without any SHM child layer.
        Scene s(800, 600); s.setWindow(2, 400, 300, 0xff334455); s.fullscreen(true);
        s.childData.vpDstW = 400; s.childData.vpDstH = 300;
        s.comp.zc().SetEnabled(3, true);
        ZeroCopyLayerInfo g;
        CHECK(s.comp.GetZeroCopyLayerInfo(3, 1, 400, 300, g) && g.source == ZeroCopySource::ProtocolOnly && g.width == 800,
              "protocol-only Vulkan uses the same fit");
        CHECK(s.take() && s.width == 800 && s.pixel(20, 20) == 0xff000000,
              "protocol-only fullscreen GPU preserves the compositor base");
    }
    { // GPU viewport updates do not depend on another SHM readback.
        Scene s(800, 600); s.setWindow(2, 400, 300, 0xff334455); s.fullscreen(true);
        s.sub(0, 0, 400, 300, 0xff778899); s.comp.zc().SetEnabled(3, true);
        s.take();
        s.childData.vpDstW = 200; s.childData.vpDstH = 100;
        ZeroCopyLayerInfo g;
        CHECK(s.comp.GetZeroCopyLayerInfo(3, 1, 400, 300, g) && g.width == 400 && g.height == 200,
              "live viewport overrides the stale SHM layer dimensions");
        { auto lk = s.tm.Lock(); s.comp.MarkDesktopRootDirtyLocked(); }
        CHECK(s.take() && s.pixel(700, 500) == 0xff334455,
              "shrinking fullscreen GPU coverage restores CPU surroundings");
    }
    { // PC/system-window mode retains its own unscaled local coordinates.
        Scene s; s.policy.desktop = false; s.setWindow(2, 400, 300, 0xff334455);
        s.sub(20, 30, 200, 100, 0xff778899);
        ZeroCopyLayerInfo g;
        CHECK(s.comp.GetZeroCopyLayerInfo(3, 2, 200, 100, g) &&
              !g.desktopCoordinates && !g.fullscreen && g.x == 20 && g.y == 30 && g.width == 200,
              "PC windowed GL/video coordinates are unchanged");
        CHECK(!s.comp.GetZeroCopyLayerInfo(3, 1, 200, 100, g), "PC rejects a different renderer's child");
    }
    { // Menus over fullscreen GPU content use the same mapping for hit/occlusion.
        Scene s(800, 600); s.setWindow(2, 400, 300, 0xff334455); s.fullscreen(true);
        s.sub(0, 0, 400, 300, 0xff778899); s.comp.zc().SetEnabled(3, true);
        SurfaceData menuData;
        wl_resource menu{&menuData};
        DesktopCompositor::SubsurfaceLayer layer;
        layer.surface = &menu; layer.surfaceKey = 5; layer.parentToplevel = 2;
        layer.localX = layer.x = 100; layer.localY = layer.y = 50;
        layer.w = layer.h = layer.vpDstW = layer.vpDstH = 100;
        { auto lk = s.tm.Lock();
          s.comp.UpsertSubsurfaceLayer(std::move(layer), std::vector<uint8_t>(100 * 100 * 4, 255)); }
        ZeroCopyOccluderRect rects[8];
        CHECK(s.comp.GetZeroCopyOccluders(3, 1, rects, 8) == 1 &&
              rects[0].x == 200 && rects[0].y == 100 && rects[0].w == 200 && rects[0].h == 200,
              "fullscreen menu occlusion matches displayed rectangle");
        InputTarget t;
        CHECK(s.input.FindInputTargetAt(250, 150, t) && t.surface == &menu &&
              t.originX == 200 && t.originY == 100 && t.scale == 2,
              "fullscreen menu input matches GPU occlusion");
        s.setWindow(4, 400, 300, 0xffabcdef);
        { auto lk = s.tm.Lock(); s.tm.EnsureToplevelLocked(4).ApplyFullscreen(true); }
        ZeroCopyLayerInfo g;
        CHECK(!s.comp.GetZeroCopyLayerInfo(3, 1, 400, 300, g),
              "a cascaded non-selected fullscreen producer is not drawn over the active one");
        { auto lk = s.tm.Lock(); s.tm.BumpFsPriorityLocked(2); }
        CHECK(s.comp.GetZeroCopyLayerInfo(3, 1, 400, 300, g), "explicit raise restores the selected GPU surface");
    }
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
        PopupManager popups(tm, outputW, outputH);
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
        CHECK(first.isNew && !first.sizeChanged && first.winW == 800 && first.dispW == 400,
              "fullscreen popup reports output size while retaining content size");
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
    std::printf("compositor state: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
