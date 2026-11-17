// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <memory>
#include <mutex>
#include <utility>

namespace gamescope {
// OHOS libc++ lacks atomic<shared_ptr<T>>; the old free atomic operations are
// deprecated by newer host libraries. Publish snapshots under one mutex on
// both toolchains, and destroy the previous owner outside that mutex.
template<typename T> class AtomicShared {
public:
    AtomicShared() = default;
    AtomicShared(std::nullptr_t) {}
    AtomicShared(const AtomicShared&) = delete;
    AtomicShared& operator=(const AtomicShared&) = delete;
    operator std::shared_ptr<T>() const { return load(); }
    std::shared_ptr<T> load() const { std::lock_guard lock(mutex_); return value_; }
    void store(std::shared_ptr<T> value) {
        { std::lock_guard lock(mutex_); value_.swap(value); }
    }
    AtomicShared& operator=(std::shared_ptr<T> value) { store(std::move(value)); return *this; }
    AtomicShared& operator=(std::nullptr_t) { store({}); return *this; }
private:
    mutable std::mutex mutex_;
    std::shared_ptr<T> value_;
};
} // namespace gamescope
