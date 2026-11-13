#include "linux_steam/surface_probe.h"
#include "input/keymap_xkb.h"
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "protocols/xdg-shell-server-protocol.h"
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <dirent.h>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

namespace {
struct Server;
struct Surface {
    Server* server;
    wl_resource* resource = nullptr;
    wl_resource* xdg = nullptr;
    wl_resource* top = nullptr;
    wl_resource* buffer = nullptr;
    std::vector<wl_resource*> callbacks;
    bool sentConfigure = false;
    bool acked = false;
};
struct Server {
    wl_display* display = nullptr;
    Surface* surface = nullptr;
    wl_resource* keyboard = nullptr;
    std::atomic<uint32_t> ready{0};
    uint32_t scenario = 0, frames = 0;
    uint32_t lastPixel = 0;
    bool pixelsAlternate = true, validOrder = true, inputSent = false;
};
void Destroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
void Attach(wl_client*, wl_resource* resource, wl_resource* buffer, int32_t, int32_t) {
    static_cast<Surface*>(wl_resource_get_user_data(resource))->buffer = buffer;
}
void Damage(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
void CallbackDestroyed(wl_resource* resource) {
    auto* surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
    auto& callbacks = surface->callbacks;
    callbacks.erase(std::remove(callbacks.begin(), callbacks.end(), resource), callbacks.end());
}
void Frame(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
    auto* callback = wl_resource_create(client, &wl_callback_interface, 1, id);
    wl_resource_set_implementation(callback, nullptr, surface, CallbackDestroyed);
    surface->callbacks.push_back(callback);
}
void Commit(wl_client*, wl_resource* resource) {
    auto& surface = *static_cast<Surface*>(wl_resource_get_user_data(resource));
    auto& s = *surface.server;
    if (!surface.sentConfigure && surface.xdg && surface.top && s.scenario != 3) {
        wl_array states; wl_array_init(&states);
        xdg_toplevel_send_configure(surface.top, 480, 270, &states);
        xdg_surface_send_configure(surface.xdg, 1);
        wl_array_release(&states);
        surface.sentConfigure = true;
    }
    if (!surface.buffer) return;
    s.validOrder &= surface.acked;
    auto* shm = wl_shm_buffer_get(surface.buffer);
    s.validOrder &= shm && wl_shm_buffer_get_width(shm) == 480 && wl_shm_buffer_get_height(shm) == 270;
    if (shm) {
        wl_shm_buffer_begin_access(shm);
        const auto* pixels = static_cast<const uint32_t*>(wl_shm_buffer_get_data(shm));
        const uint32_t pixel = pixels[0];
        s.pixelsAlternate &= (!s.frames || pixel != s.lastPixel) &&
            (pixel == 0xff3080e0u || pixel == 0xff40c080u);
        s.lastPixel = pixel;
        wl_shm_buffer_end_access(shm);
    }
    ++s.frames;
    if (s.scenario != 1) {
        wl_buffer_send_release(surface.buffer);
        while (!surface.callbacks.empty()) {
            auto* callback = surface.callbacks.front();
            wl_callback_send_done(callback, s.frames * 100);
            wl_resource_destroy(callback);
        }
    }
    surface.buffer = nullptr;
}
const struct wl_surface_interface SurfaceRequests = [] {
    struct wl_surface_interface i{};
    i.destroy = Destroy; i.attach = Attach; i.damage = Damage; i.frame = Frame; i.commit = Commit;
    return i;
}();
void SurfaceDestroyed(wl_resource* resource) {
    auto* surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
    while (!surface->callbacks.empty()) wl_resource_destroy(surface->callbacks.front());
    surface->server->surface = nullptr;
    delete surface;
}
void CreateSurface(wl_client* client, wl_resource* resource, uint32_t id) {
    auto& s = *static_cast<Server*>(wl_resource_get_user_data(resource));
    auto* surface = new Surface{};
    surface->server = &s;
    surface->resource = wl_resource_create(client, &wl_surface_interface, 4, id);
    wl_resource_set_implementation(surface->resource, &SurfaceRequests, surface, SurfaceDestroyed);
    s.surface = surface;
}
const struct wl_compositor_interface CompositorRequests = [] {
    struct wl_compositor_interface i{}; i.create_surface = CreateSurface; return i;
}();
void BindCompositor(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* resource = wl_resource_create(client, &wl_compositor_interface, version, id);
    wl_resource_set_implementation(resource, &CompositorRequests, data, nullptr);
}
void Title(wl_client*, wl_resource*, const char*) {}
const struct xdg_toplevel_interface TopRequests = [] {
    struct xdg_toplevel_interface i{}; i.destroy = Destroy; i.set_title = Title; i.set_app_id = Title; return i;
}();
void GetTop(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
    surface->top = wl_resource_create(client, &xdg_toplevel_interface, 3, id);
    wl_resource_set_implementation(surface->top, &TopRequests, surface, nullptr);
}
void Geometry(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
void Ack(wl_client*, wl_resource* resource, uint32_t serial) {
    auto& surface = *static_cast<Surface*>(wl_resource_get_user_data(resource));
    surface.acked = surface.sentConfigure && serial == 1;
}
const struct xdg_surface_interface XdgRequests = [] {
    struct xdg_surface_interface i{};
    i.destroy = Destroy; i.get_toplevel = GetTop; i.set_window_geometry = Geometry; i.ack_configure = Ack; return i;
}();
void GetXdg(wl_client* client, wl_resource*, uint32_t id, wl_resource* wlSurface) {
    auto* surface = static_cast<Surface*>(wl_resource_get_user_data(wlSurface));
    surface->xdg = wl_resource_create(client, &xdg_surface_interface, 3, id);
    wl_resource_set_implementation(surface->xdg, &XdgRequests, surface, nullptr);
}
const struct xdg_wm_base_interface WmRequests = [] {
    struct xdg_wm_base_interface i{}; i.destroy = Destroy; i.get_xdg_surface = GetXdg; return i;
}();
void BindWm(wl_client* client, void*, uint32_t version, uint32_t id) {
    auto* resource = wl_resource_create(client, &xdg_wm_base_interface, version, id);
    wl_resource_set_implementation(resource, &WmRequests, nullptr, nullptr);
}
const struct wl_keyboard_interface KeyboardRequests = {Destroy};
void Keyboard(wl_client* client, wl_resource* resource, uint32_t id) {
    auto& s = *static_cast<Server*>(wl_resource_get_user_data(resource));
    s.keyboard = wl_resource_create(client, &wl_keyboard_interface, 5, id);
    wl_resource_set_implementation(s.keyboard, &KeyboardRequests, nullptr, nullptr);
    const int fd = memfd_create("probe-keymap-fixture", MFD_CLOEXEC);
    const uint32_t size = _tmp_keymap_xkb_len + (s.scenario == 5 ? 0 : 1);
    if (fd >= 0) {
        if (ftruncate(fd, size) == 0 &&
            pwrite(fd, _tmp_keymap_xkb, _tmp_keymap_xkb_len, 0) == _tmp_keymap_xkb_len)
            wl_keyboard_send_keymap(s.keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
        close(fd);
    }
}
const struct wl_seat_interface SeatRequests = [] {
    struct wl_seat_interface i{}; i.get_keyboard = Keyboard; i.release = Destroy; return i;
}();
void BindSeat(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* resource = wl_resource_create(client, &wl_seat_interface, version, id);
    wl_resource_set_implementation(resource, &SeatRequests, data, nullptr);
    wl_seat_send_capabilities(resource, WL_SEAT_CAPABILITY_KEYBOARD);
}
void Ready(uint32_t id, void* data) { static_cast<Server*>(data)->ready.store(id); }
bool Trial(uint32_t scenario) {
    char directory[] = "/tmp/vpp-linux-steam-host-XXXXXX";
    if (!mkdtemp(directory)) return false;
    setenv("XDG_RUNTIME_DIR", directory, 1);
    Server s; s.scenario = scenario; s.display = wl_display_create();
    if (!s.display) return false;
    bool setup = wl_display_add_socket(s.display, "host-test") == 0 && wl_display_init_shm(s.display) == 0 &&
        wl_global_create(s.display, &wl_compositor_interface, 4, &s, BindCompositor) &&
        wl_global_create(s.display, &xdg_wm_base_interface, 3, &s, BindWm);
    if (scenario != 2) setup &= wl_global_create(s.display, &wl_seat_interface, 5, &s, BindSeat) != nullptr;
    std::atomic<bool> stop{false};
    std::thread server([&] {
        while (!stop.load()) {
            wl_event_loop_dispatch(wl_display_get_event_loop(s.display), 5);
            if (!s.inputSent && s.ready.load() && s.keyboard && s.surface) {
                wl_array keys; wl_array_init(&keys);
                wl_keyboard_send_enter(s.keyboard, 2, s.surface->resource, &keys);
                wl_array_release(&keys);
                const uint32_t sequence[] = {17,17,32,32,103,103,108,108};
                for (uint32_t i = 0; i < 8; ++i)
                    wl_keyboard_send_key(s.keyboard, 3+i, 200+i, sequence[i] + (scenario == 4 ? 1 : 0), i%2 == 0 ? 1 : 0);
                s.inputSent = true;
            }
            wl_display_flush_clients(s.display);
        }
    });
    const std::string path = std::string(directory) + "/host-test";
    const auto r = steamohos::ProbeSurface(path.c_str(), 700, Ready, &s);
    stop.store(true); server.join();
    wl_display_destroy_clients(s.display); wl_display_destroy(s.display); rmdir(directory);
    const bool ok = setup && (scenario == 0 ?
        r.status == 0 && r.framesSubmitted >= 4 && r.inputVerified && s.validOrder && s.pixelsAlternate :
        r.status != 0 && (scenario != 1 || r.framesSubmitted == 1) &&
        (scenario != 5 || (!r.keyboardKeymap && !r.inputVerified)));
    std::cout << "scenario=" << scenario << " status=" << r.status << " stage=" << r.stage <<
        " frames=" << r.framesSubmitted << " keys=" << r.keyEvents << " pass=" << ok << '\n';
    return ok;
}
}
int main() {
    auto descriptors = [] {
        size_t count = 0;
        auto* directory = opendir("/proc/self/fd");
        if (!directory) return size_t(0);
        while (readdir(directory)) ++count;
        closedir(directory);
        return count;
    };
    const auto before = descriptors();
    bool ok = true;
    for (uint32_t i = 0; i < 6; ++i) ok = Trial(i) && ok;
    ok &= steamohos::ProbeSurface("relative", 500).status != 0;
    ok &= steamohos::ProbeSurface("/not-present", 199).status != 0;
    const auto after = descriptors();
    ok &= before != 0 && before == after;
    std::cout << "fd_count_before=" << before << " fd_count_after=" << after << '\n';
    return ok ? 0 : 1;
}
