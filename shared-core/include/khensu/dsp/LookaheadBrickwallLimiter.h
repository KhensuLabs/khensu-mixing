#pragma once
#include <vector>

namespace khensu::dsp {

/// Provably ceiling-safe lookahead brickwall limiter (audit CQ-03, AUDIT-030).
///
/// Gain computer, per input sample n with detector peak p[n] and lookahead L:
///  1. required gain  r[n] = min(1, ceiling / p[n]);
///  2. hold           h[n] = min(r[n-L .. n])   (sliding min over L+1 samples,
///                    an O(1)-amortized monotonic ring deque);
///  3. release        q[n] = h[n] when it drops, else a 50 ms one-pole rise
///                    toward h[n] (so q[n] <= h[n]);
///  4. attack ramp    s[n] = mean(q[n-L .. n])  (length-(L+1) box filter).
/// The output is the input delayed by L samples times s[n].
///
/// Why this holds the ceiling: every q[k] in the box window k in [n-L, n] is
/// <= h[k], and each such hold window [k-L, k] contains n-L, so
/// q[k] <= r[n-L]. Their mean is therefore <= r[n-L], the gain the delayed
/// sample x[n-L] needs. (The applied gain is additionally clamped to r[n-L] so
/// float rounding in the running sum can never push it over.)
///
/// Why it doesn't click: the previous version snapped the gain down in one
/// sample when an over entered the window (lookahead bought latency but no
/// ramp). Here the box filter spreads every reduction across the whole
/// lookahead: the gain falls by at most (1 - r)/(L+1) per sample.
///
/// All storage is allocated in prepare(); process() does no allocation, so it
/// is safe on the audio thread. Reports `latencySamples()` == L.
///
/// THREAD_CONTEXT(AudioThread) for process()/reset(); prepare() is
/// message/prep-thread only.
class LookaheadBrickwallLimiter {
  public:
    /// `lookaheadSamples` is the delay/lookahead length L (>= 1). `maxChannels`
    /// bounds the per-channel delay storage. Release time is fixed at 50 ms.
    void prepare(double sampleRate, int maxChannels, int lookaheadSamples);
    void reset();

    int latencySamples() const noexcept { return lookahead_; }

    /// Processes `channels[0..numChannels)` of `numSamples` in place. When
    /// `enabled` is false, output is the delayed input at unity gain (constant
    /// latency regardless of enable state); the gain computer keeps tracking
    /// so re-enabling mid-signal is still ceiling-safe. `detectorPeakPerSample`,
    /// if non-null, supplies a precomputed per-sample max-abs peak (e.g. from an
    /// oversampled copy, for inter-sample/true-peak detection); when null the
    /// per-sample peak is taken from the raw block.
    ///
    /// ALIGNMENT CONTRACT (CORE-02 / PLG-02): detectorPeakPerSample[i] must
    /// describe channels[*][i] - the same instant, not a later one. The ceiling
    /// proof above only holds for an aligned detector: a detector that reports
    /// each peak D samples late leaves the first D samples of every over at
    /// unity gain (a +6 dBFS step overshot a -1 dBFS ceiling by ~4 dB with a
    /// 4x FIR oversampler, D ~ 36). An oversampled detector therefore needs the
    /// audio delayed by ceil(D) before it reaches this limiter (see
    /// CopilotOutputStage), and the extra delay reported as latency. Channels beyond the
    /// prepared `maxChannels` have no delay storage and are silenced rather
    /// than passed through misaligned.
    void process(float* const* channels, int numChannels, int numSamples, float ceilingLinear, bool enabled,
                 const float* detectorPeakPerSample = nullptr);

    /// Gain applied to the most recent output sample (1 when disabled).
    float currentGain() const noexcept { return gain_; }

  private:
    float windowedMinPush(long long sampleIndex, float value);

    int lookahead_ = 1;
    int maxChannels_ = 2;
    float releaseCoeff_ = 0.0f;
    float gain_ = 1.0f;     // last applied gain
    float released_ = 1.0f; // q[n]: hold followed by smoothed release

    // Per-channel delay ring (length = lookahead_).
    std::vector<std::vector<float>> delay_;
    int delayPos_ = 0;
    long long sampleCounter_ = 0;

    // Rings of length L+1 holding r[n-L..n] and q[n-L..n]; `boxSum_` is the
    // running sum of `released` (re-summed on each wrap to bound drift).
    std::vector<float> required_;
    std::vector<float> releasedRing_;
    int ringPos_ = 0;
    double boxSum_ = 0.0;

    // Monotonic (ascending) deque of (index, value) as a preallocated ring,
    // giving O(1)-amortized min over the trailing [i-L, i] window with no
    // per-sample allocation.
    struct Node {
        long long idx = 0;
        float val = 0.0f;
    };
    std::vector<Node> mono_;
    int monoHead_ = 0;
    int monoTail_ = 0; // one past the last element
    int monoCount_ = 0;
};

} // namespace khensu::dsp
