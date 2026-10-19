#include "surface_probe.h"
#include "protocols/wayland-client.h"
#include "protocols/xdg-shell-client-protocol.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace steamohos {
namespace {
using Clock = std::chrono::steady_clock;
constexpr int Width = 480, Height = 270, Stride = Width * 4;
constexpr size_t BufferBytes = Stride * Height;
constexpr uint32_t Keys[] = {17, 17, 32, 32, 103, 103, 108, 108};

struct Client;
struct Buffer {
    wl_buffer* object = nullptr;
    Client* client = nullptr;
    bool busy = false;
};
struct Client {
    SurfaceResult result;
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    wl_keyboard* keyboard = nullptr;
    xdg_wm_base* wm = nullptr;
    wl_surface* surface = nullptr;
    xdg_surface* xdg = nullptr;
    xdg_toplevel* top = nullptr;
    wl_callback* callback = nullptr;
    Buffer buffers[2];
    void* mapping = MAP_FAILED;
    bool format = false;
    bool closed = false;
    bool keyMismatch = false;
    Clock::time_point nextFrame{};

    ~Client() {
        if (surface) {
            wl_surface_attach(surface, nullptr, 0, 0);
            wl_surface_commit(surface);
        }
        if (callback) wl_callback_destroy(callback);
        if (top) xdg_toplevel_destroy(top);
        if (xdg) xdg_surface_destroy(xdg);
        if (surface) wl_surface_destroy(surface);
        for (auto& b : buffers) if (b.object) wl_buffer_destroy(b.object);
        if (keyboard) wl_keyboard_release(keyboard);
        if (seat) wl_seat_release(seat);
        if (wm) xdg_wm_base_destroy(wm);
        if (shm) wl_shm_destroy(shm);
        if (compositor) wl_compositor_destroy(compositor);
        if (registry) wl_registry_destroy(registry);
        if (display) { wl_display_flush(display); wl_display_disconnect(display); }
        if (mapping != MAP_FAILED) munmap(mapping, BufferBytes * 2);
    }
};

// Returns a bounded, freshly connected socket; environment variables are unused.
int Connect(const char* path, Clock::time_point end) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    struct stat st{};
    if (!path || path[0] != '/' || std::strlen(path) >= sizeof(address.sun_path) ||
        lstat(path, &st) || !S_ISSOCK(st.st_mode) || st.st_uid != getuid()) return -1;
    std::strcpy(address.sun_path, path);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    if (!connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address))) return fd;
    if (errno == EINPROGRESS || errno == EAGAIN) {
        while (Clock::now() < end) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
            pollfd p{fd, POLLOUT, 0};
            const int rc = poll(&p, 1, static_cast<int>(std::max<int64_t>(1, ms)));
            if (rc < 0 && errno == EINTR) continue;
            int error = 0;
            socklen_t size = sizeof(error);
            if (rc > 0 && (p.revents & POLLOUT) && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) && !error)
                return fd;
            break;
        }
    }
    close(fd);
    return -1;
}

// prepare_read/cancel_read pairs prevent a blocking dispatch/roundtrip on failure.
bool Pump(Client& c, Clock::time_point end) {
    while (wl_display_prepare_read(c.display) != 0) {
        if (wl_display_dispatch_pending(c.display) < 0 || Clock::now() >= end) return false;
    }
    short events = POLLIN;
    if (wl_display_flush(c.display) < 0) {
        if (errno != EAGAIN) { wl_display_cancel_read(c.display); return false; }
        events |= POLLOUT;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
    if (ms <= 0) { wl_display_cancel_read(c.display); return true; }
    pollfd p{wl_display_get_fd(c.display), events, 0};
    // A short tick also drives the probe's intentionally rate-limited SHM frames.
    const int rc = poll(&p, 1, static_cast<int>(std::min<int64_t>(ms, 20)));
    if (rc > 0 && (p.revents & POLLIN)) {
        if (wl_display_read_events(c.display) < 0) return false;
    } else {
        wl_display_cancel_read(c.display);
        if ((rc < 0 && errno != EINTR) || (rc > 0 && (p.revents & (POLLHUP | POLLERR | POLLNVAL)))) return false;
    }
    return wl_display_dispatch_pending(c.display) >= 0;
}

void Keymap(void* data, wl_keyboard*, uint32_t format, int32_t fd, uint32_t size) {
    auto& c = *static_cast<Client*>(data);
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 1 && size <= 1024 * 1024) {
        // Consume the received SCM_RIGHTS descriptor, checking its final NUL.
        char last = 1;
        if (pread(fd, &last, 1, size - 1) == 1 && last == 0) c.result.keyboardKeymap = 1;
    }
    close(fd);
}
void KeyEnter(void* data, wl_keyboard*, uint32_t, wl_surface* surface, wl_array*) {
    auto& c = *static_cast<Client*>(data);
    if (surface == c.surface) c.result.keyboardEntered = 1;
}
void KeyLeave(void* data, wl_keyboard*, uint32_t, wl_surface*) {
    static_cast<Client*>(data)->result.keyboardEntered = 0;
}
void Key(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
    auto& c = *static_cast<Client*>(data);
    const auto index = c.result.keyEvents++;
    if (index >= 8 || key != Keys[index] || state != (index % 2 == 0 ? 1u : 0u)) c.keyMismatch = true;
    c.result.inputVerified = c.result.keyboardKeymap && c.result.keyboardEntered &&
        c.result.keyEvents == 8 && !c.keyMismatch;
}
void Modifiers(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void Repeat(void*, wl_keyboard*, int32_t, int32_t) {}
const wl_keyboard_listener KeyboardListener = {Keymap, KeyEnter, KeyLeave, Key, Modifiers, Repeat};
void Capabilities(void* data, wl_seat* seat, uint32_t caps) {
    auto& c = *static_cast<Client*>(data);
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !c.keyboard) {
        c.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(c.keyboard, &KeyboardListener, &c);
    }
}
void SeatName(void*, wl_seat*, const char*) {}
const wl_seat_listener SeatListener = {Capabilities, SeatName};
void Format(void* data, wl_shm*, uint32_t format) {
    if (format == WL_SHM_FORMAT_XRGB8888) static_cast<Client*>(data)->format = true;
}
const wl_shm_listener ShmListener = {Format};
void Ping(void*, xdg_wm_base* wm, uint32_t serial) { xdg_wm_base_pong(wm, serial); }
const xdg_wm_base_listener WmListener = {Ping};
void Global(void* data, wl_registry* registry, uint32_t id, const char* name, uint32_t version) {
    auto& c = *static_cast<Client*>(data);
    if (!std::strcmp(name, "wl_compositor") && version >= 4 && !c.compositor)
        c.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, id, &wl_compositor_interface, 4));
    else if (!std::strcmp(name, "wl_shm") && !c.shm) {
        c.shm = static_cast<wl_shm*>(wl_registry_bind(registry, id, &wl_shm_interface, 1));
        wl_shm_add_listener(c.shm, &ShmListener, &c);
    } else if (!std::strcmp(name, "xdg_wm_base") && !c.wm) {
        c.wm = static_cast<xdg_wm_base*>(wl_registry_bind(registry, id, &xdg_wm_base_interface, std::min(version, 3u)));
        xdg_wm_base_add_listener(c.wm, &WmListener, &c);
    } else if (!std::strcmp(name, "wl_seat") && version >= 5 && !c.seat) {
        c.seat = static_cast<wl_seat*>(wl_registry_bind(registry, id, &wl_seat_interface, 5));
        wl_seat_add_listener(c.seat, &SeatListener, &c);
    }
}
void Remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener RegistryListener = {Global, Remove};
void Configure(void* data, xdg_surface* surface, uint32_t serial) {
    auto& c = *static_cast<Client*>(data);
    xdg_surface_ack_configure(surface, serial);
    c.result.configured++;
}
const xdg_surface_listener SurfaceListener = {Configure};
void TopConfigure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}
void TopClose(void* data, xdg_toplevel*) { static_cast<Client*>(data)->closed = true; }
void Bounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void WmCapabilities(void*, xdg_toplevel*, wl_array*) {}
const xdg_toplevel_listener TopListener = {TopConfigure, TopClose, Bounds, WmCapabilities};
void Released(void* data, wl_buffer*) {
    auto& b = *static_cast<Buffer*>(data);
    b.busy = false;
    b.client->result.bufferReleases++;
}
const wl_buffer_listener BufferListener = {Released};
void Done(void* data, wl_callback* callback, uint32_t) {
    auto& c = *static_cast<Client*>(data);
    c.result.frameCallbacks++;
    c.callback = nullptr;
    wl_callback_destroy(callback);
}
const wl_callback_listener CallbackListener = {Done};

bool CreateBuffers(Client& c) {
    const int fd = memfd_create("steamohos-host-shm", MFD_CLOEXEC);
    if (fd < 0) return false;
    if (ftruncate(fd, BufferBytes * 2)) { close(fd); return false; }
    c.mapping = mmap(nullptr, BufferBytes * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (c.mapping == MAP_FAILED) { close(fd); return false; }
    auto* pool = wl_shm_create_pool(c.shm, fd, BufferBytes * 2);
    close(fd);
    if (!pool) return false;
    for (int i = 0; i < 2; ++i) {
        auto& b = c.buffers[i];
        b.client = &c;
        b.object = wl_shm_pool_create_buffer(pool, i * BufferBytes, Width, Height, Stride, WL_SHM_FORMAT_XRGB8888);
        wl_buffer_add_listener(b.object, &BufferListener, &b);
    }
    wl_shm_pool_destroy(pool);
    return true;
}
void Submit(Client& c) {
    if (c.callback || Clock::now() < c.nextFrame) return;
    const uint32_t frame = c.result.framesSubmitted;
    auto& b = c.buffers[frame % 2];
    if (b.busy) return; // The client never overwrites a compositor-owned buffer.
    auto* pixels = static_cast<uint32_t*>(c.mapping) + (frame % 2) * Width * Height;
    for (int y = 0; y < Height; ++y) for (int x = 0; x < Width; ++x)
        pixels[y * Width + x] = ((x / 30 + y / 30 + frame % 2) % 2) ? 0xff3080e0u : 0xff40c080u;
    b.busy = true;
    wl_surface_attach(c.surface, b.object, 0, 0);
    wl_surface_damage(c.surface, 0, 0, Width, Height);
    c.callback = wl_surface_frame(c.surface);
    wl_callback_add_listener(c.callback, &CallbackListener, &c);
    wl_surface_commit(c.surface);
    ++c.result.framesSubmitted;
    c.nextFrame = Clock::now() + std::chrono::milliseconds(100);
}
}

SurfaceResult ProbeSurface(const char* path, uint32_t timeoutMs, SurfaceReady notify, void* context) {
    struct NotifyReset {
        SurfaceReady notify;
        void* context;
        ~NotifyReset() { if (notify) notify(0, context); }
    } reset{notify, context};
    Client c;
    if (timeoutMs < 200 || timeoutMs > 10000) { c.result.stage = "invalid_timeout"; return c.result; }
    const auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
    const int fd = Connect(path, end);
    if (fd < 0) return c.result;
    c.display = wl_display_connect_to_fd(fd); // libwayland owns fd, including failure.
    if (!c.display) return c.result;
    c.result.stage = "registry";
    c.registry = wl_display_get_registry(c.display);
    wl_registry_add_listener(c.registry, &RegistryListener, &c);
    while (Clock::now() < end && !(c.compositor && c.shm && c.wm && c.seat && c.format && c.keyboard))
        if (!Pump(c, end)) { c.result.displayError = wl_display_get_error(c.display); return c.result; }
    if (!(c.compositor && c.shm && c.wm && c.seat && c.format && c.keyboard)) return c.result;
    c.result.stage = "shm_allocation";
    if (!CreateBuffers(c)) return c.result;
    c.surface = wl_compositor_create_surface(c.compositor);
    c.result.surfaceId = wl_proxy_get_id(reinterpret_cast<wl_proxy*>(c.surface));
    c.xdg = xdg_wm_base_get_xdg_surface(c.wm, c.surface);
    xdg_surface_add_listener(c.xdg, &SurfaceListener, &c);
    c.top = xdg_surface_get_toplevel(c.xdg);
    xdg_toplevel_add_listener(c.top, &TopListener, &c);
    xdg_toplevel_set_title(c.top, "Linux ARM64 host - Wayland SHM probe");
    xdg_toplevel_set_app_id(c.top, "org.vintagepomelo.steamohos.host-probe");
    xdg_surface_set_window_geometry(c.xdg, 0, 0, Width, Height);
    wl_surface_commit(c.surface); // Wait for configure before the first attach.
    c.result.stage = "configure";
    bool notified = false;
    while (Clock::now() < end && !c.closed) {
        if (!Pump(c, end)) { c.result.displayError = wl_display_get_error(c.display); return c.result; }
        if (!c.result.configured) continue;
        // Reserve a final tick for the compositor to release the last buffer.
        if (Clock::now() + std::chrono::milliseconds(100) < end) Submit(c);
        if (!notified && c.result.frameCallbacks >= 2 && c.result.keyboardKeymap) {
            if (notify) notify(c.result.surfaceId, context);
            notified = true;
        }
        c.result.stage = c.result.framesSubmitted >= 4 ? "keyboard_roundtrip" : "shm_frames";
    }
    c.result.windowClosed = c.closed;
    c.result.displayError = wl_display_get_error(c.display);
    if (!c.result.displayError && c.result.configured && c.result.framesSubmitted >= 4 &&
        c.result.bufferReleases == c.result.framesSubmitted &&
        c.result.frameCallbacks == c.result.framesSubmitted && c.result.inputVerified) {
        c.result.status = 0;
        c.result.stage = "shm_keyboard_verified";
    }
    return c.result;
}
}
