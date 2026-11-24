#pragma once
// FN2-2a: the service's SpectralDenoise (dsp_dynamics._apply_spectral_denoise,
// FIX-B) as a native chain stage. Per channel, on the signal padded with
// 2048 zeros in front and 2048 + 512 behind:
//   * STFT: 2048-point periodic Hann, hop 512 (frame f covers padded samples
//     [512 f, 512 f + 2048));
//   * noise profile: every 4th frame's power spectrum, each bin's 10th
//     percentile (numpy's linear method) over the whole track, divided by
//     -ln(1 - 0.1) - a noise bin's power is exponential, so its 10th
//     percentile is -ln(0.9) of its mean;
//   * each frame keeps sqrt(g) of its spectrum, g = max(1 - sensitivity N/P,
//     10^(-reduction_db/10)), smoothed over bins ([0.25, 0.5, 0.25], zero
//     outside) and released over time (g < previous: 0.7 prev + 0.3 g);
//   * windowed overlap-add, divided by the summed squared window.
// A track shorter than one frame (2048 samples) passes unchanged.
//
// The profile needs the whole track before the first frame is processed, so
// the stage needs a profile pass (ChainProcessor::runProfilePasses). Its
// memory is bounded: the profile keeps every value (exact, as numpy) for the
// first kExactProfileFrames profiled frames - about 3 minutes at 44.1 kHz -
// and then folds them into a per-bin histogram of 0.05 dB buckets (-200..140 dB), read with
// linear interpolation inside the bucket (well under 0.05 dB from the exact
// percentile, tests/shared-core/test_offline_chain_renderer.cpp) - a fixed
// 28 MB per channel whatever the length.
// THREAD_CONTEXT(WorkerThread) - one instance per render.
#include <khensu/dsp/ChainProcessor.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace khensu::dsp {

namespace denoise {
constexpr int kFftSize = 2048;
constexpr int kHop = 512;
constexpr int kBins = kFftSize / 2 + 1;
constexpr int kProfileStride = 4;            ///< the profile reads every 4th frame
constexpr double kProfilePercentile = 10.0;  ///< _DENOISE_PROFILE_PERCENTILE
constexpr double kRelease = 0.7;             ///< _DENOISE_RELEASE
constexpr int kExactProfileFrames = 4096;    ///< then the histogram (see above)
} // namespace denoise

/// The p-th percentile of each column over a stream of rows, numpy's
/// default ("linear") method: exact while at most `exactRowLimit` rows have
/// been added, then from a histogram of 10 log10(value) in kStepDb buckets.
class PercentileProfile {
  public:
    static constexpr double kMinDb = -200.0, kMaxDb = 140.0, kStepDb = 0.05;

    PercentileProfile(int columns, int exactRowLimit);
    /// `row` has columns() non-negative values.
    void add(const double* row);
    /// One value per column; zeros before the first row.
    std::vector<double> percentile(double p) const;
    std::int64_t rows() const noexcept { return rows_; }
    int columns() const noexcept { return columns_; }
    bool exact() const noexcept { return hist_.empty(); }

  private:
    void addToHistogram(int column, double value);
    void toHistogram();
    int columns_, limit_, buckets_;
    std::int64_t rows_ = 0;
    std::vector<float> values_;       ///< exact mode, row-major
    std::vector<std::uint32_t> hist_; ///< histogram mode, [column * buckets_ + bucket]
};

/// The SpectralDenoise stage (parameters already clamped as the service
/// clamps them). `exactProfileFrames` is the PercentileProfile's exact limit.
std::unique_ptr<ChainStage> makeSpectralDenoiseStage(double reductionDb, double sensitivity, int numChannels,
                                                     int exactProfileFrames = denoise::kExactProfileFrames);

} // namespace khensu::dsp
