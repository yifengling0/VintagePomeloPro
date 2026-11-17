#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>

namespace winehua::game {
// One immutable policy per Wine host session; UI browsing is independent.
// Bits: enabled, nearest, integer; FPS is restricted to default/30/60.
inline std::atomic<uint32_t> presentation{0};
inline void Configure(bool enabled, bool nearest, bool integer, int fps) {
    presentation.store(enabled ? (1u | (nearest ? 2u : 0u) | (integer ? 6u : 0u) |
        (static_cast<uint32_t>(fps == 30 || fps == 60 ? fps : 0) << 8)) : 0u,
        std::memory_order_release);
}
inline bool Enabled() { return (presentation.load(std::memory_order_acquire) & 1u) != 0; }
inline bool Nearest() { return (presentation.load(std::memory_order_acquire) & 2u) != 0; }
inline bool Integer() { return (presentation.load(std::memory_order_acquire) & 4u) != 0; }
inline uint64_t LimitPeriodNs() {
    const auto fps = presentation.load(std::memory_order_acquire) >> 8;
    return fps ? (1'000'000'000ull + fps - 1) / fps : 0;
}
inline uint64_t ProducerPeriodNs(uint64_t displayPeriod) {
    return std::max(displayPeriod, LimitPeriodNs());
}

// Deadline has no accumulated debt: a slow game never causes catch-up bursts.
struct FrameDeadline {
    uint64_t next = 0;
    bool Ready(uint64_t now, uint64_t period) {
        if (!period) { next = 0; return true; }
        if (now < next) return false;
        next = now + period;
        return true;
    }
};
}
