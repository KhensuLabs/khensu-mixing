#include "LoudnessStats.h"
#include <khensu/analysis/KWeighting.h>

#include <cmath>

namespace khensu::copilot::reference {

namespace {
constexpr double kLoudnessOffset = -0.691;

double loudnessOf(double meanSquare) noexcept
{
    return meanSquare > 1.0e-12 ? kLoudnessOffset + 10.0 * std::log10(meanSquare) : -120.0;
}

void load(std::array<double, 5> c, double& b0, double& b1, double& b2, double& a1, double& a2) noexcept
{
    b0 = c[0];
    b1 = c[1];
    b2 = c[2];
    a1 = c[3];
    a2 = c[4];
}
} // namespace

LoudnessStats::LoudnessStats() noexcept
{
    for (int b = 0; b < kBins; ++b)
        binPower_[static_cast<size_t>(b)] = std::pow(10.0, (kFloorLufs + (b + 0.5) * kBinLu - kLoudnessOffset) / 10.0);
    prepare(48000.0);
}

void LoudnessStats::prepare(double sampleRate) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    blockLength_ = juce::jmax(1, static_cast<int>(std::lround(sampleRate_ * 0.1)));
    const auto k = analysis::kWeightingCoefficients(sampleRate_);
    for (auto* set : {&preTemplate_, &rlbTemplate_})
        for (auto& bq : *set)
            bq = Biquad{};
    for (auto& bq : preTemplate_)
        load(k.preFilter, bq.b0, bq.b1, bq.b2, bq.a1, bq.a2);
    for (auto& bq : rlbTemplate_)
        load(k.rlbFilter, bq.b0, bq.b1, bq.b2, bq.a1, bq.a2);
    reset();
}

void LoudnessStats::reset() noexcept
{
    pre_ = preTemplate_;
    rlb_ = rlbTemplate_;
    blockFill_ = 0;
    blockPower_ = blockRawSq_ = 0.0;
    blockRawCount_ = 0;
    recent_.fill(0.0);
    recentPos_ = recentCount_ = 0;
    for (auto& bin : momentaryHist_)
        bin.store(0, std::memory_order_relaxed);
    for (auto& bin : shortTermHist_)
        bin.store(0, std::memory_order_relaxed);
    gatedRawSq_.store(0.0, std::memory_order_relaxed);
    gatedRawCount_.store(0.0, std::memory_order_relaxed);
    peak_.store(0.0f, std::memory_order_relaxed);
    shortTermLufs_.store(-96.0f, std::memory_order_relaxed);
    integratedLufs_.store(-96.0f, std::memory_order_relaxed);
    resetRequested_.store(false, std::memory_order_relaxed);
}

int LoudnessStats::binOf(double lufs) noexcept
{
    if (!(lufs > kFloorLufs)) return -1;
    const int bin = static_cast<int>((lufs - kFloorLufs) / kBinLu);
    return juce::jlimit(0, kBins - 1, bin);
}

double LoudnessStats::integratedFromHistogram() const noexcept
{
    // BS.1770: absolute gate (the histogram floor), then -10 LU relative.
    double count = 0.0, power = 0.0;
    for (int b = 0; b < kBins; ++b) {
        const double c = momentaryHist_[static_cast<size_t>(b)].load(std::memory_order_relaxed);
        count += c;
        power += c * binPower_[static_cast<size_t>(b)];
    }
    if (count <= 0.0) return -96.0;
    const double relGate = loudnessOf(power / count) - 10.0;
    double gCount = 0.0, gPower = 0.0;
    for (int b = 0; b < kBins; ++b) {
        if (kFloorLufs + (b + 0.5) * kBinLu < relGate) continue;
        const double c = momentaryHist_[static_cast<size_t>(b)].load(std::memory_order_relaxed);
        gCount += c;
        gPower += c * binPower_[static_cast<size_t>(b)];
    }
    return gCount > 0.0 ? loudnessOf(gPower / gCount) : -96.0;
}

void LoudnessStats::process(const float* const* channels, int numChannels, int numSamples) noexcept
{
    if (resetRequested_.load(std::memory_order_acquire)) reset();
    const int chans = juce::jlimit(0, 2, numChannels);
    if (chans == 0 || numSamples <= 0 || channels == nullptr) return;
    float peak = peak_.load(std::memory_order_relaxed);
    for (int i = 0; i < numSamples; ++i) {
        for (int c = 0; c < chans; ++c) {
            const float x = channels[c][i];
            const double weighted = rlb_[static_cast<size_t>(c)].process(pre_[static_cast<size_t>(c)].process(x));
            blockPower_ += weighted * weighted;
            blockRawSq_ += static_cast<double>(x) * x;
            peak = juce::jmax(peak, std::abs(x));
        }
        blockRawCount_ += chans;
        if (++blockFill_ >= blockLength_) finishBlock();
    }
    // A non-finite input would poison the filters for good.
    for (int c = 0; c < 2; ++c)
        if (!std::isfinite(pre_[static_cast<size_t>(c)].z1) || !std::isfinite(rlb_[static_cast<size_t>(c)].z1)) {
            pre_ = preTemplate_;
            rlb_ = rlbTemplate_;
            blockPower_ = blockRawSq_ = 0.0;
        }
    if (std::isfinite(peak)) peak_.store(peak, std::memory_order_relaxed);
}

void LoudnessStats::finishBlock() noexcept
{
    const double meanSquare = blockPower_ / static_cast<double>(blockLength_);
    recent_[static_cast<size_t>(recentPos_)] = meanSquare;
    recentPos_ = (recentPos_ + 1) % kShortTermBlocks;
    recentCount_ = juce::jmin(recentCount_ + 1, kShortTermBlocks);

    const auto meanOfLatest = [this](int count) {
        double sum = 0.0;
        for (int k = 1; k <= count; ++k)
            sum += recent_[static_cast<size_t>((recentPos_ - k + kShortTermBlocks) % kShortTermBlocks)];
        return sum / count;
    };

    if (recentCount_ >= kMomentaryBlocks) {
        const double momentary = loudnessOf(meanOfLatest(kMomentaryBlocks));
        const int bin = binOf(momentary);
        if (bin >= 0) {
            momentaryHist_[static_cast<size_t>(bin)].fetch_add(1, std::memory_order_relaxed);
            integratedLufs_.store(static_cast<float>(integratedFromHistogram()), std::memory_order_relaxed);
            // Crest RMS only over audible blocks (silence between takes would
            // otherwise lower it).
            gatedRawSq_.store(gatedRawSq_.load(std::memory_order_relaxed) + blockRawSq_, std::memory_order_relaxed);
            gatedRawCount_.store(gatedRawCount_.load(std::memory_order_relaxed) + blockRawCount_,
                                 std::memory_order_relaxed);
        }
    }
    if (recentCount_ >= kShortTermBlocks) {
        const double shortTerm = loudnessOf(meanOfLatest(kShortTermBlocks));
        shortTermLufs_.store(static_cast<float>(juce::jmax(-96.0, shortTerm)), std::memory_order_relaxed);
        const int bin = binOf(shortTerm);
        if (bin >= 0) shortTermHist_[static_cast<size_t>(bin)].fetch_add(1, std::memory_order_relaxed);
    } else if (recentCount_ >= kMomentaryBlocks) {
        // Before 3 s exist the latest blocks are the best short-term estimate.
        shortTermLufs_.store(static_cast<float>(juce::jmax(-96.0, loudnessOf(meanOfLatest(recentCount_)))),
                             std::memory_order_relaxed);
    }
    blockFill_ = 0;
    blockPower_ = blockRawSq_ = 0.0;
    blockRawCount_ = 0;
}

LoudnessStats::Result LoudnessStats::result() const noexcept
{
    Result r;
    r.shortTermLufs = shortTermLufs_.load(std::memory_order_relaxed);
    const float peak = peak_.load(std::memory_order_relaxed);
    r.peakDb = peak > 1.0e-6f ? 20.0f * std::log10(peak) : -96.0f;

    r.integratedLufs = integratedLufs();
    r.valid = r.integratedLufs > -90.0f;
    double count = 0.0, power = 0.0;

    // Loudness range (EBU Tech 3342).
    std::array<juce::uint32, kBins> s{};
    for (int b = 0; b < kBins; ++b) {
        s[static_cast<size_t>(b)] = shortTermHist_[static_cast<size_t>(b)].load(std::memory_order_relaxed);
        count += s[static_cast<size_t>(b)];
        power += s[static_cast<size_t>(b)] * binPower_[static_cast<size_t>(b)];
    }
    if (count > 0.0) {
        const double relGate = loudnessOf(power / count) - 20.0;
        double gated = 0.0;
        for (int b = 0; b < kBins; ++b)
            if (kFloorLufs + (b + 0.5) * kBinLu >= relGate) gated += s[static_cast<size_t>(b)];
        if (gated > 0.0) {
            const auto percentile = [&](double p) {
                const double target = p * gated;
                double seen = 0.0;
                for (int b = 0; b < kBins; ++b) {
                    const double centre = kFloorLufs + (b + 0.5) * kBinLu;
                    if (centre < relGate) continue;
                    seen += s[static_cast<size_t>(b)];
                    if (seen >= target) return centre;
                }
                return static_cast<double>(kTopLufs);
            };
            r.loudnessRangeLu = static_cast<float>(juce::jmax(0.0, percentile(0.95) - percentile(0.10)));
        }
    }

    const double rawCount = gatedRawCount_.load(std::memory_order_relaxed);
    const double rawSq = gatedRawSq_.load(std::memory_order_relaxed);
    if (rawCount > 0.0 && rawSq > 0.0 && peak > 0.0f) {
        const double rmsDb = 10.0 * std::log10(rawSq / rawCount);
        r.crestDb = static_cast<float>(juce::jmax(0.0, static_cast<double>(r.peakDb) - rmsDb));
    }
    return r;
}

} // namespace khensu::copilot::reference
