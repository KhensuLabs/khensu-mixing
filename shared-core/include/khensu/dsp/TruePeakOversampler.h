#pragma once
// OVH-9: the N-times interpolator the service's true-peak limiter and output
// stage use (analysis/loudness.py _oversampled_chunks:
// scipy.signal.resample_poly(x, N, 1, window=firwin(64 N + 1, 1/N,
// ("kaiser", 9.0))), zeros beyond the signal's ends), as a streaming
// per-sample envelope. At N = 4 it is the meters' filter
// (analysis/TruePeakFilter.h); tests pin the two together.
// THREAD_CONTEXT(Any) for the phases (immutable once built); the streaming
// classes belong to one thread.
#include <cstdint>
#include <vector>

namespace khensu::dsp {

/// Input samples either side of an interpolated point (64 taps per phase / 2).
inline constexpr int kOversampleHalf = 32;
/// Taps per polyphase phase.
inline constexpr int kOversamplePhaseTaps = 2 * kOversampleHalf + 1;

struct OversamplerPhases {
    int oversample = 1;
    /// w[p * kOversamplePhaseTaps + j]: y[N n + p] = sum_j w * x[n - 32 + j].
    std::vector<double> w;
};

/// The phases for `oversample` (2..16), built once per factor.
const OversamplerPhases& oversamplerPhases(int oversample);

/// Per input sample n: max over channels and phases of |y[N n + p]|, the
/// N interpolated points from n up to (not including) n + 1. Available 32
/// samples after n arrives; finish() completes the tail against zeros.
class OversampledEnvelope {
  public:
    void prepare(int numChannels, int oversample);
    /// One frame (one sample per channel). Returns true with `value` for
    /// n = frames pushed - 33 once that index exists.
    bool push(const double* frame, double& value) noexcept;
    /// Frames still owed after the last real one (call pushZero this often).
    int tailFrames() const noexcept { return pushed_ > 0 ? kOversampleHalf : 0; }
    bool pushZero(double& value) noexcept;
    std::int64_t framesPushed() const noexcept { return pushed_; }

  private:
    double evaluate() const noexcept;

    const OversamplerPhases* phases_ = nullptr;
    int numChannels_ = 0;
    std::vector<double> history_; // [channel][2 * taps], stored twice
    std::vector<double> zeros_;
    int pos_ = 0;
    std::int64_t pushed_ = 0;
};

/// Linear true peak of a whole stream (max over channels of the sample peak
/// and every interpolated value) - loudness.true_peak_linear(x, N).
class StreamingTruePeak {
  public:
    void prepare(int numChannels, int oversample);
    void push(const double* const* channels, int numChannels, int numSamples) noexcept;
    /// Completes the tail; call once, after the last push.
    double finishLinear() noexcept;

  private:
    OversampledEnvelope env_;
    std::vector<double> frame_;
    double peak_ = 0.0;
};

} // namespace khensu::dsp
