// SPDX-License-Identifier: LGPL-2.1-or-later
#include "spawn_broker.h"
#include "process_backend.h"
#include "BrokerTransport.h"
#include <cerrno>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

namespace winehua::gamescope {
namespace {
bool SafeField(const std::string& value) {
    return !value.empty() && value.size() < 1024 && value.find_first_of("|\r\n") == std::string::npos;
}
}
SpawnBroker::SpawnBroker(int endpoint, std::string runtimeDirectory, std::string logPath)
    : endpoint_(endpoint), runtimeDirectory_(std::move(runtimeDirectory)), logPath_(std::move(logPath)),
      worker_([this] { Run(); }) {}
SpawnBroker::~SpawnBroker() { Stop(); close(endpoint_); }
void SpawnBroker::Stop() {
    stopped_.store(true);
    shutdown(endpoint_, SHUT_RDWR);
    if (worker_.joinable()) worker_.join();
}
void SpawnBroker::Run() {
    using namespace ::gamescope::ohos;
    if (!SafeField(runtimeDirectory_) || !SafeField(logPath_)) return;
    uint32_t previous = 0;
    while (!stopped_.load()) {
        SpawnMessage message;
        if (!ReceiveSpawn(endpoint_, message, 250)) {
            if (errno == ETIMEDOUT) continue;
            break;
        }
        const auto& request = message.request;
        SpawnResult result;
        result.sequence = request.sequence;
        if (request.sequence <= previous) { result.error = EPROTO; SendSpawnResult(endpoint_, result); break; }
        previous = request.sequence;
        // Reuse the product Xwayland shim's only argv contract. No shell,
        // exec, GNU library or alternative hardcoded Xwayland arguments.
        std::string params = std::string(request.displayName)+"|"+
            std::to_string(request.terminateDelay)+"|"+std::to_string(request.noTouch)+"|"+
            std::to_string(request.xrandr)+"|"+std::to_string(request.enableWm)+"|"+
            logPath_+"|"+std::to_string(getpid())+"|"+runtimeDirectory_;
        for (uint32_t offset = 0; offset < request.argvBytes;) {
            params += "|"+std::string(request.argv+offset);
            offset += strlen(request.argv+offset)+1;
        }
        NativeChildProcess_Fd nodes[MaxSpawnFds]{};
        const char* names[MaxSpawnFds]{"x_fd0", "x_fd1", "wl_fd", "wm_fd", "displayfd"};
        if (!request.enableWm) names[3] = "displayfd";
        for (size_t i = 0; i < message.fds.size(); ++i) {
            nodes[i].fdName = const_cast<char*>(names[i]); nodes[i].fd = message.fds[i];
            nodes[i].next = i+1 < message.fds.size() ? &nodes[i+1] : nullptr;
        }
        NativeChildProcess_Args args{};
        args.entryParams = params.data(); args.fdList.head = nodes;
        result.error = StartRuntimeChild("libxwayland_child.so:Main", args, &result.pid);
        if (!SendSpawnResult(endpoint_, result)) {
            // A timed-out/retired compositor cannot own a newly launched
            // Xserver. Request a normal termination; never report success.
            if (result.error == 0 && result.pid > 0) kill(result.pid, SIGTERM);
            break;
        }
    }
}
} // namespace winehua::gamescope
