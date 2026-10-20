#include "host_napi.h"
#include "launch_broker.h"
#include "audio_bootstrap_relay.h"
#include "phone_adapter/phone_adapter.h"
#include "proc/wine_process.h"
#include "input/controller/controller_hub.h"
#include "input/controller/gamepad_bridge.h"
#include "input/game_controller_bridge.h"
#include "graphics/graphics_broker.h"
#include "wine/wine_env.h"
#include <AbilityKit/native_child_process.h>
#include <hilog/log.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

extern std::string gSockPath; // Captured on the JS thread; no environment mutation.

namespace steamohos {
namespace {
std::atomic<bool> GnuBusy{false};
std::mutex ExitsMutex;
std::map<int32_t, int32_t> Exits;
void Exited(int32_t pid, int32_t signal) {
    std::lock_guard<std::mutex> lock(ExitsMutex);
    if (Exits.size() >= 64) Exits.erase(Exits.begin());
    Exits[pid] = signal;
}
struct GnuWork {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::string json;
    std::string parameters = "native-gnu-v1";
    bool forkBackend = false;
    bool productSession = false;
};
uint64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void ExecuteGnu(napi_env, void* data) {
    auto& work = *static_cast<GnuWork*>(data);
    work.json = "{\"schemaVersion\":1,\"status\":\"ncp_setup_failed\",\"gnuExecutionVerified\":false}";
    if (!work.forkBackend) {
        static const int registered = OH_Ability_RegisterNativeChildProcessExitCallback(Exited);
        if (registered != NCP_NO_ERROR) return;
    }
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sockets)) return;
    NativeChildProcess_Fd fd{}; fd.fdName = const_cast<char*>("gnu_result"); fd.fd = sockets[1];
    int audioFd=work.parameters.compare(0,6,"steam:")==0?CreateAudioBootstrapFd("/data/storage/el2/base/files/.wine"):-1;
    OH_LOG_Print(LOG_APP, LOG_INFO, 0, "HMOSDECK", "GNU bootstrap ready audio=%{public}d", audioFd>=0);
    AudioBootstrapRelay audioRelay;
    std::string parameters=work.parameters;
    if(audioFd>=0&&audioRelay.Start(audioFd))parameters+="\nAUDIO="+audioRelay.Path();
    if(work.forkBackend)parameters+="\nFORK=1";
    NativeChildProcess_Fd audio{};audio.fdName=const_cast<char*>("hmosdeck_audio");audio.fd=audioFd;
    if(audioFd>=0)fd.next=&audio;
    NativeChildProcess_Args args{}; args.entryParams = const_cast<char*>(parameters.c_str()); args.fdList.head = &fd;
    NativeChildProcess_Options options{}; options.isolationMode = NCP_ISOLATION_MODE_NORMAL;
    int32_t pid = -1;
    OH_LOG_Print(LOG_APP, LOG_INFO, 0, "HMOSDECK", "GNU child start begin backend=%{public}s", work.forkBackend ? "fork" : "ncp");
    int code = OH_Ability_StartNativeChildProcess("libsteamohos_gnu_child.so:SteamOhosGnuChildMain", args, options, &pid);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0, "HMOSDECK", "GNU child start end backend=%{public}s code=%{public}d pid=%{public}d", work.forkBackend ? "fork" : "ncp", code, pid);
    // Phone_StartNativeChildProcess consumes passed descriptors after handshake.
    if (!work.forkBackend || code != NCP_NO_ERROR) {
        // The phone helper also consumes FDs on a failed handshake after spawn.
        if (!work.forkBackend || code != NCP_ERR_LIB_LOADING_FAILED) {
            close(sockets[1]);
            if(audioFd>=0)close(audioFd);
        }
    }
    if (code != NCP_NO_ERROR || pid <= 0) {
        close(sockets[0]);
        work.json = "{\"schemaVersion\":1,\"status\":\"ncp_start_failed\",\"ncpError\":" + std::to_string(code) +
            ",\"gnuExecutionVerified\":false}";
        return;
    }
    if (work.parameters.compare(0, 6, "steam:") == 0) GameLaunchBroker().SetAncestor(pid);
    std::string output;
    bool exited = false, eof = false, overflow = false;
    int exitSignal = -1;
    const auto observeExit = [&] {
        if (work.forkBackend) {
            WineProcessEntry entry{};
            if (QueryProcessSnapshot(pid, &entry) && !entry.running) {
                exited = true;
                exitSignal = entry.exitCodeSource == "signal" ? entry.exitCode : 0;
            }
        } else {
            std::lock_guard<std::mutex> lock(ExitsMutex);
            auto it = Exits.find(pid);
            if (it != Exits.end()) { exited = true; exitSignal = it->second; }
        }
    };
    // A bounded four-hour research session permits downloads; probes stay short.
    const bool steamSession = work.parameters.compare(0, 6, "steam:") == 0;
    uint64_t deadline = Now() + (steamSession ? 14430000 : 70000);
    while (work.productSession || Now() < deadline) {
        char bytes[2048];
        ssize_t n = recv(sockets[0], bytes, sizeof(bytes), 0);
        if (n > 0) {
            if (output.size() + n > 2 * 1024 * 1024) { overflow = true; break; }
            output.append(bytes, static_cast<size_t>(n));
        } else if (!n) eof = true;
        else if (errno != EAGAIN && errno != EINTR) break;
        observeExit();
        if (exited && eof) break;
        pollfd pollFd{sockets[0], POLLIN, 0};
        if (eof) poll(nullptr, 0, 20);
        else poll(&pollFd, 1, 20);
    }
    close(sockets[0]);
    if (!exited) {
        if (work.forkBackend) KillChildProcess(pid);
        else OH_Ability_KillChildProcess(pid);
        const uint64_t until = Now() + 1500;
        while (Now() < until) {
            observeExit();
            if (exited) break;
            poll(nullptr, 0, 20);
        }
    }
    { std::lock_guard<std::mutex> lock(ExitsMutex); Exits.erase(pid); }
    const std::string childResult = ",\"childProcessBackend\":\"" + std::string(work.forkBackend ? "fork" : "ncp") +
        "\",\"childExitVerified\":" + (exited ? "true" : "false");
    if (!overflow && !output.empty() && output.front() == '{' && output.back() == '}') {
        output.pop_back();
        work.json = output + childResult + ",\"ncpExitVerified\":" + (!work.forkBackend && exited ? "true" : "false") +
            ",\"ncpExitSignal\":" + std::to_string(exitSignal) + '}';
    } else {
        work.json = "{\"schemaVersion\":1,\"status\":\"ncp_result_missing\",\"gnuExecutionVerified\":false,"
            "\"ncpResultOverflow\":" + std::string(overflow ? "true" : "false") +
            ",\"ncpResultBytes\":" + std::to_string(output.size()) + childResult +
            ",\"ncpExitVerified\":" + std::string(!work.forkBackend && exited ? "true" : "false") +
            ",\"ncpExitSignal\":" + std::to_string(exitSignal) + '}';
    }
}
void CompleteGnu(napi_env env, napi_status status, void* data) {
    std::unique_ptr<GnuWork> work(static_cast<GnuWork*>(data));
    GnuBusy.store(false);
    if (!env) return;
    napi_value result;
    if (status == napi_ok && napi_create_string_utf8(env, work->json.c_str(), work->json.size(), &result) == napi_ok)
        napi_resolve_deferred(env, work->deferred, result);
    else {
        napi_create_string_utf8(env, "GNU native diagnostic failed", NAPI_AUTO_LENGTH, &result);
        napi_reject_deferred(env, work->deferred, result);
    }
    napi_delete_async_work(env, work->work);
}
}
static napi_value StartGnuProbe(napi_env env, int mode) {
    if (GnuBusy.exchange(true)) { napi_throw_error(env, nullptr, "GNU native diagnostic is busy"); return nullptr; }
    auto work = std::make_unique<GnuWork>();
    work->forkBackend = PhoneAdapter_IsPhoneMode();
    work->productSession = mode == 4;
    if (mode) work->parameters = std::string(mode == 1 ? "wayland:" : mode == 2 ? "xwayland:" : "steam:") + gSockPath;
    if (mode == 3 || mode == 4) {
        // Capture the existing receiver; never restart it or mutate Wine's environment.
        const auto state = winehua::GraphicsBroker::GetInstance().GetState();
        if (state.virglSocketReady && !state.virglSocketPath.empty() &&
            state.virglSocketPath.find_first_of("\r\n") == std::string::npos)
            work->parameters += "\n" + state.virglSocketPath;
        else work->parameters += "\n";
        const auto launch = GameLaunchBroker().Parameters();
        if (!launch.empty()) work->parameters += "\n" + launch;
    }
    if(mode==3 || mode==4) {
        // Share the existing hub/server, including physical and touch sources.
        auto& bridge=winehua::controller::GamepadBridge::Instance();
        winehua::controller::EnsurePhysicalGamepadInitialized();
        bridge.AttachToHub();
        const auto endpoint=bridge.SocketPath();
        if(bridge.Start(endpoint.empty()?"/data/storage/el2/base/files/.wine/whgp.sock":endpoint))
            work->parameters+="\nGAMEPAD="+bridge.SocketPath();
    }
    if (mode == 4) work->parameters += "\nPRODUCT=1";
    napi_value promise, name;
    if (napi_create_promise(env, &work->deferred, &promise) != napi_ok) { GnuBusy.store(false); return nullptr; }
    if (napi_create_string_utf8(env, "LinuxSteamNativeGnuProbe", NAPI_AUTO_LENGTH, &name) != napi_ok ||
        napi_create_async_work(env, nullptr, name, ExecuteGnu, CompleteGnu, work.get(), &work->work) != napi_ok) {
        GnuBusy.store(false);
        napi_create_string_utf8(env, "Cannot allocate GNU native worker", NAPI_AUTO_LENGTH, &name);
        napi_reject_deferred(env, work->deferred, name);
        return promise;
    }
    if (napi_queue_async_work(env, work->work) != napi_ok) {
        GnuBusy.store(false); napi_delete_async_work(env, work->work);
        napi_reject_deferred(env, work->deferred, name); return promise;
    }
    work.release(); return promise;
}
napi_value StartSteamSession(napi_env env, napi_callback_info) { return StartGnuProbe(env, 4); }
napi_value ProbeGnuSteam(napi_env env, napi_callback_info) { return StartGnuProbe(env, 3); }
napi_value ProbeGnuXwayland(napi_env env, napi_callback_info) { return StartGnuProbe(env, 2); }
napi_value ProbeGnu(napi_env env, napi_callback_info) { return StartGnuProbe(env, 0); }
napi_value ProbeGnuWayland(napi_env env, napi_callback_info) { return StartGnuProbe(env, 1); }
}
