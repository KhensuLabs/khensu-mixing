#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/analysis/ChannelLayout.h>
#include <khensu/analysis/KWeighting.h>
#include <khensu/analysis/TruePeakFilter.h>
#include <array>
#include <vector>

namespace khensu {

/// ITU-R BS.1770-4 K-weighted loudness measurement.
/// THREAD_CONTEXT(WorkerThread) - offline, may block.
class LoudnessAnalyzer {
  public:
    static constexpr int kMaxChannels = 8;
    /// Taps per polyphase phase of the 4x true-peak interpolator: the shared
    /// filter the native meters and the Python service use (MEAS-5).
    static constexpr int kTruePeakTaps = analysis::kTruePeakPhaseTaps;

    /// Layout-aware reset. Applies ITU-R BS.1770 per-channel weighting:
    /// LFE excluded, surround/back channels +1.5 dB (power-domain x1.41).
    void reset(double sampleRate, const ChannelLayout& layout);

    /// Backward-compatible overload. Resolves the layout from the count and
    /// falls back to all-unity weighting for counts AudioAnalyzer rejects.
    void reset(double sampleRate, int numChannels);

    void process(const float** channelData, int numChannels, int numSamples);

    /// The two K-weighting biquads' {b0, b1, b2, a1, a2} at `sampleRate`
    /// (a0 normalised to 1) - exposed so tests can pin them to the ITU table.
    /// Defined once, in khensu/analysis/KWeighting.h, and shared with the
    /// plugin's live meters (FIX-11).
    using KWeightingCoefficients = analysis::KWeightingCoefficients;
    static KWeightingCoefficients kWeightingCoefficients(double sampleRate);
    AnalysisResult::LoudnessData getResult() const;

  private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double process(double x) noexcept
        {
            double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void reset() noexcept { z1 = z2 = 0.0; }
        /// Loads {b0, b1, b2, a1, a2} and clears the state.
        void set(const std::array<double, 5>& c) noexcept
        {
            b0 = c[0];
            b1 = c[1];
            b2 = c[2];
            a1 = c[3];
            a2 = c[4];
            reset();
        }
    };

    double m_sampleRate = 48000.0;
    int m_numChannels = 2;
    int m_blockTarget = 0; // 100ms in samples
    int m_blockCount = 0;
    double m_blockSumSq = 0.0; // K-weighted sum-of-squares across all channels
    double m_peakSample = 0.0; ///< max of the sample peak and every interpolated value
    /// True-peak history, stored twice (2 x kTruePeakTaps) so the newest
    /// kTruePeakTaps samples are always contiguous.
    std::array<std::array<double, 2 * kTruePeakTaps>, kMaxChannels> m_peakHistory;
    std::array<int, kMaxChannels> m_peakWriteIdx;
    /// Samples pushed per channel. An interpolated value is computed once its
    /// window is complete (kTruePeakHalf samples later); getResult() finishes
    /// the last kTruePeakHalf against zeros, as resample_poly does.
    std::array<long long, kMaxChannels> m_peakSeen{};

    std::array<Biquad, kMaxChannels> m_pre; // K-weighting stage 1 (high shelf)
    std::array<Biquad, kMaxChannels> m_rlb; // K-weighting stage 2 (high-pass)

    /// Power-domain BS.1770 weight per channel, indexed by channel.
    std::array<double, kMaxChannels> m_weights{};

    std::vector<double> m_blockPowers; // 100ms K-weighted channel-summed mean-square values

    void flushBlock();
};

} // namespace khensu
