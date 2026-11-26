// desktop-app/Source/ui/shared/FrameFifo.h
// STU-1: the overlapping-frame FIFO behind a short-time spectrum. Samples go
// in one at a time; once a whole frame is held, readFrame() copies it out
// oldest first and advance() drops one hop. Fixed storage of one frame, so
// nothing is allocated or moved per sample or per hop (the live spectrogram
// used to erase the front of a std::vector on every hop, an O(frame) memmove).
// THREAD_CONTEXT(Any) - single producer/consumer on one thread, not thread-safe.
#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>

namespace khensu {

class FrameFifo {
  public:
    /// `frame` samples per frame, `hop` dropped per advance (0 < hop <= frame).
    FrameFifo(int frame, int hop)
        : frame_(std::max(1, frame)), hop_(std::clamp(hop, 1, std::max(1, frame))),
          buf_(static_cast<size_t>(std::max(1, frame)), 0.0f)
    {}

    /// Adds one sample. Returns true when a whole frame is held; the caller
    /// then reads it and advances before pushing again.
    bool push(float s) noexcept
    {
        if (count_ >= frame_) return true; // full: the caller has to advance first
        buf_[static_cast<size_t>((head_ + count_) % frame_)] = s;
        ++count_;
        return count_ == frame_;
    }

    bool full() const noexcept { return count_ == frame_; }
    int size() const noexcept { return count_; }
    int frameSize() const noexcept { return frame_; }
    int hopSize() const noexcept { return hop_; }

    /// Copies the held samples, oldest first, to dest[0 .. size()).
    void readFrame(float* dest) const noexcept
    {
        const int first = std::min(count_, frame_ - head_);
        std::copy_n(buf_.data() + head_, first, dest);
        std::copy_n(buf_.data(), count_ - first, dest + first);
    }

    /// Drops the oldest `hop` samples (or all of them, when fewer are held).
    void advance() noexcept
    {
        const int n = std::min(hop_, count_);
        head_ = (head_ + n) % frame_;
        count_ -= n;
    }

    void clear() noexcept { head_ = count_ = 0; }

  private:
    int frame_, hop_;
    std::vector<float> buf_;
    int head_ = 0, count_ = 0;
};

} // namespace khensu
