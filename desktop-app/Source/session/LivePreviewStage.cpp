// desktop-app/Source/session/LivePreviewStage.cpp
// STU-9: see LivePreviewStage.h.
#include "LivePreviewStage.h"
#include <cmath>

namespace khensu {

LivePreviewStage::LivePreviewStage(juce::AudioSource& input) : input_(input)
{
    for (auto& slot : retired_) slot.store(nullptr, std::memory_order_relaxed);
}

LivePreviewStage::~LivePreviewStage()
{
    stopTimer();
    // The owner detached the device first: nothing runs the audio side now.
    delete pending_.exchange(nullptr);
    collectGarbage();
    if (fading_ != active_) delete fading_;
    delete active_;
}

void LivePreviewStage::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    input_.prepareToPlay(samplesPerBlockExpected, sampleRate);
    sampleRate_.store(sampleRate, std::memory_order_release);
    // Sized here, never on the audio thread; a longer callback runs in parts.
    old_.setSize(2, juce::jmax(samplesPerBlockExpected, 512), false, true, false);
    fadeSamples_ = juce::jmax(1, (int)std::lround((sampleRate > 0.0 ? sampleRate : kFallbackRate) * kCrossfadeMs / 1000.0));
}

// ---- audio thread -------------------------------------------------------------

bool LivePreviewStage::tryRetire(dsp::RealtimeChain* chain) noexcept
{
    for (auto& slot : retired_) {
        dsp::RealtimeChain* expected = nullptr;
        if (slot.compare_exchange_strong(expected, chain, std::memory_order_acq_rel)) return true;
    }
    return false;
}

bool LivePreviewStage::hasFreeRetireSlot() const noexcept
{
    for (const auto& slot : retired_)
        if (slot.load(std::memory_order_acquire) == nullptr) return true;
    return false;
}

void LivePreviewStage::takePending() noexcept
{
    if (!fadeActive_ && fading_ != nullptr && tryRetire(fading_)) fading_ = nullptr; // a parked old chain
    // Taking a chain mid-fade retires the outgoing one: wait for room.
    if (fading_ != nullptr && !hasFreeRetireSlot()) return;
    auto* next = pending_.exchange(nullptr, std::memory_order_acq_rel);
    if (next == nullptr) return;
    if (active_ != nullptr) next->adoptMatchState(*active_);
    // A fade cut short: its outgoing chain is retired now; the new fade
    // starts from what was becoming audible.
    if (fading_ != nullptr) tryRetire(fading_); // room checked above
    fading_ = active_;
    active_ = next;
    fadeActive_ = true;
    fadePos_ = 0;
}

void LivePreviewStage::finishFade() noexcept
{
    fadeActive_ = false;
    if (fading_ != nullptr && tryRetire(fading_)) fading_ = nullptr;
    // else: parked (not processed) until a retire slot is free.
}

void LivePreviewStage::getNextAudioBlock(const juce::AudioSourceChannelInfo& info)
{
    input_.getNextAudioBlock(info);
    if (info.buffer == nullptr || info.numSamples <= 0) return;
    takePending();
    if (active_ == nullptr && !fadeActive_) return; // plain signal

    const int channels = juce::jmin(2, info.buffer->getNumChannels());
    if (channels <= 0) return;
    int done = 0;
    while (done < info.numSamples) {
        const int n = fadeActive_ ? juce::jmin(info.numSamples - done, old_.getNumSamples()) : info.numSamples - done;
        const int start = info.startSample + done;
        float* io[2] = {info.buffer->getWritePointer(0, start),
                        info.buffer->getWritePointer(channels > 1 ? 1 : 0, start)};
        if (fadeActive_)
            for (int ch = 0; ch < channels; ++ch) old_.copyFrom(ch, 0, *info.buffer, ch, start, n);
        if (active_ != nullptr) active_->process(io, channels, n);
        if (fadeActive_) {
            float* oldIo[2] = {old_.getWritePointer(0), old_.getWritePointer(channels > 1 ? 1 : 0)};
            if (fading_ != nullptr) fading_->process(oldIo, channels, n);
            // Linear crossfade: the two outputs are the same music, so they
            // add in phase (an equal-power law would bump by 3 dB).
            const int len = juce::jmin(n, fadeSamples_ - fadePos_);
            const float step = 1.0f / (float)fadeSamples_;
            for (int ch = 0; ch < channels; ++ch) {
                float* out = io[ch];
                const float* prev = oldIo[ch];
                for (int i = 0; i < len; ++i) {
                    const float t = (float)(fadePos_ + i + 1) * step;
                    out[i] = prev[i] + t * (out[i] - prev[i]);
                }
            }
            fadePos_ += len;
            if (fadePos_ >= fadeSamples_) finishFade();
        }
        done += n;
    }
}

// ---- message thread -----------------------------------------------------------

dsp::RealtimeChain* LivePreviewStage::build(const std::vector<dsp::ChainStageSpec>& chain)
{
    const double rate = sampleRate_.load(std::memory_order_acquire);
    builtRate_ = rate > 0.0 ? rate : kFallbackRate;
    auto next = std::make_unique<dsp::RealtimeChain>();
    std::string error;
    if (!next->prepare(chain, builtRate_, 2, kChainBlock, error)) next->prepare({}, kFallbackRate, 2, kChainBlock, error);
    plan_ = next->plan();
    return next.release();
}

void LivePreviewStage::publish(dsp::RealtimeChain* chain)
{
    collectGarbage();
    // The audio thread never saw a chain still pending: ours to delete.
    delete pending_.exchange(chain, std::memory_order_acq_rel);
}

void LivePreviewStage::collectGarbage()
{
    for (auto& slot : retired_) delete slot.exchange(nullptr, std::memory_order_acq_rel);
}

const std::vector<dsp::RealtimeChain::StagePlan>& LivePreviewStage::setChain(std::vector<dsp::ChainStageSpec> chain)
{
    specs_ = std::move(chain);
    enabled_ = true;
    publish(build(specs_));
    if (!isTimerRunning()) startTimer(100);
    return plan_;
}

void LivePreviewStage::clear()
{
    if (!enabled_) return;
    enabled_ = false;
    specs_.clear();
    plan_.clear();
    publish(build({})); // an identity chain: crossfades back to the plain signal
}

void LivePreviewStage::timerCallback()
{
    collectGarbage();
    // A device restarted at another rate: rebuild at the new one.
    const double rate = sampleRate_.load(std::memory_order_acquire);
    if (enabled_ && rate > 0.0 && std::abs(rate - builtRate_) > 0.5) publish(build(specs_));
    // Keeps running once started: a fade can retire a chain at any time.
}

} // namespace khensu
