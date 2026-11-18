// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <cstdint>
#include <cstring>

namespace gamescope::ohos {
constexpr uint32_t BrokerMagic = 0x56475331; // VGS1
constexpr uint32_t BrokerVersion = 1;
constexpr uint32_t MaxSpawnArguments = 64;
constexpr uint32_t MaxSpawnBytes = 8192;
constexpr uint32_t MaxSpawnFds = 5;
constexpr int SpawnTimeoutMs = 5000;

// A private SOCK_SEQPACKET endpoint is passed by the app through NCP. The
// payload contains no raw pointers or platform objects; FDs use SCM_RIGHTS.
struct SpawnRequest {
    uint32_t magic = BrokerMagic;
    uint32_t version = BrokerVersion;
    uint32_t sequence = 0;
    uint32_t fdCount = 0;
    uint32_t argc = 0;
    uint32_t argvBytes = 0;
    int32_t terminateDelay = 0;
    uint32_t noTouch = 0;
    uint32_t xrandr = 0;
    uint32_t enableWm = 0;
    char displayName[16]{};
    char argv[MaxSpawnBytes]{};
};
struct SpawnResult {
    uint32_t magic = BrokerMagic;
    uint32_t version = BrokerVersion;
    uint32_t sequence = 0;
    int32_t error = -1;
    int32_t pid = -1;
};
inline bool ValidateSpawn(const SpawnRequest& request) {
    if (request.magic != BrokerMagic || request.version != BrokerVersion || !request.sequence ||
        request.enableWm > 1 || request.noTouch > 1 || request.xrandr > 1 ||
        request.terminateDelay < 0 || request.terminateDelay > 3600 ||
        request.fdCount != (request.enableWm ? 5u : 4u) || !request.argc ||
        request.argc > MaxSpawnArguments || !request.argvBytes || request.argvBytes > MaxSpawnBytes ||
        request.displayName[0] != ':') return false;
    const auto* end = static_cast<const char*>(memchr(request.displayName, 0, sizeof(request.displayName)));
    if (!end || end == request.displayName+1) return false;
    for (const char* p = request.displayName+1; p < end; ++p)
        if (*p < '0' || *p > '9') return false;
    uint32_t count = 0;
    for (uint32_t i = 0; i < request.argvBytes; ++i) {
        // The existing NCP Xwayland shim serializes argv with '|'. Preserve
        // that contract and reject ambiguous fields instead of shell quoting.
        if (request.argv[i] == '|' || request.argv[i] == '\n' || request.argv[i] == '\r') return false;
        if (!request.argv[i]) ++count;
    }
    return request.argv[request.argvBytes-1] == 0 && count == request.argc;
}
} // namespace gamescope::ohos
