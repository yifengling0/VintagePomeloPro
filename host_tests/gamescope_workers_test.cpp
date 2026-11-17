// SPDX-License-Identifier: LGPL-2.1-or-later
#include "../native/gamescope/ohos/JoiningThread.h"
#include "../native/gamescope/ohos/AtomicShared.h"
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <mutex>

int main() {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<unsigned> exited{0}, wakes{0};
    auto worker = [&](gamescope::StopToken token) {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return token.stop_requested(); });
        ++exited;
    };
    auto wake = [&] { std::lock_guard lock(mutex); ++wakes; changed.notify_all(); };
    {
        // Stop before the worker starts waiting must still wake/join it.
        gamescope::JoiningThread first(worker, wake);
        assert(first.request_stop());
        assert(!first.request_stop());
        gamescope::JoiningThread second(std::move(first));
        assert(!first.joinable() && second.joinable());
        gamescope::JoiningThread third(worker, wake);
        // Assignment drains the replaced worker as well as the moved worker.
        third = std::move(second);
    }
    assert(exited == 2 && wakes == 2);

    gamescope::AtomicShared<unsigned> value = nullptr;
    std::weak_ptr<unsigned> weak;
    {
        auto owner = std::make_shared<unsigned>(123);
        weak = owner;
        value = owner;
    }
    auto snapshot = value.load();
    assert(snapshot && *snapshot == 123);
    value = nullptr;
    assert(!weak.expired() && !value.load());
    snapshot.reset();
    assert(weak.expired());
    std::thread writer([&] {
        for (unsigned i = 0; i < 10000; ++i) value = std::make_shared<unsigned>(i);
    });
    for (unsigned i = 0; i < 10000; ++i) {
        auto current = value.load();
        if (current) assert(*current < 10000);
    }
    writer.join();
    assert(*value.load() == 9999);
    std::puts("Gamescope worker cancellation, wake, move, join and shared ownership: PASS");
}
