#include "CopilotOutputStage.h"

#include <cmath>

namespace khensu {

namespace {
constexpr double kMatchTimeConstantSec = 3.0; // ~short-term loudness window
constexpr float kMaxMatchDb = 12.0f;
constexpr double kSilenceMs = 1.0e-10; // ~-100 dBFS: don't match against silence
// Ceiling guard (FIX-12): how long a peak of the dry / delta side keeps
// setting that side's trim, and how slowly the trim lets go afterwards. Both
// as long as the level match itself, so the trim sits still through a phrase
// instead of pumping with every hit.
constexpr double kPeakHoldSeconds = 3.0;
constexpr double kPeakReleaseSeconds = 3.0;
} // namespace

float CopilotOutputStage::measureUpsampleDelay(int blockSize)
{
    // The detector's delay is a property of JUCE's FIR design, which differs
    // between the up and down halves and is not exposed per direction. Measure
    // it once: the upsampled response of a unit impulse peaks at the group
    // delay (linear-phase FIRs). Message thread / prepare only.
    if (oversampler_ == nullptr) return 0.0f;
    constexpr int kProbeSamples = 512;
    const int chunk = juce::jmax(1, juce::jmin(blockSize, 64));
    juce::AudioBuffer<float> probe(preparedChannels_, chunk);
    const auto factor = static_cast<int>(oversampler_->getOversamplingFactor());
    float best = 0.0f;
    long long bestIndex = 0;
    oversampler_->reset();
    for (int start = 0; start < kProbeSamples; start += chunk) {
        probe.clear();
        if (start == 0)
            for (int c = 0; c < preparedChannels_; ++c) probe.setSample(c, 0, 1.0f);
        juce::dsp::AudioBlock<const float> block(probe);
        auto up = oversampler_->processSamplesUp(block);
        for (size_t i = 0; i < up.getNumSamples(); ++i) {
            const float v = std::abs(up.getSample(0, static_cast<int>(i)));
            if (v > best) {
                best = v;
                bestIndex = static_cast<long long>(start) * factor + static_cast<long long>(i);
            }
        }
    }
    oversampler_->reset();
    return static_cast<float>(static_cast<double>(bestIndex) / static_cast<double>(juce::jmax(1, factor)));
}

void CopilotOutputStage::prepareToPlay(double sampleRate, int samplesPerBlock, int preparedChannels)
{
    preparedChannels_ = juce::jlimit(1, 2, preparedChannels);
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    const int blockSize = juce::jmax(1, samplesPerBlock);

    lookaheadSamples_ = juce::jmax(1, static_cast<int>(std::round(sampleRate_ * 0.0015)));
    brickwall_.prepare(sampleRate_, preparedChannels_, lookaheadSamples_);

    oversampler_ = std::make_unique<juce::dsp::Oversampling<float>>(
        static_cast<size_t>(preparedChannels_), 2, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true);
    oversampler_->initProcessing(static_cast<size_t>(blockSize));
    detectorDelay_ = measureUpsampleDelay(blockSize);
    alignSamples_ = juce::jmax(1, static_cast<int>(std::ceil(detectorDelay_)));
    alignRing_.setSize(preparedChannels_, alignSamples_);
    alignRing_.clear();
    alignPos_ = 0;
    previousUpsampledPeak_ = 0.0f;

    upsampledPeak_.assign(static_cast<size_t>(blockSize), 0.0f);
    detectorPeak_.assign(static_cast<size_t>(blockSize), 0.0f);
    abDryBuffer_.setSize(preparedChannels_, blockSize);
    abDryBuffer_.clear();
    // One second of hosted latency up front; reserveHostedLatency() grows it.
    abDryDelay_.prepare(sampleRate_, blockSize, preparedChannels_,
                        latencySamples() + static_cast<int>(std::ceil(sampleRate_)) + 8);

    wetMs_ = dryMs_ = 0.0;
    matchGain_ = 1.0f;
    matchGainDb_.store(0.0f, std::memory_order_relaxed);
    peakHoldSamples_ = juce::jmax(1, static_cast<int>(std::round(sampleRate_ * kPeakHoldSeconds)));
    peakRelease_ = std::exp(-1.0 / (kPeakReleaseSeconds * sampleRate_));
    dryPeak_.reset();
    deltaPeak_.reset();
    monitorTrimDb_.store(0.0f, std::memory_order_relaxed);
    lastAbMode_ = 0;

    fadeSamples_ = juce::jmax(1, static_cast<int>(std::round(sampleRate_ * kSwitchFadeSeconds)));
    listenFrom_ = listenTo_ = Listen::Processed;
    listenFadePos_ = fadeSamples_;
}

void CopilotOutputStage::resetState() noexcept
{
    brickwall_.reset();
    abDryDelay_.reset();
    abDryBuffer_.clear();
    alignRing_.clear();
    alignPos_ = 0;
    previousUpsampledPeak_ = 0.0f;
    if (oversampler_ != nullptr) oversampler_->reset();
    wetMs_ = dryMs_ = 0.0;
    matchGain_ = 1.0f;
    matchGainDb_.store(0.0f, std::memory_order_relaxed);
    dryPeak_.reset();
    deltaPeak_.reset();
    monitorTrimDb_.store(0.0f, std::memory_order_relaxed);
}

void CopilotOutputStage::captureDry(const juce::AudioBuffer<float>& buffer, int /*abMode*/)
{
    // Always captured (not only while A/B is active): the dry delay line must
    // stay warm, otherwise switching to Original/Delta replayed stale audio
    // from the last A/B session, and bypass needs it for latency alignment.
    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0 || numSamples > abDryBuffer_.getNumSamples()) return;

    const int channels = juce::jmin(buffer.getNumChannels(), preparedChannels_);
    for (int c = 0; c < channels; ++c) abDryBuffer_.copyFrom(c, 0, buffer, c, 0, numSamples);
}

void CopilotOutputStage::process(juce::AudioBuffer<float>& buffer, bool globalBypass, bool limiterOn,
                                 float limiterThresholdDb, int abMode, int activeHostedLatency,
                                 int reportedHostedLatency)
{
    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    if (numSamples <= 0) return;

    // Fully bypassed (fade finished): the chain was skipped upstream.
    const bool bypassSettled = !needsProcessedPath(globalBypass);

    // The delays always run so the reported latency is constant; bypass only
    // disables gain reduction. Oversampled detection approximates dBTP.
    const bool limiterEnabled = limiterOn && !bypassSettled;
    const float ceiling = juce::Decibels::decibelsToGain(limiterThresholdDb);
    const int channels = juce::jmin(numChannels, preparedChannels_);
    const bool detectorReady = oversampler_ != nullptr && channels > 0
                               && numSamples <= static_cast<int>(detectorPeak_.size());

    // 1. Upsampled peak per base-rate sample. Runs every block, also with the
    //    limiter off, so the oversampler never holds stale state (CORE-02).
    if (detectorReady) {
        juce::dsp::AudioBlock<const float> inputBlock(buffer.getArrayOfReadPointers(),
                                                      static_cast<size_t>(channels),
                                                      static_cast<size_t>(numSamples));
        auto upsampled = oversampler_->processSamplesUp(inputBlock);
        const int factor = static_cast<int>(oversampler_->getOversamplingFactor());
        const int upsampledSamples = static_cast<int>(upsampled.getNumSamples());
        for (int i = 0; i < numSamples; ++i) {
            float peak = 0.0f;
            for (int k = 0; k < factor; ++k) {
                const int index = i * factor + k;
                if (index < upsampledSamples)
                    for (int c = 0; c < channels; ++c)
                        peak = juce::jmax(peak, std::abs(upsampled.getSample(c, index)));
            }
            upsampledPeak_[static_cast<size_t>(i)] = peak;
        }
    }

    // 2. Delay the audio by alignSamples_ so it lines up with the detector,
    //    and build the per-sample detector: the upsampled peaks covering this
    //    sample and the span back to the previous one, plus the sample itself
    //    (so the sample-peak ceiling holds exactly).
    const int ringChannels = juce::jmin(channels, alignRing_.getNumChannels());
    for (int i = 0; i < numSamples; ++i) {
        float samplePeak = 0.0f;
        for (int c = 0; c < ringChannels; ++c) {
            float* ring = alignRing_.getWritePointer(c);
            float* data = buffer.getWritePointer(c);
            const float delayed = ring[alignPos_];
            ring[alignPos_] = data[i];
            data[i] = delayed;
            samplePeak = juce::jmax(samplePeak, std::abs(delayed));
        }
        alignPos_ = (alignPos_ + 1) % juce::jmax(1, alignSamples_);
        if (detectorReady) {
            const float up = upsampledPeak_[static_cast<size_t>(i)];
            const float prev = i > 0 ? upsampledPeak_[static_cast<size_t>(i - 1)] : previousUpsampledPeak_;
            detectorPeak_[static_cast<size_t>(i)] = juce::jmax(samplePeak, up, prev);
        }
    }
    if (detectorReady) previousUpsampledPeak_ = upsampledPeak_[static_cast<size_t>(numSamples - 1)];
    // Channels the stage was not prepared for cannot be aligned; silence them
    // (the limiter does the same) rather than pass them through early.
    for (int c = ringChannels; c < numChannels; ++c) buffer.clear(c, 0, numSamples);

    brickwall_.process(buffer.getArrayOfWritePointers(), channels, numSamples, ceiling, limiterEnabled,
                       detectorReady ? detectorPeak_.data() : nullptr);

    // The processor splits host blocks to the prepared size, so this only
    // trips for a caller that skipped prepareToPlay.
    const int abChannels = juce::jmin(numChannels, preparedChannels_, 2);
    if (numSamples > abDryBuffer_.getNumSamples() || abChannels <= 0) return;

    // Align the dry signal with what the host is told. When bypassed, the
    // hosted chain was skipped but its latency is still reported, so delay
    // the dry by the REPORTED latency and output it (otherwise bypassed
    // audio arrived early by the hosted latency).
    const int dryDelaySamples = bypassSettled ? latencySamples() + reportedHostedLatency
                                              : latencySamples() + activeHostedLatency;
    abDryDelay_.adoptPending();
    for (int c = 0; c < abChannels; ++c)
        abDryDelay_.process(c, abDryBuffer_.getWritePointer(c), numSamples, dryDelaySamples);

    if (bypassSettled) {
        for (int c = 0; c < abChannels; ++c) buffer.copyFrom(c, 0, abDryBuffer_.getReadPointer(c), numSamples);
        wasBypassSettled_ = true;
        monitorTrimDb_.store(0.0f, std::memory_order_relaxed);
        return;
    }
    // Leaving bypass: the limiter/align delays still hold unprocessed input
    // from the bypassed period, and every latency-bearing hosted plugin (and
    // its dry-compensation line) still holds H samples of pre-bypass audio,
    // because the chain was not fed while bypassed (PLG-03). Stay on the
    // aligned dry until all of it has flushed, then fade in the processed side.
    if (wasBypassSettled_) bypassHold_ = latencySamples() + juce::jmax(0, reportedHostedLatency);
    wasBypassSettled_ = false;

    // Running mean-squares of processed (B) and aligned dry (A).
    double wetSum = 0.0, drySum = 0.0;
    for (int c = 0; c < abChannels; ++c) {
        const float* w = buffer.getReadPointer(c);
        const float* d = abDryBuffer_.getReadPointer(c);
        for (int i = 0; i < numSamples; ++i) {
            wetSum += static_cast<double>(w[i]) * w[i];
            drySum += static_cast<double>(d[i]) * d[i];
        }
    }
    const double n = static_cast<double>(numSamples) * juce::jmax(1, abChannels);
    const double alpha = 1.0 - std::exp(-static_cast<double>(numSamples) / (kMatchTimeConstantSec * sampleRate_));
    wetMs_ += alpha * (wetSum / n - wetMs_);
    dryMs_ += alpha * (drySum / n - dryMs_);
    // A non-finite EMA would otherwise disable matching until the next
    // prepare (AUDIT-025).
    if (!std::isfinite(wetMs_) || !std::isfinite(dryMs_)) {
        wetMs_ = dryMs_ = 0.0;
        matchGain_ = 1.0f;
    }
    float targetGain = 1.0f;
    if (wetMs_ > kSilenceMs && dryMs_ > kSilenceMs) {
        const float maxGain = juce::Decibels::decibelsToGain(kMaxMatchDb);
        targetGain = juce::jlimit(1.0f / maxGain, maxGain, static_cast<float>(std::sqrt(wetMs_ / dryMs_)));
    }

    // Blind: pick the hidden side once per entry into the mode, and again on
    // every explicit "next trial" request from the editor (PLG-12).
    const bool reshuffle = blindReshuffle_.exchange(false, std::memory_order_acq_rel);
    if (abMode == 3 && (lastAbMode_ != 3 || reshuffle))
        blindIsProcessed_.store(blindRandom_.nextBool(), std::memory_order_relaxed);
    lastAbMode_ = abMode;

    Listen listen = Listen::Processed;
    if (abMode == 1 || (abMode == 3 && !blindIsProcessed_.load(std::memory_order_relaxed)))
        listen = Listen::Original;
    else if (abMode == 2)
        listen = Listen::Delta;
    else if (abMode == 3)
        listen = Listen::BlindProcessed;
    if (listen != listenTo_) {
        // A switch mid-fade restarts from the side we were heading to.
        listenFrom_ = listenTo_;
        listenTo_ = listen;
        listenFadePos_ = 0;
    }

    /// What each listen side would put out for one sample of one channel.
    struct Sides {
        float processed, original, delta, blindProcessed;
    };
    const auto listenValue = [](Listen side, const Sides& s) noexcept {
        switch (side) {
        case Listen::Original: return s.original;
        case Listen::Delta: return s.delta;
        case Listen::BlindProcessed: return s.blindProcessed;
        case Listen::Processed:
        default: return s.processed;
        }
    };

    const float bypassTarget = globalBypass ? 1.0f : 0.0f;
    const float fadeStep = 1.0f / static_cast<float>(fadeSamples_);
    const float startGain = matchGain_;
    float* out[2] = {buffer.getWritePointer(0), abChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    const float* dryIn[2] = {abDryBuffer_.getReadPointer(0),
                             abChannels > 1 ? abDryBuffer_.getReadPointer(1) : nullptr};
    // The trims of the last sample, for monitorTrimDb().
    float originalTrim = 1.0f, deltaTrim = 1.0f;

    for (int i = 0; i < numSamples; ++i) {
        // Per-block linear ramp avoids zipper noise as the match moves.
        const float g =
            startGain + (targetGain - startGain) * static_cast<float>(i + 1) / static_cast<float>(numSamples);
        const bool abFading = listenFadePos_ < fadeSamples_;
        const float abT = abFading ? static_cast<float>(listenFadePos_ + 1) * fadeStep : 1.0f;
        if (bypassHold_ > 0)
            --bypassHold_;
        else if (bypassMix_ < bypassTarget)
            bypassMix_ = juce::jmin(bypassTarget, bypassMix_ + fadeStep);
        else if (bypassMix_ > bypassTarget)
            bypassMix_ = juce::jmax(bypassTarget, bypassMix_ - fadeStep);

        // Ceiling guard (FIX-12). The peaks include this sample, so a new
        // maximum is caught as it arrives; both channels share one trim.
        float dryMag = 0.0f, deltaMag = 0.0f;
        for (int c = 0; c < abChannels; ++c) {
            dryMag = juce::jmax(dryMag, std::abs(dryIn[c][i]));
            deltaMag = juce::jmax(deltaMag, std::abs(out[c][i] - dryIn[c][i] * g));
        }
        dryPeak_.push(dryMag, peakHoldSamples_, peakRelease_);
        deltaPeak_.push(deltaMag, peakHoldSamples_, peakRelease_);
        const float dryLevel = static_cast<float>(dryPeak_.level);
        const float deltaLevel = static_cast<float>(deltaPeak_.level);
        // With the limiter off there is no ceiling to hold, in any mode.
        const bool originalCapped = limiterEnabled && dryLevel * g > ceiling;
        const bool deltaCapped = limiterEnabled && deltaLevel > ceiling;
        // Gain of the Original: the level match, or what the dry headroom allows.
        const float originalGain = originalCapped ? ceiling / dryLevel : g;
        originalTrim = originalCapped ? originalGain / g : 1.0f;
        deltaTrim = deltaCapped ? ceiling / deltaLevel : 1.0f;

        for (int c = 0; c < abChannels; ++c) {
            const float wet = out[c][i];
            const float dry = dryIn[c][i];
            const float matched = dry * g;
            Sides sides{wet, matched, wet - matched, wet};
            // The jlimits only take up float rounding at the ceiling itself.
            if (originalCapped) {
                sides.original = juce::jlimit(-ceiling, ceiling, dry * originalGain);
                sides.blindProcessed = wet * originalTrim;
            }
            if (deltaCapped) sides.delta = juce::jlimit(-ceiling, ceiling, sides.delta * deltaTrim);
            float v = listenValue(listenTo_, sides);
            if (abFading) {
                const float from = listenValue(listenFrom_, sides);
                v = from + abT * (v - from);
                // A mix of two sides that are under the ceiling is under it too,
                // bar rounding. Never tighter than the processed sample itself,
                // which this stage does not alter.
                if (limiterEnabled) {
                    const float bound = juce::jmax(ceiling, std::abs(wet));
                    v = juce::jlimit(-bound, bound, v);
                }
            }
            // Bypass fades to the raw aligned dry (no level matching).
            out[c][i] = v + bypassMix_ * (dry - v);
        }
        if (abFading) ++listenFadePos_;
    }
    matchGain_ = targetGain;
    matchGainDb_.store(juce::Decibels::gainToDecibels(targetGain), std::memory_order_relaxed);
    float heardTrim = 1.0f;
    if (listenTo_ == Listen::Original || listenTo_ == Listen::BlindProcessed)
        heardTrim = originalTrim;
    else if (listenTo_ == Listen::Delta)
        heardTrim = deltaTrim;
    monitorTrimDb_.store(juce::Decibels::gainToDecibels(heardTrim), std::memory_order_relaxed);
}

} // namespace khensu
