// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "SessionProtocol.h"
namespace gamescope::ohos {
void ReportRuntimeStage(SessionStage stage, const char* detail, int error = 0);
}
