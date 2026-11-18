// SPDX-License-Identifier: LGPL-2.1-or-later
#include "GamescopeVersion.h"
#include "Runtime.h"
#include <atomic>

namespace {
using StateCallback = void(*)(uint32_t, int, const char*);
std::atomic<StateCallback> stateCallback{nullptr};
}

// Unlike main --version, querying identity does not exit its hosting process.
extern "C" __attribute__((visibility("default"))) const char* VppGamescopeVersion() {
    return gamescope::k_szGamescopeVersion;
}
extern "C" __attribute__((visibility("default"))) void VppGamescopeSetStateCallback(StateCallback callback) {
    stateCallback.store(callback);
}
void gamescope::ohos::ReportRuntimeStage(SessionStage stage, const char* detail, int error) {
    if (auto callback = stateCallback.load()) callback(uint32_t(stage), error, detail);
}
