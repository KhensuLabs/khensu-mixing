// THREAD_CONTEXT(WorkerThread)
#include <khensu/analysis/StereoAnalyzer.h>
#include <cmath>
#include <algorithm>

namespace khensu {

void StereoAnalyzer::reset()
{
    reset(ChannelLayout::fromChannelCount(2).value());
}

void StereoAnalyzer::reset(const ChannelLayout& layout)
{
    m_sumL = m_sumR = 0.0;
    m_sumLL = m_sumRR = m_sumLR = 0.0;
    m_sumMM = m_sumSS = 0.0;
    m_count = 0;
    m_isStereo = false;
    m_frontL = layout.frontLeft();
    m_frontR = layout.frontRight();
}

void StereoAnalyzer::process(const float** channelData, int numChannels, int numSamples)
{
    if (numChannels < 2) {
        // Mono - fill both sides with L
        for (int i = 0; i < numSamples; ++i) {
            const double s = static_cast<double>(channelData[m_frontL][i]);
            m_sumL += s;
            m_sumR += s;
            m_sumLL += s * s;
            m_sumRR += s * s;
            m_sumLR += s * s;
            m_sumMM += (2.0 * s) * (2.0 * s);
            m_sumSS += 0.0; // L==R, side = 0
            ++m_count;
        }
    } else {
        m_isStereo = true;
        for (int i = 0; i < numSamples; ++i) {
            const double L = static_cast<double>(channelData[m_frontL][i]);
            const double R = static_cast<double>(channelData[m_frontR][i]);
            const double M = L + R;
            const double S = L - R;
            m_sumL += L;
            m_sumR += R;
            m_sumLL += L * L;
            m_sumRR += R * R;
            m_sumLR += L * R;
            m_sumMM += M * M;
            m_sumSS += S * S;
            ++m_count;
        }
    }
}

// ---------------------------------------------------------------------------
// Private helper: unbiased Pearson L/R correlation.
// Shared by getResult() and getPhaseResult() - avoids duplicated computation.
// ---------------------------------------------------------------------------
float StereoAnalyzer::computeCorrelation() const noexcept
{
    if (m_count == 0) return 1.0f;
    const double N = static_cast<double>(m_count);
    const double varL = std::max(0.0, m_sumLL - (m_sumL * m_sumL) / N);
    const double varR = std::max(0.0, m_sumRR - (m_sumR * m_sumR) / N);
    const double cov = m_sumLR - (m_sumL * m_sumR) / N;
    // Same silence policy as Python dsp_metrics.stereo_correlation (CORE-08):
    // std < 1e-9 counts as silent. Both silent -> 1 (nothing can cancel in
    // mono); exactly one silent -> 0 (no common signal - a hard-panned part
    // used to read 1.0 next to width 1.0, a contradictory pair).
    constexpr double kSilentVar = 1e-18; // (1e-9)^2, per-sample variance
    const bool silentL = varL / N < kSilentVar;
    const bool silentR = varR / N < kSilentVar;
    if (silentL && silentR) return 1.0f;
    if (silentL || silentR) return 0.0f;
    const double denom = std::sqrt(varL * varR);
    return denom > 1e-20 ? static_cast<float>(std::max(-1.0, std::min(1.0, cov / denom))) : 1.0f;
}

AnalysisResult::StereoData StereoAnalyzer::getResult() const
{
    AnalysisResult::StereoData result;
    if (m_count == 0) return result;

    // Centered Pearson correlation via shared helper.
    result.correlation = computeCorrelation();

    // Stereo width = clamp(2*S^2 / (M^2 + S^2), 0, 1), M/S energies.
    // 0 = mono, 1.0 = uncorrelated equal-power L/R (as wide as a normal mix
    // gets without anti-phase); anti-phase content clamps at 1. The old
    // S^2/(M^2+S^2) topped out at 0.5 for uncorrelated material, so genre
    // targets of 0.5-0.7 were only reachable with phase problems and
    // "too narrow" fired on almost every real mix. The Python service uses
    // the identical definition (analysis/dsp_metrics stereo width).
    const double totalMS = m_sumMM + m_sumSS;
    result.width = totalMS > 1e-20 ? static_cast<float>(std::min(1.0, 2.0 * m_sumSS / totalMS)) : 0.0f;

    // M/S balance: positive = mid-heavy, negative = side-heavy
    // Normalised: (M-S)/(M+S), 0 = balanced
    result.ms_balance = totalMS > 1e-20 ? static_cast<float>((m_sumMM - m_sumSS) / totalMS) : 0.0f;

    return result;
}

AnalysisResult::PhaseData StereoAnalyzer::getPhaseResult() const
{
    AnalysisResult::PhaseData result;
    if (m_count == 0 || !m_isStereo) {
        result.overall_correlation = 1.0f;
        result.has_polarity_issue = false;
        result.has_anti_phase_energy = false;
        result.has_comb_filtering = false;
        return result;
    }

    // Reuse shared correlation helper - no duplicated math.
    result.overall_correlation = computeCorrelation();
    result.has_polarity_issue = (result.overall_correlation < AnalysisResult::PhaseData::kPhaseIssueCorrelation);

    // Mid power versus summed L+R power: L==R (mono) yields 2.0, uncorrelated
    // 1.0, and a pair with substantial anti-phase energy < 0.6 (the mono
    // fold-down loses level). An anti-correlation test (CORE-09), reported
    // under its honest name; has_comb_filtering is the deprecated alias.
    const double ratio = m_sumMM / (m_sumLL + m_sumRR + 1e-12);
    result.has_anti_phase_energy = (ratio < 0.6);
    result.has_comb_filtering = result.has_anti_phase_energy;

    return result;
}

} // namespace khensu
