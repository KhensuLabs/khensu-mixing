// ITU-R BS.1770-4 K-weighted integrated loudness, LRA, and sample peak.
// THREAD_CONTEXT(WorkerThread)
#include <khensu/analysis/LoudnessAnalyzer.h>
#include <khensu/analysis/Percentile.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

static constexpr double kPi = 3.14159265358979323846;

namespace khensu {

// ---------------------------------------------------------------------------
// K-weighting: the coefficients live in KWeighting.cpp, the one definition
// shared with the plugin's live meters (FIX-11).
// ---------------------------------------------------------------------------

LoudnessAnalyzer::KWeightingCoefficients LoudnessAnalyzer::kWeightingCoefficients(double fs)
{
    return analysis::kWeightingCoefficients(fs);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// True peak (MEAS-5): the shared 4x interpolator in analysis/TruePeakFilter.h
// - scipy's resample_poly filter, the one the native meters and the Python
// service use. CORE-13's own 48-tap Kaiser(7.5) design read up to 0.08 dB
// apart from the service, and never measured the last 24 samples of a file.
namespace {

/// max |y| over the four interpolated values at the centre of `window`
/// (kTruePeakTaps samples, oldest..newest).
double interpolatedPeak(const double* window) noexcept
{
    const auto& f = analysis::truePeakPhases();
    double peak = 0.0;
    for (int p = 0; p < analysis::kTruePeakOversample; ++p) {
        const double* h = f.w[p];
        double val = 0.0;
        for (int tap = 0; tap < LoudnessAnalyzer::kTruePeakTaps; ++tap)
            val += window[tap] * h[tap];
        peak = std::max(peak, std::abs(val));
    }
    return peak;
}

} // namespace

void LoudnessAnalyzer::reset(double sampleRate, const ChannelLayout& layout)
{
    m_sampleRate = sampleRate;
    m_numChannels = std::min(layout.numChannels, kMaxChannels);
    m_blockTarget = static_cast<int>(sampleRate * 0.1 + 0.5); // 100ms
    m_blockCount = 0;
    m_blockSumSq = 0.0;
    m_peakSample = 0.0;
    (void)analysis::truePeakPhases(); // build the filter here, not mid-stream
    m_blockPowers.clear();

    for (int c = 0; c < kMaxChannels; ++c) {
        m_peakHistory[c].fill(0.0);
        m_peakWriteIdx[c] = 0;
        m_peakSeen[static_cast<size_t>(c)] = 0;
        m_weights[static_cast<size_t>(c)] = layout.bs1770Weight(c);
    }

    const auto kWeighting = analysis::kWeightingCoefficients(sampleRate);
    for (int c = 0; c < m_numChannels; ++c) {
        m_pre[c].set(kWeighting.preFilter);
        m_rlb[c].set(kWeighting.rlbFilter);
    }
}

void LoudnessAnalyzer::reset(double sampleRate, int numChannels)
{
    auto layout = ChannelLayout::fromChannelCount(numChannels);
    reset(sampleRate, layout.is_ok() ? layout.value() : ChannelLayout::unspecified(numChannels));
}

void LoudnessAnalyzer::process(const float** channelData, int numChannels, int numSamples)
{
    // Not reset() yet (m_blockTarget 0) would spin forever below.
    if (channelData == nullptr || numChannels <= 0 || numSamples <= 0 || m_blockTarget <= 0) return;
    const int ch = std::min(numChannels, m_numChannels);
    int offset = 0;
    while (offset < numSamples) {
        const int remaining = m_blockTarget - m_blockCount;
        const int toProcess = std::min(numSamples - offset, remaining);

        for (int i = 0; i < toProcess; ++i) {
            for (int c = 0; c < ch; ++c) {
                const float s = channelData[c][offset + i];

                // Track true peak (MEAS-5). The history is stored twice (idx
                // and idx + N) so the newest N samples are always one
                // contiguous run - no modulo in the inner loop, which the
                // compiler can vectorise. The window's centre is the sample
                // kTruePeakHalf behind the newest; before the first sample
                // reaches the centre there is nothing to interpolate.
                constexpr int N = kTruePeakTaps;
                auto& hist = m_peakHistory[c];
                const int wIdx = m_peakWriteIdx[c];
                hist[static_cast<size_t>(wIdx)] = static_cast<double>(s);
                hist[static_cast<size_t>(wIdx + N)] = static_cast<double>(s);
                const int nextIdx = (wIdx + 1) % N;
                m_peakWriteIdx[c] = nextIdx;
                m_peakSample = std::max(m_peakSample, static_cast<double>(std::abs(s)));
                if (++m_peakSeen[static_cast<size_t>(c)] > analysis::kTruePeakHalf)
                    m_peakSample = std::max(m_peakSample, interpolatedPeak(hist.data() + nextIdx));

                // K-weighting chain. The BS.1770 channel weight is power-domain,
                // so it multiplies the squared sample - never the amplitude.
                double k = m_pre[c].process(static_cast<double>(s));
                k = m_rlb[c].process(k);
                m_blockSumSq += m_weights[static_cast<size_t>(c)] * k * k;
            }
        }

        m_blockCount += toProcess;
        offset += toProcess;

        if (m_blockCount >= m_blockTarget) flushBlock();
    }
}

void LoudnessAnalyzer::flushBlock()
{
    if (m_blockCount > 0) {
        // Mean square summed over channels (ITU-R sum, not average, for channel-summed loudness)
        m_blockPowers.push_back(m_blockSumSq / static_cast<double>(m_blockCount));
    }
    m_blockCount = 0;
    m_blockSumSq = 0.0;
}

AnalysisResult::LoudnessData LoudnessAnalyzer::getResult() const
{
    AnalysisResult::LoudnessData result;
    // The last kTruePeakHalf samples of each channel have not been the centre
    // of a window yet: finish them against zeros, as resample_poly does.
    double peak = m_peakSample;
    constexpr int N = kTruePeakTaps;
    constexpr int H = analysis::kTruePeakHalf;
    for (int c = 0; c < m_numChannels; ++c) {
        const long long seen = m_peakSeen[static_cast<size_t>(c)];
        if (seen <= 0) continue;
        std::array<double, N + H> tail{};
        const double* newest = m_peakHistory[c].data() + m_peakWriteIdx[c]; // oldest .. newest
        std::copy(newest, newest + N, tail.begin());
        // Centre `seen - H + k` sits at tail index k + 1 (its window starts there).
        const long long first = std::max<long long>(0, seen - H);
        for (long long centre = first; centre < seen; ++centre)
            peak = std::max(peak, interpolatedPeak(tail.data() + (centre - seen + H + 1)));
    }
    result.true_peak_dbtp = static_cast<float>(analysis::peakToDb(peak));

    // Flush partial trailing block
    std::vector<double> blocks = m_blockPowers;
    if (m_blockCount > 0) {
        blocks.push_back(m_blockSumSq / static_cast<double>(m_blockCount));
    }

    if (blocks.empty()) {
        result.lufs_integrated = -70.0f;
        result.lufs_short_term_max = -70.0f;
        result.lra = 0.0f;
        result.plr = 0.0f;
        return result;
    }

    // --- Integrated LUFS (gated) ---
    // 400ms windows = 4 x 100ms blocks with 1-block step
    const int kBlocks400 = 4;
    const double kAbsGate = std::pow(10.0, (-70.0 + 0.691) / 10.0);

    std::vector<double> windows400;
    if (static_cast<int>(blocks.size()) >= kBlocks400) {
        for (int i = kBlocks400 - 1; i < static_cast<int>(blocks.size()); ++i) {
            double sum = 0.0;
            for (int j = 0; j < kBlocks400; ++j)
                sum += blocks[i - j];
            windows400.push_back(sum / kBlocks400);
        }
    } else {
        // Short file: use entire content as one window
        double sum = 0.0;
        for (double b : blocks)
            sum += b;
        windows400.push_back(sum / blocks.size());
    }

    // Absolute gate pass
    double absGatedSum = 0.0;
    int absGatedN = 0;
    for (double w : windows400) {
        if (w >= kAbsGate) {
            absGatedSum += w;
            ++absGatedN;
        }
    }

    if (absGatedN == 0) {
        result.lufs_integrated = -70.0f;
    } else {
        const double absGatedMean = absGatedSum / absGatedN;
        const double relGate = absGatedMean * std::pow(10.0, -10.0 / 10.0);

        double relGatedSum = 0.0;
        int relGatedN = 0;
        for (double w : windows400) {
            if (w >= kAbsGate && w >= relGate) {
                relGatedSum += w;
                ++relGatedN;
            }
        }
        if (relGatedN == 0) {
            result.lufs_integrated = -70.0f;
        } else {
            result.lufs_integrated = static_cast<float>(-0.691 + 10.0 * std::log10(relGatedSum / relGatedN));
        }
    }

    // --- Short-term LUFS (3000ms = 30 x 100ms blocks) ---
    const int kBlocks3s = 30;
    std::vector<float> shortTerm;
    if (static_cast<int>(blocks.size()) >= kBlocks3s) {
        for (int i = kBlocks3s - 1; i < static_cast<int>(blocks.size()); ++i) {
            double sum = 0.0;
            for (int j = 0; j < kBlocks3s; ++j)
                sum += blocks[i - j];
            const double power = sum / kBlocks3s;
            if (power > 1e-15) {
                shortTerm.push_back(static_cast<float>(-0.691 + 10.0 * std::log10(power)));
            }
        }
    }

    if (!shortTerm.empty()) {
        result.lufs_short_term_max = *std::max_element(shortTerm.begin(), shortTerm.end());
    } else {
        // File shorter than 3s - use integrated as proxy
        result.lufs_short_term_max = result.lufs_integrated;
    }

    // --- LRA (EBU Tech 3342 Gated Algorithm) ---
    // 1. Apply absolute gate (-70 LUFS)
    std::vector<float> absGatedST;
    double sumPower = 0.0;
    for (float st : shortTerm) {
        if (st >= -70.0f) {
            absGatedST.push_back(st);
            sumPower += std::pow(10.0, (st + 0.691) / 10.0);
        }
    }

    if (absGatedST.size() >= 2) {
        // 2. Compute relative threshold (-20 dB relative to absolute gated average power)
        double meanPower = sumPower / absGatedST.size();
        float refLoudness = static_cast<float>(-0.691f + 10.0f * std::log10(meanPower));
        float relGateThreshold = refLoudness - 20.0f;

        // 3. Apply relative gate
        std::vector<float> relGatedST;
        for (float st : absGatedST) {
            if (st >= relGateThreshold) {
                relGatedST.push_back(st);
            }
        }

        if (relGatedST.size() >= 2) {
            std::sort(relGatedST.begin(), relGatedST.end());
            // Shared nearest-rank rule (analysis/Percentile.h) - Python matches it.
            const float p10 = analysis::percentileSorted(relGatedST, 0.10);
            const float p95 = analysis::percentileSorted(relGatedST, 0.95);
            result.lra = std::max(0.0f, p95 - p10);
        } else {
            result.lra = 0.0f;
            result.lra_measured = false;
        }
    } else {
        result.lra = 0.0f;
        result.lra_measured = false;
    }

    // --- PLR: True peak - Integrated LUFS ---
    result.plr = result.true_peak_dbtp - result.lufs_integrated;

    return result;
}

} // namespace khensu
