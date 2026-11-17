// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

namespace gamescope {

// OHOS libc++ does not provide C++20 jthread/stop_token. These workers need
// cooperative cancellation and one fixed wake operation, not stop callbacks.
class StopToken {
public:
    explicit StopToken(std::shared_ptr<std::atomic<bool>> state) : state_(std::move(state)) {}
    bool stop_requested() const { return state_->load(std::memory_order_acquire); }
private:
    std::shared_ptr<std::atomic<bool>> state_;
};

class JoiningThread {
public:
    JoiningThread() = default;
    template<typename F>
    explicit JoiningThread(F function, std::function<void()> wake = {})
        : state_(std::make_shared<std::atomic<bool>>(false)), wake_(std::move(wake)),
          thread_([function = std::move(function), token = StopToken(state_)]() mutable { function(token); }) {}
    JoiningThread(const JoiningThread&) = delete;
    JoiningThread& operator=(const JoiningThread&) = delete;
    JoiningThread(JoiningThread&& other) noexcept
        : state_(std::move(other.state_)), wake_(std::move(other.wake_)), thread_(std::move(other.thread_)) {}
    JoiningThread& operator=(JoiningThread&& other) noexcept {
        if (this != &other) {
            stopAndJoin();
            state_ = std::move(other.state_);
            wake_ = std::move(other.wake_);
            thread_ = std::move(other.thread_);
        }
        return *this;
    }
    ~JoiningThread() { stopAndJoin(); }
    bool joinable() const { return thread_.joinable(); }
    bool request_stop() {
        if (!state_ || state_->exchange(true, std::memory_order_acq_rel)) return false;
        if (wake_) wake_();
        return true;
    }
    void join() { thread_.join(); }
private:
    void stopAndJoin() {
        if (joinable()) { request_stop(); join(); }
    }
    std::shared_ptr<std::atomic<bool>> state_;
    std::function<void()> wake_;
    std::thread thread_;
};

} // namespace gamescope
