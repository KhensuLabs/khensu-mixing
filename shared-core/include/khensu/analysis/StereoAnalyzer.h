#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/analysis/ChannelLayout.h>

namespace khensu {

/// Stereo correlation, width, and mid/side balance.
/// For mono sources, correlation = 1 and width = 0.
///
/// For surround layouts these are FRONT-PAIR imaging metrics (L/R), not a
/// whole-field measurement. Deriving a surround-field width from an L/R pair
/// would be a confident number with nothing behind it, so it is deliberately
/// not attempted.
/// THREAD_CONTEXT(WorkerThread)
class StereoAnalyzer {
  public:
    /// Defaults to a stereo front pair (channels 0 and 1).
    void reset();
    /// Reads the front pair from the layout.
    void reset(const ChannelLayout& layout);
    void process(const float** channelData, int numChannels, int numSamples);
    AnalysisResult::StereoData getResult() const;
    AnalysisResult::PhaseData getPhaseResult() const;

    /// Returns the unbiased Pearson L/R correlation [-1, 1].
    /// Shared by getResult() and getPhaseResult() to avoid duplicated computation.
    float computeCorrelation() const noexcept;

  private:
    double m_sumL = 0.0;  // Σ L
    double m_sumR = 0.0;  // Σ R
    double m_sumLL = 0.0; // Σ L²
    double m_sumRR = 0.0; // Σ R²
    double m_sumLR = 0.0; // Σ L·R
    double m_sumMM = 0.0; // Σ (L+R)²  mid energy
    double m_sumSS = 0.0; // Σ (L-R)²  side energy
    long long m_count = 0;
    bool m_isStereo = false;
    int m_frontL = 0;
    int m_frontR = 1;
};

} // namespace khensu
