#include "CopilotRealtimeTelemetry.h"

#include <khensu/analysis/KWeighting.h>

#include <cmath>

namespace khensu {

void CopilotRealtimeTelemetry::prepareToPlay(double sampleRate, int /*channels*/)
{
    leftPeakDb_.store(-96.0f, std::memory_order_relaxed);
    rightPeakDb_.store(-96.0f, std::memory_order_relaxed);
    ringBuffer_.setSize(2, 16384);
    ringBuffer_.reset();

    currentSampleRate_ = sampleRate;
    // K-weighting comes from shared-core, the one definition the desktop
    // LoudnessAnalyzer uses too (FIX-11): the plugin used to carry its own
    // RBJ shelf + normalised high-pass, which read about 0.045 LU low.
    // Computed here, never on the audio thread.
    const auto kWeighting = analysis::kWeightingCoefficients(sampleRate);
    for (int c = 0; c < 2; ++c) {
        kPre_[c].set(kWeighting.preFilter);
        kRlb_[c].set(kWeighting.rlbFilter);
    }

    binSamples_ = juce::jmax(1, juce::roundToInt(sampleRate * 0.1));
    resetLoudnessWindows();
    emaLR_ = emaLL_ = emaRR_ = emaMid_ = emaSide_ = 0.0;
}

void CopilotRealtimeTelemetry::resetLoudnessWindows() noexcept
{
    bins_.fill(0.0);
    binPos_ = 0;
    binFill_ = 0;
    binEnergy_ = 0.0;
    momentaryLufs_.store(-96.0f, std::memory_order_relaxed);
    shortTermLufs_.store(-96.0f, std::memory_order_relaxed);
}

void CopilotRealtimeTelemetry::publishLoudness() noexcept
{
    // Rectangular windows over the newest completed bins (silence before the
    // first bins, as a fresh BS.1770 meter starts).
    double momentary = 0.0, shortTerm = 0.0;
    for (int k = 1; k <= kShortTermBins; ++k) {
        const double bin = bins_[static_cast<size_t>((binPos_ - k + kShortTermBins) % kShortTermBins)];
        shortTerm += bin;
        if (k <= kMomentaryBins) momentary += bin;
    }
    const double binLen = static_cast<double>(binSamples_);
    auto powToLufs = [](double p) noexcept -> float {
        return p > 1e-12 ? static_cast<float>(-0.691 + 10.0 * std::log10(p)) : -96.0f;
    };
    momentaryLufs_.store(powToLufs(momentary / (kMomentaryBins * binLen)), std::memory_order_relaxed);
    shortTermLufs_.store(powToLufs(shortTerm / (kShortTermBins * binLen)), std::memory_order_relaxed);
}

CopilotRealtimeTelemetry::HostTransport CopilotRealtimeTelemetry::captureHostTransport(juce::AudioProcessor& processor)
{
    HostTransport transport;
    if (auto* hostPlayHead = processor.getPlayHead()) {
        if (auto position = hostPlayHead->getPosition()) {
            if (auto seconds = position->getTimeInSeconds()) {
                transport.timeSec = *seconds;
                transport.timeValid = true;
            } else if (auto samples = position->getTimeInSamples(); samples && processor.getSampleRate() > 0.0) {
                transport.timeSec = static_cast<double>(*samples) / processor.getSampleRate();
                transport.timeValid = true;
            }
            transport.playing = position->getIsPlaying();
            positionSec_.store(static_cast<float>(transport.timeSec), std::memory_order_relaxed);
            isPlaying_.store(transport.playing, std::memory_order_relaxed);
            if (auto bpmOpt = position->getBpm())
                bpm_.store(static_cast<float>(*bpmOpt), std::memory_order_relaxed);
            looping_.store(position->getIsLooping(), std::memory_order_relaxed);
            if (auto loop = position->getLoopPoints())
                loopBeats_.store(juce::jmax(0.0, loop->ppqEnd - loop->ppqStart), std::memory_order_relaxed);
        }
    }
    return transport;
}

void CopilotRealtimeTelemetry::process(const juce::AudioBuffer<float>& buffer)
{
    const int nSamples = buffer.getNumSamples();
    const int nChannels = buffer.getNumChannels();
    if (nSamples <= 0) return;

    auto toDb = [](float gain) noexcept -> float {
        return gain > 0.0f ? juce::Decibels::gainToDecibels(gain) : -96.0f;
    };

    if (nChannels >= 1) leftPeakDb_.store(toDb(buffer.getMagnitude(0, 0, nSamples)), std::memory_order_relaxed);
    if (nChannels >= 2)
        rightPeakDb_.store(toDb(buffer.getMagnitude(1, 0, nSamples)), std::memory_order_relaxed);
    else if (nChannels == 1)
        // Mono host: mirror the single channel so the desktop meters do not
        // show a frozen -96 right channel.
        rightPeakDb_.store(toDb(buffer.getMagnitude(0, 0, nSamples)), std::memory_order_relaxed);

    // Loudness uses a K-weighted metering copy; correlation and width use the
    // unweighted signal. Mono hosts duplicate channel 0.
    if (nChannels >= 1) {
        const float* L = buffer.getReadPointer(0);
        const float* R = buffer.getReadPointer(nChannels >= 2 ? 1 : 0);
        double sumLL = 0.0, sumRR = 0.0, sumLR = 0.0, sumMid = 0.0, sumSide = 0.0;
        // BS.1770 sums channels that exist: a mono host must contribute one
        // channel, not channel 0 twice (that read +3.01 LU high).
        const bool isMono = nChannels < 2;
        for (int i = 0; i < nSamples; ++i) {
            const double l = L[i], r = R[i];
            sumLL += l * l;
            sumRR += r * r;
            sumLR += l * r;
            const double m = 0.5 * (l + r), s = 0.5 * (l - r);
            sumMid += m * m;
            sumSide += s * s;

            const double kl = kRlb_[0].process(kPre_[0].process(l));
            double energy = kl * kl;
            if (!isMono) {
                const double kr = kRlb_[1].process(kPre_[1].process(r));
                energy += kr * kr;
            }
            binEnergy_ += energy;
            if (++binFill_ >= binSamples_) {
                bins_[static_cast<size_t>(binPos_)] = binEnergy_;
                binPos_ = (binPos_ + 1) % kShortTermBins;
                binEnergy_ = 0.0;
                binFill_ = 0;
                publishLoudness();
            }
        }
        const double inv = 1.0 / nSamples;
        const double blockSec = nSamples / currentSampleRate_;
        const double aMom = 1.0 - std::exp(-blockSec / 0.4);

        emaLL_ += aMom * (sumLL * inv - emaLL_);
        emaRR_ += aMom * (sumRR * inv - emaRR_);
        emaLR_ += aMom * (sumLR * inv - emaLR_);
        emaMid_ += aMom * (sumMid * inv - emaMid_);
        emaSide_ += aMom * (sumSide * inv - emaSide_);

        // One non-finite block would otherwise leave the meters stuck on NaN
        // until the next prepare (AUDIT-025): drop the history and restart.
        if (!std::isfinite(binEnergy_ + emaLL_ + emaRR_ + emaLR_ + emaMid_ + emaSide_)) {
            resetLoudnessWindows();
            emaLR_ = emaLL_ = emaRR_ = emaMid_ = emaSide_ = 0.0;
            for (int c = 0; c < 2; ++c) {
                kPre_[c].reset();
                kRlb_[c].reset();
            }
        }

        const double denom = std::sqrt(emaLL_ * emaRR_);
        correlation_.store(denom > 1e-12 ? static_cast<float>(juce::jlimit(-1.0, 1.0, emaLR_ / denom)) : 0.0f,
                           std::memory_order_relaxed);
        const double midSide = emaMid_ + emaSide_;
        stereoWidth_.store(midSide > 1e-12 ? static_cast<float>(juce::jlimit(0.0, 1.0, 2.0 * emaSide_ / midSide))
                                            : 1.0f,
                           std::memory_order_relaxed);
    }

    ringBuffer_.write(buffer);
}

CopilotRealtimeTelemetry::Snapshot CopilotRealtimeTelemetry::snapshot() const noexcept
{
    Snapshot result;
    result.position_sec = positionSec_.load(std::memory_order_relaxed);
    result.is_playing = isPlaying_.load(std::memory_order_relaxed);
    result.left_peak_db = leftPeakDb_.load(std::memory_order_relaxed);
    result.right_peak_db = rightPeakDb_.load(std::memory_order_relaxed);
    result.momentary_lufs = momentaryLufs_.load(std::memory_order_relaxed);
    result.short_term_lufs = shortTermLufs_.load(std::memory_order_relaxed);
    result.correlation = correlation_.load(std::memory_order_relaxed);
    result.width = stereoWidth_.load(std::memory_order_relaxed);
    return result;
}

} // namespace khensu
