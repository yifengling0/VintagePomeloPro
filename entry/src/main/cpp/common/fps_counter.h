#pragma once
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <cstdint>

#undef LOG_TAG
#define LOG_TAG "WL_FPS"
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#include <hilog/log.h>

// Per-toplevel displayed FPS published from EglRenderer present threads
// and read by ArkTS via NAPI. Tick is render-thread; Get is JS-thread.
class DisplayFpsRegistry {
public:
    static DisplayFpsRegistry& Instance() {
        static DisplayFpsRegistry s;
        return s;
    }

    void Publish(uint32_t id, float fps) {
        if (id == 0) return;
        std::lock_guard<std::mutex> lock(mu_);
        values_[id] = fps;
    }

    void Remove(uint32_t id) {
        if (id == 0) return;
        std::lock_guard<std::mutex> lock(mu_);
        values_.erase(id);
    }

    void Move(uint32_t from, uint32_t to) {
        if (from == to) return;
        std::lock_guard<std::mutex> lock(mu_);
        float fps = 0.f;
        auto it = values_.find(from);
        if (it != values_.end()) {
            fps = it->second;
            values_.erase(it);
        }
        if (to != 0) values_[to] = fps;
    }

    float Get(uint32_t id) const {
        std::lock_guard<std::mutex> lock(mu_);
        if (id != 0) {
            auto it = values_.find(id);
            if (it != values_.end()) return it->second;
        }
        float best = 0.f;
        for (const auto& kv : values_) {
            if (kv.second > best) best = kv.second;
        }
        return best;
    }

private:
    DisplayFpsRegistry() = default;
    mutable std::mutex mu_;
    std::unordered_map<uint32_t, float> values_;
};

// 轻量 FPS 计数器：500ms 窗口发布 overlay 读数，每 10 秒输出一次 hilog
class FpsCounter {
public:
    explicit FpsCounter(const char* tag) : tag_(tag) {
        last_ = std::chrono::steady_clock::now();
        hudLast_ = last_;
    }

    void Tick(uint32_t toplevelId) {
        ++frames_;
        ++hudFrames_;
        auto now = std::chrono::steady_clock::now();
        const auto hudMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - hudLast_).count();
        if (hudMs >= 500) {
            const float fps = hudFrames_ * 1000.0f / static_cast<float>(hudMs);
            DisplayFpsRegistry::Instance().Publish(toplevelId, fps);
            hudFrames_ = 0;
            hudLast_ = now;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_).count();
        if (ms >= 10000) {
            const double fps = frames_ * 1000.0 / ms;
            OH_LOG_INFO(LOG_APP, "[%{public}s] %{public}.2f fps", tag_, fps);
            frames_ = 0;
            last_ = now;
        }
    }

private:
    const char* tag_;
    std::chrono::steady_clock::time_point last_;
    std::chrono::steady_clock::time_point hudLast_;
    int frames_ = 0;
    int hudFrames_ = 0;
};
