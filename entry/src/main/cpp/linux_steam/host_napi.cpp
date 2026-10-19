#include "host_napi.h"
#include "surface_probe.h"
#include "host_v1/winehua_host.h"
#include "compositor/wayland_server.h"
#include "compositor/frame/surface_data.h"
#include "input/input_manager.h"
#include <atomic>
#include <memory>
#include <string>
#include <unistd.h>

extern std::string gSockPath; // Captured only on the JS thread that owns it.

namespace steamohos {
namespace {
std::atomic<bool> Busy{false};
std::atomic<uint32_t> ReadySurface{0};
std::atomic<bool> Injected{false};
struct Work {
    std::string path;
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    SteamOhosGraphicsProbeV1 discovery{};
    SurfaceResult surface;
};
void Ready(uint32_t surfaceId, void*) { ReadySurface.store(surfaceId); }
void Execute(napi_env, void* data) {
    auto& w = *static_cast<Work*>(data);
    w.discovery.struct_size = sizeof(w.discovery);
    if (steamohos_winehua_probe_graphics_v1(w.path.c_str(), &w.discovery, 1000) == 0)
        w.surface = ProbeSurface(w.path.c_str(), 6000, Ready);
    else w.surface.stage = "discovery";
    ReadySurface.store(0);
}
bool Number(napi_env env, napi_value object, const char* name, uint32_t value) {
    napi_value n;
    return napi_create_uint32(env, value, &n) == napi_ok && napi_set_named_property(env, object, name, n) == napi_ok;
}
bool Boolean(napi_env env, napi_value object, const char* name, bool value) {
    napi_value n;
    return napi_get_boolean(env, value, &n) == napi_ok && napi_set_named_property(env, object, name, n) == napi_ok;
}
bool Text(napi_env env, napi_value object, const char* name, const char* value) {
    napi_value n;
    return napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &n) == napi_ok &&
        napi_set_named_property(env, object, name, n) == napi_ok;
}
void Reject(napi_env env, napi_deferred deferred, const char* message) {
    napi_value text, error;
    if (napi_create_string_utf8(env, message, NAPI_AUTO_LENGTH, &text) == napi_ok &&
        napi_create_error(env, nullptr, text, &error) == napi_ok) napi_reject_deferred(env, deferred, error);
}
void Complete(napi_env env, napi_status status, void* data) {
    std::unique_ptr<Work> w(static_cast<Work*>(data));
    Busy.store(false);
    ReadySurface.store(0);
    if (!env) return;
    const auto& d = w->discovery;
    const auto& s = w->surface;
    napi_value object, versions;
    bool ok = status == napi_ok && napi_create_object(env, &object) == napi_ok &&
        Text(env, object, "status", d.status == 0 && s.status == 0 ? "host_protocol_verified" : "host_probe_failed") &&
        Text(env, object, "stage", s.stage) && Number(env, object, "schemaVersion", 1) &&
        Number(env, object, "discoveryStatus", d.status) && Number(env, object, "surfaceStatus", s.status) &&
        Number(env, object, "globalsSeen", d.globals_seen) && Boolean(env, object, "syncReceived", d.sync_received) &&
        Boolean(env, object, "windowConfigured", s.configured != 0) &&
        Number(env, object, "framesSubmitted", s.framesSubmitted) &&
        Number(env, object, "bufferReleases", s.bufferReleases) &&
        Number(env, object, "frameCallbacks", s.frameCallbacks) &&
        Number(env, object, "displayError", s.displayError) &&
        Boolean(env, object, "keyboardKeymap", s.keyboardKeymap) &&
        Boolean(env, object, "keyboardEntered", s.keyboardEntered) &&
        Number(env, object, "keyEvents", s.keyEvents) && Boolean(env, object, "inputVerified", s.inputVerified) &&
        Boolean(env, object, "inputInjected", Injected.load()) &&
        Boolean(env, object, "framePresented", false) &&
        Boolean(env, object, "gnuExecutionVerified", false) &&
        Boolean(env, object, "steamClientStarted", false) &&
        napi_create_array_with_length(env, STEAMOHOS_WL_GLOBAL_COUNT, &versions) == napi_ok;
    for (uint32_t i = 0; ok && i < STEAMOHOS_WL_GLOBAL_COUNT; ++i) {
        napi_value n;
        ok = napi_create_uint32(env, d.versions[i], &n) == napi_ok && napi_set_element(env, versions, i, n) == napi_ok;
    }
    if (ok) ok = napi_set_named_property(env, object, "versions", versions) == napi_ok;
    if (ok) napi_resolve_deferred(env, w->deferred, object);
    else Reject(env, w->deferred, "Linux Steam host probe could not complete");
    napi_delete_async_work(env, w->work);
}
}

napi_value ProbeHost(napi_env env, napi_callback_info) {
    if (Busy.exchange(true)) { napi_throw_error(env, nullptr, "Linux Steam host probe is already running"); return nullptr; }
    try {
        auto w = std::make_unique<Work>();
        w->path = gSockPath;
        ReadySurface.store(0);
        Injected.store(false);
        napi_value promise, name;
        if (napi_create_promise(env, &w->deferred, &promise) != napi_ok) { Busy.store(false); return nullptr; }
        if (napi_create_string_utf8(env, "LinuxSteamHostProbe", NAPI_AUTO_LENGTH, &name) != napi_ok ||
            napi_create_async_work(env, nullptr, name, Execute, Complete, w.get(), &w->work) != napi_ok) {
            Busy.store(false);
            Reject(env, w->deferred, "Linux Steam host worker could not be created");
            return promise;
        }
        if (napi_queue_async_work(env, w->work) != napi_ok) {
            Busy.store(false);
            napi_delete_async_work(env, w->work);
            Reject(env, w->deferred, "Linux Steam host worker could not be queued");
            return promise;
        }
        w.release();
        return promise;
    } catch (...) {
        Busy.store(false);
        napi_throw_error(env, nullptr, "Linux Steam host worker allocation failed");
        return nullptr;
    }
}

napi_value InjectProbeKeys(napi_env env, napi_callback_info) {
    bool injected = false;
    const uint32_t surfaceId = ReadySurface.load();
    if (Busy.load() && surfaceId && !Injected.load()) {
        auto& manager = WaylandServer::GetInstance()->GetToplevelManager();
        uint32_t top = 0;
        {
            auto lock = manager.Lock();
            const uint64_t key = (uint64_t(getpid()) << 32) | surfaceId;
            auto* resource = manager.FindSurfaceResource(key);
            auto* data = manager.FindRegisteredSurfaceDataLocked(resource);
            if (data) top = data->toplevelId;
        }
        // Desktop input is routed by actual keyboard focus. A toplevel ID
        // alone does not direct SendKeyEvent to that client.
        if (top && InputManager::GetInstance()->GetKeyboardFocusedToplevel() == top &&
            !Injected.exchange(true)) {
            // Go through the same queued input path as ArkUI, targeting only
            // this probe's exact surface. This is synthetic keyboard evidence.
            for (int key : {17, 32, 103, 108}) {
                InputManager::GetInstance()->SendKeyEvent(top, key, true);
                InputManager::GetInstance()->SendKeyEvent(top, key, false);
            }
            injected = true;
        }
    }
    napi_value result;
    napi_get_boolean(env, injected, &result);
    return result;
}
}
