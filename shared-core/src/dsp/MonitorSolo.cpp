// ENH-10 (2) / STU-5: see MonitorSolo.h. Moved from plugin-copilot so the
// desktop transport and the plugin share the crossovers.
#include <khensu/dsp/MonitorSolo.h>

#include <cmath>

namespace khensu::dsp {

juce::String MonitorSolo::modeName(Mode mode)
{
    switch (mode) {
        case Mode::Mono: return TRANS("Mono");
        case Mode::Side: return TRANS("Side");
        case Mode::Sub: return TRANS("Sub < 120 Hz");
        case Mode::Low: return TRANS("Low < 250 Hz");
        case Mode::Mid: return TRANS("Mid 250 Hz - 4 kHz");
        case Mode::High: return TRANS("High > 4 kHz");
        case Mode::Off: break;
    }
    return TRANS("Off");
}

void MonitorSolo::prepare(double sampleRate, int maxBlockSize)
{
    const juce::dsp::ProcessSpec spec{sampleRate > 0.0 ? sampleRate : 48000.0,
                                      static_cast<juce::uint32>(juce::jmax(1, maxBlockSize)), 2};
    for (auto* f : {&subSplit_, &lowSplit_, &highSplit_}) {
        f->setType(Filter::Type::lowpass);
        f->prepare(spec);
    }
    subSplit_.setCutoffFrequency(kSubHz);
    lowSplit_.setCutoffFrequency(kLowMidHz);
    highSplit_.setCutoffFrequency(kMidHighHz);
    fadeSamples_ = juce::jmax(1, static_cast<int>(std::lround(spec.sampleRate * kFadeSeconds)));
    from_ = to_ = Mode::Off;
    fadePos_ = fadeSamples_;
    dimGain_ = dimRequested_.load(std::memory_order_relaxed) ? juce::Decibels::decibelsToGain(kDimDb) : 1.0f;
    dimStep_ = (1.0f - juce::Decibels::decibelsToGain(kDimDb)) / static_cast<float>(fadeSamples_);
    filtersWarm_ = false;
    active_.store(false, std::memory_order_relaxed);
}

float MonitorSolo::valueFor(Mode mode, int channel, float left, float right) const noexcept
{
    const auto c = static_cast<size_t>(channel);
    switch (mode) {
        case Mode::Mono: return 0.5f * (left + right);
        case Mode::Side: return 0.5f * (left - right);
        case Mode::Sub: return sub_[c];
        case Mode::Low: return low_[c];
        case Mode::Mid: return mid_[c];
        case Mode::High: return high_[c];
        case Mode::Off: break;
    }
    return channel == 0 ? left : right;
}

void MonitorSolo::process(juce::AudioBuffer<float>& buffer, bool offline) noexcept
{
    process(buffer, 0, buffer.getNumSamples(), offline);
}

void MonitorSolo::process(juce::AudioBuffer<float>& buffer, int startSample, int numSamples, bool offline) noexcept
{
    const int n = juce::jmin(numSamples, buffer.getNumSamples() - startSample);
    const int channels = juce::jmin(2, buffer.getNumChannels());
    if (n <= 0 || channels <= 0 || startSample < 0) return;

    const float dimTarget = dimRequested_.load(std::memory_order_relaxed) ? juce::Decibels::decibelsToGain(kDimDb)
                                                                          : 1.0f;
    if (offline) {
        // A bounce never carries a monitoring solo: plain signal, no fade.
        from_ = to_ = Mode::Off;
        fadePos_ = fadeSamples_;
        dimGain_ = 1.0f;
        active_.store(false, std::memory_order_relaxed);
        return;
    }
    const auto wanted = static_cast<Mode>(juce::jlimit(0, kNumModes - 1, requested_.load(std::memory_order_relaxed)));
    if (wanted != to_) {
        // A switch mid-fade restarts from the mode we were heading to.
        from_ = to_;
        to_ = wanted;
        fadePos_ = 0;
    }
    const bool fading = fadePos_ < fadeSamples_;
    const bool dimming = !juce::exactlyEqual(dimGain_, dimTarget) || !juce::exactlyEqual(dimTarget, 1.0f);
    if (!fading && to_ == Mode::Off && !dimming) {
        filtersWarm_ = false;
        active_.store(false, std::memory_order_relaxed);
        return;
    }
    active_.store(true, std::memory_order_relaxed);
    const auto isBand = [](Mode m) { return m == Mode::Sub || m == Mode::Low || m == Mode::Mid || m == Mode::High; };
    const bool needFilters = isBand(from_) || isBand(to_);
    if (needFilters && !filtersWarm_) {
        // The filters slept while unused; start them clean (the fade covers
        // their settling).
        subSplit_.reset();
        lowSplit_.reset();
        highSplit_.reset();
        filtersWarm_ = true;
    } else if (!needFilters) {
        filtersWarm_ = false;
    }

    float* data[2] = {buffer.getWritePointer(0, startSample),
                      buffer.getWritePointer(channels > 1 ? 1 : 0, startSample)};
    const float step = 1.0f / static_cast<float>(fadeSamples_);
    for (int i = 0; i < n; ++i) {
        const float left = data[0][i];
        const float right = channels > 1 ? data[1][i] : left;
        if (needFilters) {
            const float in[2] = {left, right};
            for (int c = 0; c < channels; ++c) {
                float lo = 0.0f, hi = 0.0f;
                subSplit_.processSample(c, in[c], lo, hi);
                sub_[c] = lo;
                lowSplit_.processSample(c, in[c], lo, hi);
                low_[c] = lo;
                float midPart = 0.0f, highPart = 0.0f;
                highSplit_.processSample(c, hi, midPart, highPart);
                mid_[c] = midPart;
                high_[c] = highPart;
            }
        }
        if (dimGain_ < dimTarget)
            dimGain_ = juce::jmin(dimTarget, dimGain_ + dimStep_);
        else if (dimGain_ > dimTarget)
            dimGain_ = juce::jmax(dimTarget, dimGain_ - dimStep_);
        const bool inFade = fadePos_ < fadeSamples_;
        const float t = inFade ? static_cast<float>(fadePos_ + 1) * step : 1.0f;
        for (int c = 0; c < channels; ++c) {
            float v = valueFor(to_, c, left, right);
            if (inFade) {
                const float from = valueFor(from_, c, left, right);
                v = from + t * (v - from);
            }
            data[c][i] = v * dimGain_;
        }
        if (inFade) ++fadePos_;
    }
}

} // namespace khensu::dsp
