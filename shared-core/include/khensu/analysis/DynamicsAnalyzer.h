#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/analysis/ChannelLayout.h>
#include <array>
#include <vector>

namespace khensu {

/// Measures crest factor (peak-to-RMS) and macro dynamic range (P95-P10
/// of per-100ms block RMS). These are distinct: crest factor captures transient
/// punch while dynamic range captures track-level variation.
///
/// crest_factor_db matches the Python loudness.crest_factor_db: per-channel
/// sample peak over that channel's RMS, the max over channels, skipping
/// channels at or below -90 dBFS RMS and (layout-aware reset) the LFE.
/// THREAD_CONTEXT(WorkerThread)
class DynamicsAnalyzer {
  public:
    /// ~100ms block size at 44.1kHz for macro dynamic range computation
    int m_blockSizeSamples = 4410;

    static constexpr int kMaxChannels = 8;
    /// loudness.CREST_RMS_FLOOR_DBFS: channels this quiet are noise, not programme.
    static constexpr double kCrestRmsFloorDbfs = -90.0;

    /// Layout-aware reset: LFE channels are excluded from every measurement.
    void reset(double sampleRate, const ChannelLayout& layout);
    /// Backward-compatible overload: every channel is programme material.
    void reset(double sampleRate);
    void process(const float** channelData, int numChannels, int numSamples);
    AnalysisResult::DynamicsData getResult() const;

  private:
    double m_sumSq = 0.0; ///< overall sum-of-squares for RMS
    float m_peak = 0.0f;  ///< overall peak sample
    long long m_count = 0;

    std::array<double, kMaxChannels> m_chSumSq{}; ///< per-channel sum-of-squares (crest)
    std::array<float, kMaxChannels> m_chPeak{};   ///< per-channel sample peak (crest)
    std::array<long long, kMaxChannels> m_chCount{};
    std::array<bool, kMaxChannels> m_isLfe{};

    double m_blockSumSq = 0.0; ///< accumulator for current 100ms block
    int m_blockCount = 0;
    std::vector<float> m_blockRmsDb; ///< per-block RMS (dB) history
};

} // namespace khensu
