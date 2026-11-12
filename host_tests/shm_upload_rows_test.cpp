#include "compositor/frame/shm_upload_rows.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <random>
#include <vector>

int main() {
    constexpr uint32_t w = 31, h = 23;
    constexpr size_t stride = w * 4;
    std::vector<uint8_t> frame(stride * h, 7), staging(frame.size(), 0);
    auto plan = winehua::PlanShmUploadRows(staging.data(), frame.data(), w, h, false);
    assert(plan.first == 0 && plan.count == h && plan.offset == 0 && plan.bytes == frame.size());
    staging = frame;
    plan = winehua::PlanShmUploadRows(staging.data(), frame.data(), w, h, true);
    assert(!plan.count && !plan.bytes);
    frame[11 * stride + 3] ^= 1; // alpha-only difference
    plan = winehua::PlanShmUploadRows(staging.data(), frame.data(), w, h, true);
    assert(plan.first == 11 && plan.count == 1 && plan.offset == 11 * stride && plan.bytes == stride);
    frame.back() ^= 1;
    plan = winehua::PlanShmUploadRows(staging.data(), frame.data(), w, h, true);
    assert(plan.first == 11 && plan.count == h - 11);
    frame.front() ^= 1;
    plan = winehua::PlanShmUploadRows(staging.data(), frame.data(), w, h, true);
    assert(plan.first == 0 && plan.count == h);

    // Model two independently fenced staging/texture slots. Rendering may
    // skip commits or reuse the same slot; each partial transfer must yield
    // the complete current image without stale rows or missing alpha edits.
    std::array<std::vector<uint8_t>, 2> slots{staging, staging};
    std::array<bool, 2> initialized{};
    std::mt19937 random(21987);
    for (unsigned step = 0; step < 1000; ++step) {
        for (unsigned edit = 0, n = random() % 7; edit < n; ++edit)
            frame[random() % frame.size()] = static_cast<uint8_t>(random());
        const auto slot = random() % slots.size();
        plan = winehua::PlanShmUploadRows(slots[slot].data(), frame.data(), w, h, initialized[slot]);
        assert(plan.first + plan.count <= h && plan.offset + plan.bytes <= frame.size());
        std::copy(frame.begin() + plan.offset, frame.begin() + plan.offset + plan.bytes,
                  slots[slot].begin() + plan.offset);
        initialized[slot] = true;
        assert(slots[slot] == frame);
        if (step % 53 == 0) initialized[slot] = false; // texture rebuild: force full upload
    }
    std::cout << "shm_upload_rows_test PASS (1000 skipped/alternating-slot frames)\n";
}
