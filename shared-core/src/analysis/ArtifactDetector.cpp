// THREAD_CONTEXT(WorkerThread)
#include <khensu/analysis/ArtifactDetector.h>
#include <khensu/analysis/Percentile.h>
#include <algorithm>
#include <cmath>

namespace khensu {

void ArtifactDetector::reset(double sampleRate)
{
    m_clippingFrames = 0;
    m_clipRun.clear();
    m_lastCountedFrame = -1;
    m_blockSumSq = 0.0;
    m_blockCount = 0;
    m_totalSamples = 0;
    m_blockRmsDb.clear();
    m_noiseBlockSamples = std::max(1, static_cast<int>(sampleRate * 0.1));

    m_reversals = 0;
    m_monoSum = 0.0;
    m_dcBlockSamples = std::max(1, static_cast<int>(sampleRate));
    m_dcBlockSum = 0.0;
    m_dcBlockCount = 0;
    m_dcBlockMeans.clear();
    m_prevSample = 0.0f;
    m_prevDiff = 0.0f;
}

void ArtifactDetector::process(const float** channelData, int numChannels, int numSamples)
{
    if (channelData == nullptr || numChannels <= 0 || numSamples <= 0) return;
    if (static_cast<int>(m_clipRun.size()) < numChannels) m_clipRun.resize(static_cast<size_t>(numChannels), 0);

    for (int i = 0; i < numSamples; ++i) {
        // Frame-level clip counting (CORE-19). When a channel's run reaches
        // kMinClipRun, the frames it covers ([frame - kMinClipRun + 1, frame])
        // become clipped; each further over-threshold frame extends it by one.
        // The earliest frame confirmed by ANY channel this sample is tracked,
        // and only frames after the last counted one are added - taking the
        // per-channel max used to re-count frames another channel's run had
        // already counted.
        const long long frame = m_totalSamples;
        long long firstClippedFrame = -1; // -1 = no channel confirms this frame
        double blockSq = 0.0;
        double monoSample = 0.0;

        for (int c = 0; c < numChannels; ++c) {
            const float s = channelData[c][i];
            const float a = s < 0.0f ? -s : s;
            auto& run = m_clipRun[static_cast<size_t>(c)];
            if (a >= kClipThreshold && a <= kClipCeiling) {
                ++run;
                if (run >= kMinClipRun) {
                    const long long start = run == kMinClipRun ? frame - (kMinClipRun - 1) : frame;
                    firstClippedFrame = firstClippedFrame < 0 ? start : std::min(firstClippedFrame, start);
                }
            } else {
                run = 0;
            }
            const double sd = static_cast<double>(s);
            blockSq += sd * sd;
            monoSample += sd;
        }
        monoSample /= numChannels;
        m_monoSum += monoSample;
        m_dcBlockSum += monoSample;
        if (++m_dcBlockCount >= m_dcBlockSamples) {
            m_dcBlockMeans.push_back(static_cast<float>(m_dcBlockSum / static_cast<double>(m_dcBlockCount)));
            m_dcBlockSum = 0.0;
            m_dcBlockCount = 0;
        }

        if (firstClippedFrame >= 0) {
            const long long from = std::max(firstClippedFrame, m_lastCountedFrame + 1);
            if (frame >= from) m_clippingFrames += static_cast<int>(frame - from + 1);
            m_lastCountedFrame = frame;
        }

        m_blockSumSq += blockSq / numChannels; // mono-averaged
        ++m_blockCount;
        ++m_totalSamples;

        // Slope reversals of the mono signal (sample_reversal_rate)
        const float currentSample = static_cast<float>(monoSample);
        const float diff = currentSample - m_prevSample;
        if ((diff > 0.0f && m_prevDiff < 0.0f) || (diff < 0.0f && m_prevDiff > 0.0f)) {
            ++m_reversals;
        }

        m_prevSample = currentSample;
        m_prevDiff = diff;

        if (m_blockCount >= m_noiseBlockSamples) {
            const double rms = std::sqrt(m_blockSumSq / static_cast<double>(m_blockCount));
            if (rms > 1e-12) m_blockRmsDb.push_back(static_cast<float>(20.0 * std::log10(rms)));
            m_blockSumSq = 0.0;
            m_blockCount = 0;
        }
    }
}

AnalysisResult::ArtifactData ArtifactDetector::getResult() const
{
    AnalysisResult::ArtifactData result;
    result.clipping_frame_count = m_clippingFrames;
    result.has_clipping = (m_clippingFrames > 0);

    // Noise floor: 5th percentile of the per-100ms block RMS levels that sit
    // well below the programme (>= kNoiseBelowMedianDb under the median).
    // A dense master has no such blocks, and its quietest blocks are music,
    // not noise - report "nothing measurable" (-90 dBFS) instead of scoring
    // the music as noise. A file that is quiet throughout (median below
    // -40 dBFS, e.g. room tone or hiss alone) is measured directly.
    result.noise_floor_db = -90.0f;
    if (!m_blockRmsDb.empty()) {
        auto sorted = m_blockRmsDb;
        std::sort(sorted.begin(), sorted.end());
        const float median = sorted[sorted.size() / 2];
        auto percentile5 = [](const std::vector<float>& v) { return analysis::percentileSorted(v, 0.05); };
        std::vector<float> quiet;
        for (float db : sorted)
            if (db <= median - kNoiseBelowMedianDb) quiet.push_back(db);
        if (!quiet.empty()) {
            result.noise_floor_db = percentile5(quiet);
            result.noise_quiet_seconds = static_cast<float>(quiet.size()) * 0.1f;
            result.noise_floor_spread_db = analysis::percentileSorted(quiet, 0.5) - result.noise_floor_db;
        }
        else if (median < -40.0f) {
            result.noise_floor_db = percentile5(sorted);
            result.noise_quiet_seconds = static_cast<float>(sorted.size()) * 0.1f;
            result.noise_floor_spread_db = median - result.noise_floor_db;
        }
    }

    // DC offset: the mean of the mono signal over the whole file (MEAS-4).
    if (m_totalSamples > 0)
        result.dc_offset = static_cast<float>(m_monoSum / static_cast<double>(m_totalSamples));
    if (m_dcBlockMeans.size() >= 3) {
        auto sorted = m_dcBlockMeans;
        std::sort(sorted.begin(), sorted.end());
        const float p10 = analysis::percentileSorted(sorted, 0.10);
        const float p90 = analysis::percentileSorted(sorted, 0.90);
        result.dc_offset_steady = result.dc_offset >= 0.0f ? (p10 > 0.0f && p90 > 0.0f) : (p10 < 0.0f && p90 < 0.0f);
    }

    // Sample-reversal rate: reported as a raw, uncalibrated, informational
    // metric only (AUDIT-010). It used to drive an "AI artifacts" confidence
    // via (rate - 0.40) * 10, but the rate follows spectral slope and sample
    // rate, not generation - white 0.67, pink 0.58, a typical 1/f^1.5 mix 0.53
    // and brown noise 0.45 all read "100% AI". Until a detector is calibrated
    // on a labelled corpus nothing here claims AI, badges, or scores.
    if (m_totalSamples > 1000)
        result.sample_reversal_rate =
            static_cast<float>(static_cast<double>(m_reversals) / static_cast<double>(m_totalSamples));

    return result;
}

} // namespace khensu
