#include <khensu/dsp/LookaheadBrickwallLimiter.h>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace khensu::dsp {

void LookaheadBrickwallLimiter::prepare(double sampleRate, int maxChannels, int lookaheadSamples)
{
    lookahead_ = std::max(1, lookaheadSamples);
    maxChannels_ = std::max(1, maxChannels);

    const double rel = 0.050; // 50 ms release
    releaseCoeff_ = 1.0f - static_cast<float>(std::exp(-1.0 / (rel * std::max(1.0, sampleRate))));

    delay_.assign(static_cast<size_t>(maxChannels_), std::vector<float>(static_cast<size_t>(lookahead_), 0.0f));
    required_.assign(static_cast<size_t>(lookahead_ + 1), 1.0f);
    releasedRing_.assign(static_cast<size_t>(lookahead_ + 1), 1.0f);
    // The min deque holds at most L+1 entries (the window span). +1 slack so
    // the ring's "full" state (count == capacity) is never ambiguous with head.
    mono_.assign(static_cast<size_t>(lookahead_ + 2), Node{});
    reset();
}

void LookaheadBrickwallLimiter::reset()
{
    for (auto& ch : delay_)
        std::fill(ch.begin(), ch.end(), 0.0f);
    std::fill(required_.begin(), required_.end(), 1.0f);
    std::fill(releasedRing_.begin(), releasedRing_.end(), 1.0f);
    boxSum_ = static_cast<double>(releasedRing_.size());
    ringPos_ = 0;
    delayPos_ = 0;
    sampleCounter_ = 0;
    gain_ = 1.0f;
    released_ = 1.0f;
    monoHead_ = 0;
    monoTail_ = 0;
    monoCount_ = 0;
}

// Pushes `value` (the required gain for `sampleIndex`) and returns the minimum
// over the trailing window of indices [sampleIndex - lookahead_, sampleIndex].
// Ascending monotonic deque: the front is always the window min.
float LookaheadBrickwallLimiter::windowedMinPush(long long sampleIndex, float value)
{
    const int cap = static_cast<int>(mono_.size());

    // Drop entries larger than the incoming value from the back - they can
    // never again be the min while `value` is in the window.
    while (monoCount_ > 0) {
        const int backIdx = (monoTail_ - 1 + cap) % cap;
        if (mono_[static_cast<size_t>(backIdx)].val >= value) {
            monoTail_ = backIdx;
            --monoCount_;
        } else {
            break;
        }
    }
    mono_[static_cast<size_t>(monoTail_)] = Node{sampleIndex, value};
    monoTail_ = (monoTail_ + 1) % cap;
    ++monoCount_;

    // Evict entries that have fallen out of the window from the front.
    const long long oldest = sampleIndex - static_cast<long long>(lookahead_);
    while (monoCount_ > 0 && mono_[static_cast<size_t>(monoHead_)].idx < oldest) {
        monoHead_ = (monoHead_ + 1) % cap;
        --monoCount_;
    }

    return mono_[static_cast<size_t>(monoHead_)].val;
}

void LookaheadBrickwallLimiter::process(float* const* channels, int numChannels, int numSamples, float ceilingLinear,
                                        bool enabled, const float* detectorPeakPerSample)
{
    if (delay_.empty() || numSamples <= 0) return; // not prepared
    const int chs = std::min(numChannels, maxChannels_);
    const int ringLen = lookahead_ + 1;
    const double boxScale = 1.0 / static_cast<double>(ringLen);

    for (int i = 0; i < numSamples; ++i) {
        // Detector peak for this input sample (external oversampled estimate,
        // or the raw across-channel max).
        float peak = 0.0f;
        if (detectorPeakPerSample != nullptr) {
            peak = detectorPeakPerSample[i];
        } else {
            for (int c = 0; c < chs; ++c)
                peak = std::max(peak, std::abs(channels[c][i]));
        }

        // 1. Required gain for this sample. Tracked even while disabled, so
        // the pipeline is valid the moment the limiter is switched on. A NaN
        // detector value yields no gain information (unity); containing
        // non-finite audio is the caller's job (see CopilotOutputStage).
        float required = 1.0f;
        if (std::isfinite(peak) && peak > ceilingLinear && peak > 1e-12f) required = ceilingLinear / peak;

        // 2. Hold: min over the L+1 samples that are still in flight.
        const float held = windowedMinPush(sampleCounter_, required);

        // 3. Instant-down / smoothed-up release, never above the hold.
        if (held < released_)
            released_ = held;
        else
            released_ += releaseCoeff_ * (held - released_);

        // 4. Box-average over the lookahead so reductions ramp in smoothly.
        const auto pos = static_cast<size_t>(ringPos_);
        boxSum_ += static_cast<double>(released_) - static_cast<double>(releasedRing_[pos]);
        releasedRing_[pos] = released_;
        required_[pos] = required;
        ringPos_ = (ringPos_ + 1) % ringLen;
        if (ringPos_ == 0) // re-sum once per wrap: O(1) amortized, bounds drift
            boxSum_ = std::accumulate(releasedRing_.begin(), releasedRing_.end(), 0.0);

        // After the write, the oldest entry (at ringPos_) is r[n-L]: the gain
        // the sample leaving the delay line needs. Clamp to it so rounding in
        // the running sum can never exceed the proven bound.
        const float requiredForDelayed = required_[static_cast<size_t>(ringPos_)];
        const float smoothed = static_cast<float>(boxSum_ * boxScale);
        gain_ = enabled ? std::min(smoothed, requiredForDelayed) : 1.0f;

        for (int c = 0; c < chs; ++c) {
            float* ring = delay_[static_cast<size_t>(c)].data();
            const float delayed = ring[static_cast<size_t>(delayPos_)];
            ring[static_cast<size_t>(delayPos_)] = channels[c][i];
            channels[c][i] = delayed * gain_;
        }
        delayPos_ = (delayPos_ + 1) % lookahead_;
        ++sampleCounter_;
    }

    // Channels without delay storage cannot be kept sample-aligned with the
    // rest; silence them instead of passing them through L samples early.
    for (int c = chs; c < numChannels; ++c)
        std::fill(channels[c], channels[c] + numSamples, 0.0f);
}

} // namespace khensu::dsp
