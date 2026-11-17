// SPDX-License-Identifier: LGPL-2.1-or-later
#include "SessionProtocol.h"
#include "BrokerTransport.h"
#include "display/ncp/bounded_log.h"
#include <AbilityKit/native_child_process.h>
#include <IPCKit/ipc_kit.h>
#include <native_window/external_window.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
std::atomic<int> stateEndpoint{-1};
std::mutex bootstrapMutex;
std::condition_variable bootstrapReady;
bool configured = false, bootstrapClosed = false;
int bootstrapSession = -1, bootstrapSpawn = -1;
OHNativeWindow* bootstrapWindow = nullptr;
int Configure(uint32_t code, const OHIPCParcel* data, OHIPCParcel* reply, void*) {
    if (OH_IPCSkeleton_GetCallingUid() != static_cast<uint64_t>(getuid()) ||
        code != 1 || !data || !reply) return EINVAL;
    int32_t version = 0, session = -1, spawn = -1;
    OHNativeWindow* window = nullptr;
    int32_t result = EINVAL;
    const bool valid = OH_IPCParcel_ReadInt32(data, &version) == 0 && version == 1 &&
        OH_IPCParcel_ReadFileDescriptor(data, &session) == 0 && session >= 0 &&
        OH_IPCParcel_ReadFileDescriptor(data, &spawn) == 0 && spawn >= 0 &&
        OH_NativeWindow_ReadFromParcel(const_cast<OHIPCParcel*>(data), &window) == 0 && window;
    if (valid) {
        std::lock_guard lock(bootstrapMutex);
        result = configured || bootstrapClosed ? EBUSY : 0;
        if (!result) {
            bootstrapSession = session; bootstrapSpawn = spawn; bootstrapWindow = window;
            session = spawn = -1; window = nullptr; configured = true;
            bootstrapReady.notify_one();
        }
    }
    if (session >= 0) close(session);
    if (spawn >= 0) close(spawn);
    if (window) OH_NativeWindow_DestroyNativeWindow(window);
    if (OH_IPCParcel_WriteInt32(reply, result) != 0 || OH_IPCParcel_WriteInt32(reply, getpid()) != 0)
        return EIO;
    return 0;
}
struct OutputWindow {
    OHNativeWindow* value = nullptr;
    ~OutputWindow() { if (value) OH_NativeWindow_DestroyNativeWindow(value); }
};
int FindFd(const NativeChildProcess_Args& args, const char* name) {
    int fd = -1;
    for (auto* node = args.fdList.head; node; node = node->next) {
        if (node->fdName && strcmp(node->fdName, name) == 0) {
            if (fd >= 0) return -1;
            fd = node->fd;
        }
    }
    return fd;
}
void Report(int endpoint, gamescope::ohos::SessionStage stage, int error, const char* detail) {
    gamescope::ohos::SessionEvent event;
    event.stage = stage; event.error = error; event.pid = getpid();
    snprintf(event.detail, sizeof(event.detail), "%s", detail ? detail : "");
    send(endpoint, &event, sizeof(event), MSG_NOSIGNAL | MSG_DONTWAIT);
}
void CoreState(uint32_t stage, int error, const char* detail) {
    const int endpoint = stateEndpoint.load();
    if (endpoint >= 0) Report(endpoint, gamescope::ohos::SessionStage(stage), error, detail);
}
}
extern "C" __attribute__((visibility("default"))) void Main(NativeChildProcess_Args args) {
    using namespace gamescope::ohos;
    OutputWindow output;
    { std::lock_guard lock(bootstrapMutex); output.value = bootstrapWindow; bootstrapWindow = nullptr; }
    const int session = FindFd(args, "gs_session");
    if (session < 0) return;
    SessionConfig config;
    if (!WaitReadable(session, 5000) ||
        recv(session, &config, sizeof(config), MSG_DONTWAIT | MSG_TRUNC) != sizeof(config) ||
        !ValidateSessionConfig(config)) {
        Report(session, SessionStage::Failed, EINVAL, "session-config"); return;
    }
    DisplayChildLog log;
    if (config.mode == SessionMode::Compositor && !log.Start(config.logPath)) {
        Report(session, SessionStage::Failed, errno, "session-log"); return;
    }
    void* core = dlopen("libgamescope_core.so", RTLD_NOW | RTLD_LOCAL);
    if (!core) { Report(session, SessionStage::Failed, ENOENT, dlerror()); return; }
    auto version = reinterpret_cast<const char*(*)()>(dlsym(core, "VppGamescopeVersion"));
    auto main = reinterpret_cast<int(*)(int, char**)>(dlsym(core, "main"));
    auto setBroker = reinterpret_cast<bool(*)(int)>(dlsym(core, "VppGamescopeSetBroker"));
    auto setState = reinterpret_cast<void(*)(void(*)(uint32_t, int, const char*))>(dlsym(core, "VppGamescopeSetStateCallback"));
    auto outputState = reinterpret_cast<bool(*)(uint32_t, uint32_t, bool)>(dlsym(core, "VppGamescopeOutputState"));
    auto setWindow = reinterpret_cast<bool(*)(void*)>(dlsym(core, "VppGamescopeSetOutputWindow"));
    if (!version || !main || !setBroker || !setState || !outputState || !setWindow) {
        Report(session, SessionStage::Failed, ENOSYS, "core-api"); dlclose(core); return;
    }
    Report(session, SessionStage::Loaded, 0, version());
    if (config.mode == SessionMode::QualifyLoader) { dlclose(core); close(session); return; }
    if (!output.value || !setWindow(output.value)) {
        Report(session, SessionStage::Failed, ENOTSUP, "output-window-not-transferred"); return;
    }
    output.value = nullptr; // The core backend now owns the IPC NativeWindow.
    const int broker = FindFd(args, "gs_spawn");
    if (broker < 0 || !setBroker(broker)) { Report(session, SessionStage::Failed, ENOTCONN, "spawn-broker"); return; }
    // NCP does not inherit the app environment; phone clean-fork must use the
    // same explicit environment. No Wine DPI or graphics backend is changed.
    unsetenv("DISPLAY"); unsetenv("WAYLAND_DISPLAY"); unsetenv("WAYLAND_SOCKET");
    setenv("XDG_RUNTIME_DIR", config.runtimeDirectory, 1);
    const std::string runtimeDirectory = config.runtimeDirectory;
    const auto parent = runtimeDirectory.substr(0, runtimeDirectory.rfind('/'));
    setenv("XKB_CONFIG_ROOT", (parent + "/wine/xkb").c_str(), 1);
    setenv("XCURSOR_PATH", (parent + "/wine/share/icons").c_str(), 1);
    setenv("VPP_GAMESCOPE_SURFACE_ID", std::to_string(config.surfaceId).c_str(), 1);
    stateEndpoint.store(session); setState(CoreState);
    std::vector<std::string> values{"gamescope", "--backend", "ohos", "--expose-wayland",
        "-w", std::to_string(config.renderWidth), "-h", std::to_string(config.renderHeight),
        "-W", std::to_string(config.outputWidth), "-H", std::to_string(config.outputHeight), "-r", "60"};
    const char* filters[]{"linear", "nearest", "nearest", "fsr"};
    values.insert(values.end(), {"-F", filters[config.filter], "-S", config.filter == 2 ? "integer" : "fit",
        "--sharpness", std::to_string(config.sharpness)});
    if (config.fpsLimit) values.insert(values.end(), {"--framerate-limit", std::to_string(config.fpsLimit)});
    std::vector<char*> argv;
    for (auto& value : values) argv.push_back(value.data());
    argv.push_back(nullptr);
    std::atomic<bool> finished{false};
    std::thread control([&] {
        while (!finished.load()) {
            if (!WaitReadable(session, 100)) {
                if (errno == ETIMEDOUT) continue;
                kill(getpid(), SIGTERM); break;
            }
            SessionCommand command;
            if (recv(session, &command, sizeof(command), MSG_DONTWAIT | MSG_TRUNC) != sizeof(command) ||
                command.magic != SessionMagic || command.version != 1 || command.size != sizeof(command)) {
                kill(getpid(), SIGTERM); break;
            }
            if (command.kind == CommandKind::Stop) { kill(getpid(), SIGTERM); break; }
            if (command.kind != CommandKind::OutputState || command.visible > 1 ||
                !outputState(command.width, command.height, command.visible != 0)) {
                Report(session, SessionStage::Failed, EINVAL, "invalid-command");
                kill(getpid(), SIGTERM); break;
            }
        }
    });
    const int result = main(int(values.size()), argv.data());
    finished.store(true); control.join();
    setState(nullptr); stateEndpoint.store(-1);
    Report(session, SessionStage::Returned, result, "core-returned");
    // Core globals may reference Vulkan until process teardown. Never dlclose
    // a running or just-retired compositor and invalidate callback code.
}
extern "C" __attribute__((visibility("default"))) OHIPCRemoteStub* NativeChildProcess_OnConnect() {
    return OH_IPCRemoteStub_Create("com.vintage.pomelopro.Gamescope.v1", Configure, nullptr, nullptr);
}
extern "C" __attribute__((visibility("default"))) void NativeChildProcess_MainProc() {
    {
        std::unique_lock lock(bootstrapMutex);
        if (!bootstrapReady.wait_for(lock, std::chrono::seconds(10), [] { return configured; })) {
            bootstrapClosed = true; return;
        }
        bootstrapClosed = true;
    }
    NativeChildProcess_Fd nodes[2]{};
    nodes[0].fdName = const_cast<char*>("gs_session"); nodes[0].fd = bootstrapSession; nodes[0].next = &nodes[1];
    nodes[1].fdName = const_cast<char*>("gs_spawn"); nodes[1].fd = bootstrapSpawn;
    NativeChildProcess_Args args{}; args.entryParams = const_cast<char*>("gamescope-v1"); args.fdList.head = nodes;
    Main(args);
    close(bootstrapSession); close(bootstrapSpawn);
}
