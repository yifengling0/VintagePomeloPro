// SPDX-License-Identifier: LGPL-2.1-or-later
#include "BrokerTransport.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "wlr_begin.hpp"
#include <wlr/xwayland/server.h>
#include "wlr_end.hpp"

namespace {
std::mutex brokerMutex;
int brokerFd = -1;
uint32_t sequence = 0;
}

// Called once by the Gamescope NCP shim before entering the core. The shim
// owns the FD until main returns. Replacing a live endpoint is not supported.
extern "C" __attribute__((visibility("default"))) bool VppGamescopeSetBroker(int fd) {
    std::lock_guard lock(brokerMutex);
    if (fd < 0 || brokerFd >= 0) return false;
    brokerFd = fd;
    return true;
}

extern "C" bool wlr_ohos_spawn_xwayland(wlr_xwayland_server* server, int displayFd) {
    using namespace gamescope::ohos;
    std::lock_guard lock(brokerMutex);
    if (!server || brokerFd < 0) { errno = ENOTCONN; return false; }
    char* argv[MaxSpawnArguments]{};
    if (!wlr_xwayland_server_ohos_build_argv(server, displayFd, argv, MaxSpawnArguments)) return false;
    SpawnRequest request;
    if (++sequence == 0) ++sequence;
    request.sequence = sequence;
    request.enableWm = server->options.enable_wm;
    request.noTouch = server->options.no_touch_pointer_emulation;
    request.xrandr = server->options.force_xrandr_emulation;
    request.terminateDelay = server->options.terminate_delay;
    snprintf(request.displayName, sizeof(request.displayName), "%s", server->display_name);
    for (uint32_t i = 0; i < MaxSpawnArguments && argv[i]; ++i) {
        size_t bytes = strlen(argv[i])+1;
        if (bytes > MaxSpawnBytes-request.argvBytes) { errno = E2BIG; return false; }
        memcpy(request.argv+request.argvBytes, argv[i], bytes);
        request.argvBytes += bytes;
        ++request.argc;
    }
    int fds[MaxSpawnFds]{server->x_fd[0], server->x_fd[1], server->wl_fd[1], -1, -1};
    request.fdCount = 3;
    if (request.enableWm) fds[request.fdCount++] = server->wm_fd[1];
    fds[request.fdCount++] = displayFd;
    if (!ValidateSpawn(request)) { errno = EINVAL; return false; }
    for (uint32_t i = 0; i < request.fdCount; ++i)
        if (fds[i] < 0) { errno = EBADF; return false; }

    if (!SendSpawn(brokerFd, request, fds)) return false;
    SpawnResult result;
    if (!AwaitSpawnResult(brokerFd, request.sequence, result)) return false;
    server->pid = result.pid;
    // SCM_RIGHTS duplicated these endpoints into the app/NCP. Retain the two
    // listening sockets but retire our child-side endpoints on success only.
    close(server->wl_fd[1]);
    if (request.enableWm) close(server->wm_fd[1]);
    close(displayFd);
    return true;
}
