#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include "present_pacing.h"

// Shared presenter clocks and diagnostics; product pacing stays single-source.

namespace winehua {

using SteadyClock = std::chrono::steady_clock;

// Product pacing is shared by Vulkan and GL in present_pacing.h.
constexpr uint64_t kDefaultFramePeriodNs = kDefaultPresentFramePeriodNs;
constexpr uint64_t kReleaseFenceWatchdogNs = 1000000000;

// -- 时钟 (本命名空间内唯一实现) --
inline uint64_t NowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        SteadyClock::now().time_since_epoch()).count());
}

inline uint64_t NowUs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        SteadyClock::now().time_since_epoch()).count());
}

// -- WINEHUA_VTEST_PRESENT_PERF_SUMMARY 三位开关 (仅只在三个调用点打过日志) --
inline bool PresentPerfSummaryEnabled()
{
    const char* summary = std::getenv("WINEHUA_VTEST_PRESENT_PERF_SUMMARY");
    return summary && summary[0] == '1' && !summary[1];
}

} // namespace winehua
