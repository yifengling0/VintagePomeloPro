// GNU/Linux fixture. Never link this into the OHOS host or its libc namespace.
#include "linux_steam/surface_probe.h"
#include <cstdio>
#include <cstdlib>
#include <dirent.h>

static int OpenDescriptors() {
    DIR* directory = opendir("/proc/self/fd");
    if (!directory) return -1;
    int count = 0;
    while (const auto* item = readdir(directory))
        if (item->d_name[0] != '.') ++count;
    closedir(directory);
    return count;
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* path = getenv("STEAMOHOS_WAYLAND_SOCKET");
    if (!path || path[0] != '/') return 2;
    const int before = OpenDescriptors();
    const auto result = steamohos::ProbeSurface(path, 5000);
    const int after = OpenDescriptors();
    // Input event injection is a separate gate. This stage checks both SCM_RIGHTS
    // directions: GNU SHM to OHOS and OHOS keymap to GNU, plus the real frame lifecycle.
    const bool passed = result.configured && !result.displayError && !result.windowClosed &&
        result.framesSubmitted >= 4 && result.bufferReleases == result.framesSubmitted &&
        result.frameCallbacks == result.framesSubmitted && result.keyboardKeymap &&
        before >= 0 && before == after;
    printf("STEAMOHOS_GNU_WAYLAND_V1 {\"status\":\"%s\",\"stage\":\"%s\","
        "\"configured\":%u,\"framesSubmitted\":%u,\"bufferReleases\":%u,"
        "\"frameCallbacks\":%u,\"displayError\":%u,\"keyboardKeymap\":%u,"
        "\"keyEvents\":%u,\"inputVerified\":false,\"framePresented\":false,"
        "\"fdBefore\":%d,\"fdAfter\":%d}\n",
        passed ? "gnu_wayland_shm_verified" : "gnu_wayland_failed", result.stage,
        result.configured, result.framesSubmitted, result.bufferReleases,
        result.frameCallbacks, result.displayError, result.keyboardKeymap,
        result.keyEvents, before, after);
    return passed ? 0 : 1;
}
