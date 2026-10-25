#pragma once
#include <string>
#include "launch_wire.h"

namespace steamohos {
struct ProtonProbeCapabilities {
    std::string vtestSocket, audioSocket, gamepadSocket;
    std::string wineLanguage = "zh_CN";
    int audioFd = -1, cancellationFd = -1;
};
// Explicit diagnostic only. Uses a separate package, HOME and Wine prefix.
std::string ProbeGnuProton(const std::string& runtime, bool forkBackend, const std::string& waylandSocket,
    const ProtonProbeCapabilities& capabilities = {}, const LaunchRequest* manual = nullptr);
}
