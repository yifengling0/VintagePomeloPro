// SPDX-License-Identifier: LGPL-2.1-or-later
#include "runtime_napi.h"
#include "runtime_session.h"
#include "process_backend.h"
#include "harmony_frame_generation.h"
#include "SessionProtocol.h"
#include "BrokerTransport.h"
#include <dlfcn.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/stat.h>

namespace winehua::gamescope {
namespace {
struct FrameGenerationProbe {
    napi_async_work work = nullptr;
    napi_deferred promise = nullptr;
    HarmonyFrameGenerationCapability capability;
};
void ExecuteFrameGenerationProbe(napi_env, void* context) {
    static_cast<FrameGenerationProbe*>(context)->capability = ProbeHarmonyFrameGeneration();
}
void CompleteFrameGenerationProbe(napi_env env, napi_status status, void* context) {
    auto* probe = static_cast<FrameGenerationProbe*>(context);
    if (status != napi_ok) {
        napi_value message, error;
        napi_create_string_utf8(env, "Frame generation capability query cancelled", NAPI_AUTO_LENGTH, &message);
        napi_create_error(env, nullptr, message, &error);
        napi_reject_deferred(env, probe->promise, error);
    } else {
        const auto& c = probe->capability;
        napi_value object, value;
        napi_create_object(env, &object);
        for (auto entry : {std::pair{"libraryLoaded", c.libraryLoaded},
                           {"supportQueryAvailable", c.supportQueryAvailable},
                           {"vulkanEntryPointsAvailable", c.vulkanEntryPointsAvailable},
                           {"renderQualified", false}}) {
            napi_get_boolean(env, entry.second, &value);
            napi_set_named_property(env, object, entry.first, value);
        }
        for (auto entry : {std::pair{"deviceSupport", c.deviceSupport},
                           {"stage", c.stage}, {"detail", c.detail}}) {
            napi_create_string_utf8(env, entry.second.c_str(), NAPI_AUTO_LENGTH, &value);
            napi_set_named_property(env, object, entry.first, value);
        }
        napi_resolve_deferred(env, probe->promise, object);
    }
    napi_delete_async_work(env, probe->work);
    delete probe;
}
bool Installed() {
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(&PrepareRuntime), &info) || !info.dli_fname) return false;
    const std::string path(info.dli_fname);
    const auto slash = path.rfind('/');
    if (slash == std::string::npos) return false;
    const auto directory = path.substr(0, slash+1);
    return access((directory+"libgamescope_core.so").c_str(), R_OK) == 0 &&
        access((directory+"libgamescope_child.so").c_str(), R_OK) == 0;
}
struct Probe {
    napi_async_work work = nullptr;
    napi_deferred promise = nullptr;
    bool loaded = false;
    int launchCode = -1;
    int32_t pid = -1;
    std::string stage = "not-installed", detail, route;
};
void Execute(napi_env, void* context) {
    auto& p = *static_cast<Probe*>(context);
    if (!Installed()) return;
    // A successful clean-fork loader probe would still not qualify a phone
    // NativeWindow. Do not launch a diagnostic compositor on this route.
    if (!StandaloneOutputSupported()) { p.stage = "in-process-presentation-required"; return; }
    if (!ProcessBackendAvailable()) { p.stage = "process-unavailable"; return; }
    int endpoints[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, endpoints)) { p.stage = "socketpair"; return; }
    ::gamescope::ohos::SessionConfig config;
    if (send(endpoints[0], &config, sizeof(config), MSG_NOSIGNAL) != sizeof(config)) {
        close(endpoints[0]); close(endpoints[1]); p.stage = "send-config"; return;
    }
    NativeChildProcess_Fd node{};
    node.fdName = const_cast<char*>("gs_session"); node.fd = endpoints[1];
    NativeChildProcess_Args args{};
    args.entryParams = const_cast<char*>("gamescope-v1"); args.fdList.head = &node;
    p.launchCode = StartRuntimeChild("libgamescope_child.so:Main", args, &p.pid);
    close(endpoints[1]);
    if (p.launchCode != 0 || p.pid <= 0) p.stage = "child-launch";
    else if (!::gamescope::ohos::WaitReadable(endpoints[0], 8000)) p.stage = "child-loader-timeout";
    else {
        ::gamescope::ohos::SessionEvent event;
        const auto count = recv(endpoints[0], &event, sizeof(event), MSG_DONTWAIT | MSG_TRUNC);
        if (count == sizeof(event) && event.magic == ::gamescope::ohos::SessionMagic &&
            event.version == 1 && event.size == sizeof(event) && event.pid == p.pid &&
            memchr(event.detail, 0, sizeof(event.detail))) {
            p.loaded = event.stage == ::gamescope::ohos::SessionStage::Loaded && event.error == 0;
            p.stage = p.loaded ? "core-loaded" : "core-load-failed";
            p.detail = event.detail;
        } else p.stage = "child-loader-protocol";
    }
    close(endpoints[0]);
}
void Complete(napi_env env, napi_status status, void* context) {
    auto* p = static_cast<Probe*>(context);
    napi_value value, field;
    napi_create_object(env, &value);
    napi_get_boolean(env, status == napi_ok && p->loaded, &field);
    napi_set_named_property(env, value, "loaded", field);
    // This proves loader identity only. No game backend is qualified here.
    napi_get_boolean(env, false, &field); napi_set_named_property(env, value, "renderQualified", field);
    napi_get_boolean(env, StandaloneOutputSupported(), &field);
    napi_set_named_property(env, value, "standaloneOutputSupported", field);
    for (auto entry : {std::pair{"stage", p->stage}, {"detail", p->detail}, {"processBackend", p->route}}) {
        napi_create_string_utf8(env, entry.second.c_str(), NAPI_AUTO_LENGTH, &field);
        napi_set_named_property(env, value, entry.first, field);
    }
    napi_create_int32(env, p->launchCode, &field); napi_set_named_property(env, value, "launchCode", field);
    napi_create_int32(env, p->pid, &field); napi_set_named_property(env, value, "pid", field);
    napi_resolve_deferred(env, p->promise, value);
    napi_delete_async_work(env, p->work);
    delete p;
}
}
napi_value ProbeHarmonyFrameGenerationNapi(napi_env env, napi_callback_info) {
    auto* probe = new FrameGenerationProbe;
    napi_value promise, name;
    napi_create_string_utf8(env, "HarmonyFrameGenerationCapability", NAPI_AUTO_LENGTH, &name);
    if (napi_create_promise(env, &probe->promise, &promise) != napi_ok) {
        delete probe;
        napi_throw_error(env, nullptr, "Cannot create frame generation capability promise");
        return nullptr;
    }
    if (napi_create_async_work(env, nullptr, name, ExecuteFrameGenerationProbe,
            CompleteFrameGenerationProbe, probe, &probe->work) != napi_ok ||
        napi_queue_async_work(env, probe->work) != napi_ok) {
        if (probe->work) napi_delete_async_work(env, probe->work);
        napi_value message, error;
        napi_create_string_utf8(env, "Cannot schedule frame generation capability query", NAPI_AUTO_LENGTH, &message);
        napi_create_error(env, nullptr, message, &error);
        napi_reject_deferred(env, probe->promise, error);
        delete probe;
    }
    return promise;
}
napi_value PrepareRuntime(napi_env env, napi_callback_info) {
    const int status = Installed() ? PrepareProcessBackend() : NCP_ERR_NOT_SUPPORTED;
    napi_value result;
    napi_create_int32(env, status, &result);
    return result;
}
napi_value ProbeRuntime(napi_env env, napi_callback_info) {
    auto* p = new Probe;
    p->route = ProcessBackendName();
    napi_value promise, label;
    napi_create_promise(env, &p->promise, &promise);
    napi_create_string_utf8(env, "Gamescope loader qualification", NAPI_AUTO_LENGTH, &label);
    if (napi_create_async_work(env, nullptr, label, Execute, Complete, p, &p->work) != napi_ok ||
        napi_queue_async_work(env, p->work) != napi_ok) {
        if (p->work) napi_delete_async_work(env, p->work);
        delete p;
        napi_throw_error(env, nullptr, "Cannot queue Gamescope loader check"); return nullptr;
    }
    return promise;
}
namespace {
struct StartWork {
    napi_async_work work = nullptr;
    napi_deferred promise = nullptr;
    ::gamescope::ohos::SessionConfig config;
    int result = ENOENT;
};
void StartExecute(napi_env, void* context) {
    auto& job = *static_cast<StartWork*>(context);
    if (!Installed()) return;
    if (mkdir(job.config.runtimeDirectory, 0700) != 0 && errno != EEXIST) { job.result = errno; return; }
    job.result = StartSession(job.config);
}
void StartComplete(napi_env env, napi_status status, void* context) {
    auto* job = static_cast<StartWork*>(context);
    napi_value result;
    napi_create_int32(env, status == napi_ok ? job->result : ECANCELED, &result);
    napi_resolve_deferred(env, job->promise, result);
    napi_delete_async_work(env, job->work); delete job;
}
}
napi_value StartRuntimeSession(napi_env env, napi_callback_info info) {
    napi_value options;
    size_t count = 1;
    napi_get_cb_info(env, info, &count, &options, nullptr, nullptr);
    auto* job = new StartWork;
    bool valid = count == 1;
    napi_value value;
    bool lossless = false;
    valid = valid && napi_get_named_property(env, options, "surfaceId", &value) == napi_ok &&
        napi_get_value_bigint_uint64(env, value, &job->config.surfaceId, &lossless) == napi_ok && lossless;
    auto number = [&](const char* key, uint32_t& output) {
        valid = valid && napi_get_named_property(env, options, key, &value) == napi_ok &&
            napi_get_value_uint32(env, value, &output) == napi_ok;
    };
    number("renderWidth", job->config.renderWidth); number("renderHeight", job->config.renderHeight);
    number("outputWidth", job->config.outputWidth); number("outputHeight", job->config.outputHeight);
    number("filter", job->config.filter); number("fpsLimit", job->config.fpsLimit); number("sharpness", job->config.sharpness);
    job->config.mode = ::gamescope::ohos::SessionMode::Compositor;
    strcpy(job->config.runtimeDirectory, "/data/storage/el2/base/files/.gamescope-rt");
    strcpy(job->config.logPath, "/data/storage/el2/base/files/gamescope.log");
    if (!valid || !::gamescope::ohos::ValidateSessionConfig(job->config)) {
        delete job; napi_throw_type_error(env, nullptr, "Invalid Gamescope session configuration"); return nullptr;
    }
    napi_value promise, label;
    napi_create_promise(env, &job->promise, &promise);
    napi_create_string_utf8(env, "Start Gamescope", NAPI_AUTO_LENGTH, &label);
    if (napi_create_async_work(env, nullptr, label, StartExecute, StartComplete, job, &job->work) != napi_ok ||
        napi_queue_async_work(env, job->work) != napi_ok) {
        if (job->work) napi_delete_async_work(env, job->work);
        delete job; napi_throw_error(env, nullptr, "Cannot queue Gamescope session"); return nullptr;
    }
    return promise;
}
napi_value StopRuntimeSession(napi_env env, napi_callback_info) {
    napi_value result; napi_get_boolean(env, RequestSessionStop(), &result); return result;
}
napi_value StartDiagnosticClient(napi_env env, napi_callback_info) {
    const auto status = SessionStatus();
    int error = ENOTCONN;
    // The probe is staged only in the opt-in debug package. Use the active
    // display, never assume :0 or accidentally attach this source to Wine.
    if (status.active && status.displayReady && status.display.compare(0, 4, "x11=") == 0) {
        auto end = status.display.find(';');
        std::string display = status.display.substr(4, end == std::string::npos ? end : end-4);
        NativeChildProcess_Args args{}; args.entryParams = display.data();
        int32_t pid = -1;
        error = StartRuntimeChild("libgamescope_pixel_probe.so:Main", args, &pid);
    }
    napi_value result; napi_create_int32(env, error, &result); return result;
}
napi_value SetRuntimeOutputState(napi_env env, napi_callback_info info) {
    napi_value args[3]; size_t count = 3; uint32_t width = 0, height = 0; bool visible = false;
    napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
    const bool valid = count == 3 && napi_get_value_uint32(env, args[0], &width) == napi_ok &&
        napi_get_value_uint32(env, args[1], &height) == napi_ok && napi_get_value_bool(env, args[2], &visible) == napi_ok;
    napi_value result; napi_get_boolean(env, valid && UpdateSessionOutput(width, height, visible), &result); return result;
}
napi_value GetRuntimeSessionStatus(napi_env env, napi_callback_info) {
    const auto status = SessionStatus(); napi_value result, value;
    napi_create_object(env, &result);
    for (auto entry : {std::pair{"active", status.active}, {"outputReady", status.outputReady},
                      {"displayReady", status.displayReady}, {"presented", status.presented}}) {
        napi_get_boolean(env, entry.second, &value); napi_set_named_property(env, result, entry.first, value);
    }
    for (auto entry : {std::pair{"stage", status.stage}, {"detail", status.detail}, {"display", status.display}}) {
        napi_create_string_utf8(env, entry.second.c_str(), NAPI_AUTO_LENGTH, &value);
        napi_set_named_property(env, result, entry.first, value);
    }
    napi_create_int32(env, status.pid, &value); napi_set_named_property(env, result, "pid", value);
    napi_create_int32(env, status.error, &value); napi_set_named_property(env, result, "error", value);
    return result;
}
}
