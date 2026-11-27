#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

struct PixelRect {
    int64_t left = 0, top = 0, right = 0, bottom = 0;

    bool Empty() const { return left >= right || top >= bottom; }
    int64_t Width() const { return right - left; }
    int64_t Height() const { return bottom - top; }
    PixelRect Intersect(PixelRect other) const {
        return {std::max(left, other.left), std::max(top, other.top),
                std::min(right, other.right), std::min(bottom, other.bottom)};
    }
    PixelRect Bounds(PixelRect other) const {
        if (Empty()) return other;
        return {std::min(left, other.left), std::min(top, other.top),
                std::max(right, other.right), std::max(bottom, other.bottom)};
    }
};

// Rectangles are disjoint. Overlap and overflow conservatively expand coverage.
class PixelDamage {
public:
    void Add(PixelRect rect) {
        if (rect.Empty()) return;
        if (collapsed_) {
            rects_[0] = rects_[0].Bounds(rect);
            return;
        }
        for (size_t i = 0; i < count_;) {
            if (rect.Intersect(rects_[i]).Empty()) {
                ++i;
                continue;
            }
            rect = rect.Bounds(rects_[i]);
            rects_[i] = rects_[--count_];
            i = 0; // The expanded rectangle can intersect an earlier entry.
        }
        if (count_ == rects_.size()) {
            rects_[0] = Bounds().Bounds(rect);
            count_ = 1;
            collapsed_ = true;
            return;
        }
        rects_[count_++] = rect;
    }
    void AddRequest(int32_t x, int32_t y, int32_t w, int32_t h) {
        Add({x, y, static_cast<int64_t>(x) + w, static_cast<int64_t>(y) + h});
    }
    void Clear() { count_ = 0; collapsed_ = false; }
    bool Empty() const { return count_ == 0; }
    const PixelRect* begin() const { return rects_.data(); }
    const PixelRect* end() const { return begin() + count_; }
    PixelRect Bounds() const {
        PixelRect result;
        for (const auto& rect : *this) result = result.Bounds(rect);
        return result;
    }

private:
    std::array<PixelRect, 32> rects_{};
    size_t count_ = 0;
    bool collapsed_ = false;
};
