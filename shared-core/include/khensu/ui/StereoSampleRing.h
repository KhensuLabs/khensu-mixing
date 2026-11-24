#pragma once
// UI-5: a lock-free single-producer / single-consumer ring of stereo sample
// pairs - the audio thread pushes what it outputs, a scope (Vectorscope)
// drains it on the message thread. Storage is allocated once in the
// constructor; push() and pop() never allocate, lock or wait. When the ring
// is full the newest samples are dropped (a scope that is not drawing loses
// nothing it needs) and counted.
// THREAD_CONTEXT: push -> one producer thread (AudioThread);
//                 pop / clear / available -> one consumer thread (UIThread).

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace khensu::ui {

class StereoSampleRing {
  public:
    /// `capacity` is rounded up to a power of two (at least 2).
    explicit StereoSampleRing(int capacity = 8192)
    {
        std::uint32_t size = 2;
        while (size < static_cast<std::uint32_t>(std::max(2, capacity))) size <<= 1;
        left_.assign(size, 0.0f);
        right_.assign(size, 0.0f);
        mask_ = size - 1;
    }

    int capacity() const noexcept { return static_cast<int>(mask_ + 1); }

    /// Producer. `right` may be null or equal `left` (mono). Returns how many
    /// pairs were stored; the rest did not fit and were dropped.
    int push(const float* left, const float* right, int count) noexcept
    {
        if (left == nullptr || count <= 0) return 0;
        if (right == nullptr) right = left;
        const auto w = write_.load(std::memory_order_relaxed);
        const auto r = read_.load(std::memory_order_acquire);
        const auto free = static_cast<std::uint32_t>(capacity()) - (w - r);
        const auto n = std::min(free, static_cast<std::uint32_t>(count));
        for (std::uint32_t i = 0; i < n; ++i) {
            left_[(w + i) & mask_] = left[i];
            right_[(w + i) & mask_] = right[i];
        }
        write_.store(w + n, std::memory_order_release);
        if (n < static_cast<std::uint32_t>(count))
            dropped_.fetch_add(static_cast<std::uint32_t>(count) - n, std::memory_order_relaxed);
        return static_cast<int>(n);
    }

    /// Consumer. Copies up to `maxCount` of the oldest pairs out; returns
    /// how many were copied.
    int pop(float* left, float* right, int maxCount) noexcept
    {
        if (left == nullptr || right == nullptr || maxCount <= 0) return 0;
        const auto r = read_.load(std::memory_order_relaxed);
        const auto w = write_.load(std::memory_order_acquire);
        const auto n = std::min(w - r, static_cast<std::uint32_t>(maxCount));
        for (std::uint32_t i = 0; i < n; ++i) {
            left[i] = left_[(r + i) & mask_];
            right[i] = right_[(r + i) & mask_];
        }
        read_.store(r + n, std::memory_order_release);
        return static_cast<int>(n);
    }

    /// Consumer: pairs waiting to be read.
    int available() const noexcept
    {
        return static_cast<int>(write_.load(std::memory_order_acquire) - read_.load(std::memory_order_relaxed));
    }

    /// Consumer: discards everything waiting (e.g. when a scope opens).
    void clear() noexcept { read_.store(write_.load(std::memory_order_acquire), std::memory_order_release); }

    /// Pairs the producer could not store since construction.
    std::uint32_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

  private:
    std::vector<float> left_, right_;
    std::uint32_t mask_ = 1;
    std::atomic<std::uint32_t> write_{0}, read_{0};
    std::atomic<std::uint32_t> dropped_{0};
};

} // namespace khensu::ui
