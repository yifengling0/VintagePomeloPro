// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "SessionProtocol.h"
#include <cstdint>
#include <string>

namespace winehua::gamescope {
struct RuntimeSessionStatus {
    int32_t pid = -1, error = 0;
    bool active = false, outputReady = false, displayReady = false, presented = false;
    std::string stage = "idle", detail, display;
};
int StartSession(const ::gamescope::ohos::SessionConfig& config);
bool RequestSessionStop();
bool UpdateSessionOutput(uint32_t width, uint32_t height, bool visible);
RuntimeSessionStatus SessionStatus();
}
