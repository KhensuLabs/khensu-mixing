// THREAD_CONTEXT(WorkerThread)
#include <khensu/analysis/DynamicsAnalyzer.h>
#include <khensu/analysis/Percentile.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace khensu {

void DynamicsAnalyzer::reset(double sampleRate) { reset(sampleRate, ChannelLayout::unspecified(kMaxChannels)); }

void DynamicsAnalyzer::reset(double sampleRate, const ChannelLayout& layout)
{
    m_sumSq = 0.0;
    m_peak = 0.0f;
    m_count = 0;
    m_chSumSq.fill(0.0);
    m_chPeak.fill(0.0f);
    m_chCount.fill(0);
    m_isLfe.fill(false);
    for (int c = 0; c < std::min(layout.numChannels, kMaxChannels); ++c)
        m_isLfe[static_cast<size_t>(c)] = layout.isLFE(c);
    m_blockSumSq = 0.0;
    m_blockCount = 0;
    m_blockRmsDb.clear();
    m_blockSizeSamples = std::max(1, static_cast<int>(sampleRate * 0.1));
}

void DynamicsAnalyzer::process(const float** channelData, int numChannels, int numSamples)
{
    if (channelData == nullptr || numChannels <= 0 || numSamples <= 0) return;
    const int tracked = std::min(numChannels, kMaxChannels);
    int programmeChannels = 0;
    for (int c = 0; c < tracked; ++c)
        if (!m_isLfe[static_cast<size_t>(c)]) ++programmeChannels;
    for (int i = 0; i < numSamples; ++i) {
        double monoSq = 0.0;
        for (int c = 0; c < tracked; ++c) {
            if (m_isLfe[static_cast<size_t>(c)]) continue; // LFE is effects content, not programme
            const float s = channelData[c][i];
            const float a = s < 0.0f ? -s : s;
            if (a > m_peak) m_peak = a;
            const double sd = static_cast<double>(s);
            m_sumSq += sd * sd;
            monoSq += sd * sd;
            ++m_count;
            if (a > m_chPeak[static_cast<size_t>(c)]) m_chPeak[static_cast<size_t>(c)] = a;
            m_chSumSq[static_cast<size_t>(c)] += sd * sd;
            ++m_chCount[static_cast<size_t>(c)];
        }
        // Per-block mono-averaged power for macro DR
        m_blockSumSq += monoSq / std::max(programmeChannels, 1);
        ++m_blockCount;

        if (m_blockCount >= m_blockSizeSamples) {
            const double rms = std::sqrt(m_blockSumSq / static_cast<double>(m_blockCount));
            if (rms > 1e-10) m_blockRmsDb.push_back(static_cast<float>(20.0 * std::log10(rms)));
            m_blockSumSq = 0.0;
            m_blockCount = 0;
        }
    }
}

AnalysisResult::DynamicsData DynamicsAnalyzer::getResult() const
{
    AnalysisResult::DynamicsData result;
    if (m_count == 0) return result;

    // Overall (pooled programme-channel) RMS - the reference for the
    // block-based peak_to_rms_db below.
    const double rms = std::sqrt(m_sumSq / static_cast<double>(m_count));
    const float rmsDb = rms > 1e-10 ? static_cast<float>(20.0 * std::log10(rms)) : -90.0f;

    // Crest factor: the LARGEST per-channel sample-peak / RMS over non-silent,
    // non-LFE channels - the loudness.crest_factor_db definition (AUDIT-053).
    // Pooling RMS across channels read +3.01 dB high when one side was silent
    // and let the LFE leak in.
    const double rmsFloor = std::pow(10.0, kCrestRmsFloorDbfs / 20.0);
    bool anyChannel = false;
    double bestCrest = 0.0;
    for (int c = 0; c < kMaxChannels; ++c) {
        const auto ci = static_cast<size_t>(c);
        if (m_chCount[ci] == 0) continue;
        const double chRms = std::sqrt(m_chSumSq[ci] / static_cast<double>(m_chCount[ci]));
        if (chRms <= rmsFloor) continue; // noise over noise
        const double crest = 20.0 * std::log10(static_cast<double>(m_chPeak[ci]) / chRms);
        if (!anyChannel || crest > bestCrest) bestCrest = crest;
        anyChannel = true;
    }
    result.crest_factor_db = anyChannel ? static_cast<float>(bestCrest) : 0.0f;

    // peak_to_rms_db: loudest 100ms block RMS vs. global RMS.
    // Captures "how loud the loudest moment feels" vs. the average -
    // distinct from crest_factor_db which tracks instantaneous peaks.
    if (!m_blockRmsDb.empty()) {
        const float maxBlockRmsDb = *std::max_element(m_blockRmsDb.begin(), m_blockRmsDb.end());
        result.peak_to_rms_db = maxBlockRmsDb - rmsDb; // loudest 100ms window above average
    } else {
        result.peak_to_rms_db = result.crest_factor_db; // fallback for very short files
    }

    // Macro dynamic range: EBU DR-style P95 - P10 of per-100ms block RMS.
    // Captures track-level loudness variation, distinct from crest factor.
    auto blocks = m_blockRmsDb;
    // Include partial trailing block if substantial enough
    if (m_blockCount >= m_blockSizeSamples / 4) {
        const double rmsB = std::sqrt(m_blockSumSq / static_cast<double>(m_blockCount));
        if (rmsB > 1e-10) blocks.push_back(static_cast<float>(20.0 * std::log10(rmsB)));
    }

    if (blocks.size() >= 5) {
        std::sort(blocks.begin(), blocks.end());
        const float p10 = analysis::percentileSorted(blocks, 0.10);
        const float p95 = analysis::percentileSorted(blocks, 0.95);
        result.dynamic_range_db = std::max(0.0f, p95 - p10);
    } else {
        // Short file: fall back to crest factor as DR proxy
        result.dynamic_range_db = result.crest_factor_db;
    }

    return result;
}

} // namespace khensu
