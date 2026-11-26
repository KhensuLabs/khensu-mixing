// desktop-app/Source/session/PlaybackEngine.cpp
#include "PlaybackEngine.h"
#include <cmath>
#include <khensu/session/AudioFileProber.h>

namespace khensu {

void PlaybackEngine::FadeGate::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    sampleRate_.store(sampleRate, std::memory_order_release);
    input_.prepareToPlay(samplesPerBlockExpected, sampleRate);
    if (tap_ != nullptr) tap_->prepare(sampleRate);
}

void PlaybackEngine::FadeGate::getNextAudioBlock(const juce::AudioSourceChannelInfo& info)
{
    input_.getNextAudioBlock(info);
    if (info.buffer == nullptr || info.numSamples <= 0) return;

    const float target = target_.load(std::memory_order_acquire);
    if (snapRequested_.exchange(false, std::memory_order_acq_rel)) current_ = target;

    const double sr = sampleRate_.load(std::memory_order_acquire);
    const int fadeSamples = juce::jmax(1, (int)std::lround((sr > 0.0 ? sr : 44100.0) * kFadeMs / 1000.0));
    const float step = 1.0f / (float)fadeSamples; // a full 0<->1 ramp takes kFadeMs

    int done = 0;
    if (current_ != target) {
        const int needed = (int)std::ceil(std::abs(target - current_) / step);
        const int rampLen = juce::jmin(needed, info.numSamples);
        const float end = rampLen >= needed ? target : current_ + (target > current_ ? step : -step) * (float)rampLen;
        for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
            info.buffer->applyGainRamp(ch, info.startSample, rampLen, current_, end);
        current_ = end;
        done = rampLen;
    }
    if (done < info.numSamples) {
        if (current_ <= 0.0f) {
            for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
                info.buffer->clear(ch, info.startSample + done, info.numSamples - done);
        } else if (current_ != 1.0f) {
            for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
                info.buffer->applyGain(ch, info.startSample + done, info.numSamples - done, current_);
        }
    }
    reached_.store(current_, std::memory_order_release);
    if (tap_ != nullptr) tap_->process(info); // UI-5: meter what is heard
}

// ---- A/B mixer (UI-6) -----------------------------------------------------------

void PlaybackEngine::equalPowerGains(float t, float& gainA, float& gainB) noexcept
{
    t = juce::jlimit(0.0f, 1.0f, t);
    if (t <= 0.0f) {
        gainA = 1.0f;
        gainB = 0.0f;
        return;
    }
    if (t >= 1.0f) {
        gainA = 0.0f;
        gainB = 1.0f;
        return;
    }
    const float angle = t * juce::MathConstants<float>::halfPi;
    gainA = std::cos(angle);
    gainB = std::sin(angle);
}

void PlaybackEngine::AbMixer::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    main_.prepareToPlay(samplesPerBlockExpected, sampleRate);
    alt_.prepareToPlay(samplesPerBlockExpected, sampleRate);
    // Sized here, never on the audio thread; a longer callback is mixed in parts.
    scratch_.setSize(8, juce::jmax(samplesPerBlockExpected, 512), false, true, false);
}

void PlaybackEngine::AbMixer::releaseResources()
{
    main_.releaseResources();
    alt_.releaseResources();
}

void PlaybackEngine::AbMixer::getNextAudioBlock(const juce::AudioSourceChannelInfo& info)
{
    const bool alt = altActive_.load(std::memory_order_acquire);
    // Keep the alternate on the main deck's sample: both read from here on in
    // the same callback, so they cannot drift apart by a block.
    if (alt && resync_.exchange(false, std::memory_order_acq_rel))
        alt_.setNextReadPosition(main_.getNextReadPosition());
    main_.getNextAudioBlock(info);
    if (info.buffer == nullptr || info.numSamples <= 0) return;

    const float target = alt ? target_.load(std::memory_order_acquire) : 0.0f;
    if (snap_.exchange(false, std::memory_order_acq_rel) || !alt) t_ = target;
    if (!alt) {
        reached_.store(t_, std::memory_order_release);
        return;
    }

    const int fadeSamples = juce::jmax(1, (int)std::lround(sampleRate_ * kCrossfadeMs / 1000.0));
    const float step = 1.0f / (float)fadeSamples;
    const int channels = juce::jmin(info.buffer->getNumChannels(), scratch_.getNumChannels());
    int done = 0;
    while (done < info.numSamples) {
        const int n = juce::jmin(info.numSamples - done, scratch_.getNumSamples());
        // Always read the alternate, so it keeps time with the main deck.
        alt_.getNextAudioBlock(juce::AudioSourceChannelInfo(&scratch_, 0, n));
        const int start = info.startSample + done;
        if (juce::exactlyEqual(t_, target) && t_ <= 0.0f) {
            // All A: the main deck's output stands.
        } else if (juce::exactlyEqual(t_, target) && t_ >= 1.0f) {
            for (int ch = 0; ch < channels; ++ch)
                info.buffer->copyFrom(ch, start, scratch_, ch, 0, n);
            for (int ch = channels; ch < info.buffer->getNumChannels(); ++ch)
                info.buffer->clear(ch, start, n);
        } else {
            for (int i = 0; i < n; ++i) {
                if (t_ < target)
                    t_ = juce::jmin(target, t_ + step);
                else if (t_ > target)
                    t_ = juce::jmax(target, t_ - step);
                float gainA = 1.0f, gainB = 0.0f;
                equalPowerGains(t_, gainA, gainB);
                for (int ch = 0; ch < channels; ++ch) {
                    float* out = info.buffer->getWritePointer(ch, start);
                    out[i] = out[i] * gainA + scratch_.getSample(ch, i) * gainB;
                }
            }
        }
        done += n;
    }
    reached_.store(t_, std::memory_order_release);
}

// ---- Engine ---------------------------------------------------------------------

PlaybackEngine::PlaybackEngine()
{
    readAheadThread_.startThread(juce::Thread::Priority::high);
    deviceManager_.initialiseWithDefaultDevices(0, 2);
    deviceManager_.addAudioCallback(&sourcePlayer_);
    sourcePlayer_.setSource(&monitorStage_);
    formatManager_.registerBasicFormats();
}

PlaybackEngine::~PlaybackEngine()
{
    mixer_.setAltActive(false);
    altTransport_.setSource(nullptr);
    transportSource_.setSource(nullptr);
    sourcePlayer_.setSource(nullptr);
    deviceManager_.removeAudioCallback(&sourcePlayer_);
    altBufferingSource_.reset();
    altReaderSource_.reset();
    bufferingSource_.reset();
    readerSource_.reset();
    readAheadThread_.stopThread(2000);
}

bool PlaybackEngine::deviceRunning() const
{
    auto* device = deviceManager_.getCurrentAudioDevice();
    return device != nullptr && device->isPlaying() && fadeGate_.sampleRate() > 0.0;
}

void PlaybackEngine::fadeOutAndWait()
{
    if (!transportSource_.isPlaying() || !deviceRunning()) {
        fadeGate_.snapTo(0.0f);
        return;
    }
    fadeGate_.fadeTo(0.0f);
    // The ramp needs kFadeMs plus up to a couple of device blocks to land;
    // bound the wait so a stalled device can never hang the message thread.
    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32)(kFadeMs * 2.0) + 80u;
    while (fadeGate_.reachedGain() > 0.0f && juce::Time::getMillisecondCounter() < deadline)
        juce::Thread::sleep(1);
}

void PlaybackEngine::play()
{
    if (transportSource_.isPlaying()) return;
    // Pre-roll: setSource()/setPosition() leave the read-ahead buffer empty,
    // so starting at once played a gap of silence (or a stale block) before
    // the decoder caught up. Wait - bounded - for the first block at the play
    // position, then start from silence and fade in.
    if (bufferingSource_ != nullptr && deviceRunning()) {
        const juce::AudioSourceChannelInfo want(nullptr, 0, kPrerollSamples);
        bufferingSource_->waitForNextAudioBlockReady(want, kPrerollTimeoutMs);
        if (altBufferingSource_ != nullptr && hasAlternate())
            altBufferingSource_->waitForNextAudioBlockReady(want, kPrerollTimeoutMs);
    }
    fadeGate_.snapTo(0.0f);
    transportSource_.start();
    if (hasAlternate()) {
        altTransport_.start();
        mixer_.requestResync(); // both decks start on the same sample
    }
    fadeGate_.fadeTo(1.0f);
}

void PlaybackEngine::pause()
{
    fadeOutAndWait();
    transportSource_.stop();
    altTransport_.stop();
}

void PlaybackEngine::stop()
{
    fadeOutAndWait();
    transportSource_.stop();
    altTransport_.stop();
    setPosition(0.0);
}

void PlaybackEngine::setPosition(double seconds)
{
    transportSource_.setPosition(seconds);
    if (hasAlternate()) {
        altTransport_.setPosition(seconds);
        mixer_.requestResync();
    }
}

double PlaybackEngine::getPosition() const { return transportSource_.getCurrentPosition(); }
double PlaybackEngine::getLength() const { return transportSource_.getLengthInSeconds(); }
bool PlaybackEngine::isPlaying() const { return transportSource_.isPlaying(); }
void PlaybackEngine::setGain(float linear) { transportSource_.setGain(linear); }
void PlaybackEngine::setAlternateGain(float linear) { altTransport_.setGain(linear); }

double PlaybackEngine::loadFile(const juce::File& file)
{
    clearSource();
    if (!file.existsAsFile()) return 0.0;
    auto* reader = AudioFileProber::openReader(formatManager_, file).release();
    if (reader == nullptr) return 0.0;
    const double sampleRate = reader->sampleRate;
    const int channels = juce::jlimit(1, 8, (int)reader->numChannels);
    readerSource_ = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
    bufferingSource_ = std::make_unique<juce::BufferingAudioSource>(readerSource_.get(), readAheadThread_, false,
                                                                    kReadAheadSamples, juce::jmax(2, channels),
                                                                    /*prefillBufferOnPrepareToPlay*/ false);
    transportSource_.setSource(bufferingSource_.get(), 0, nullptr, sampleRate, juce::jmax(2, channels));
    return sampleRate;
}

void PlaybackEngine::clearSource()
{
    // Fade the old source out before cutting it (AUDIT-037); setSource()
    // itself drops playback instantly mid-waveform.
    fadeOutAndWait();
    detachAlternate(); // silent already: no crossfade back
    transportSource_.setSource(nullptr);
    bufferingSource_.reset();
    readerSource_.reset();
}

double PlaybackEngine::loadAlternate(const juce::File& file)
{
    clearAlternate();
    if (!file.existsAsFile() || readerSource_ == nullptr) return 0.0;
    auto* reader = AudioFileProber::openReader(formatManager_, file).release();
    if (reader == nullptr) return 0.0;
    const double sampleRate = reader->sampleRate;
    const int channels = juce::jlimit(1, 8, (int)reader->numChannels);
    altReaderSource_ = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
    altBufferingSource_ = std::make_unique<juce::BufferingAudioSource>(
        altReaderSource_.get(), readAheadThread_, false, kReadAheadSamples, juce::jmax(2, channels),
        /*prefillBufferOnPrepareToPlay*/ false);
    altTransport_.setSource(altBufferingSource_.get(), 0, nullptr, sampleRate, juce::jmax(2, channels));
    altTransport_.setPosition(transportSource_.getCurrentPosition());
    mixer_.snapTo(0.0f); // the main deck stays the audible one
    mixer_.requestResync();
    mixer_.setAltActive(true);
    altLoaded_.store(true, std::memory_order_release);
    if (transportSource_.isPlaying()) altTransport_.start();
    return sampleRate;
}

void PlaybackEngine::clearAlternate()
{
    if (!hasAlternate()) return;
    // Back to the main deck first, so a playing B does not cut off.
    if (alternateSelected() && transportSource_.isPlaying() && deviceRunning()) {
        mixer_.crossfadeTo(0.0f);
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32)(kCrossfadeMs * 2.0) + 80u;
        while (mixer_.reached() > 0.0f && juce::Time::getMillisecondCounter() < deadline)
            juce::Thread::sleep(1);
    }
    detachAlternate();
}

void PlaybackEngine::detachAlternate()
{
    mixer_.setAltActive(false);
    mixer_.snapTo(0.0f);
    altLoaded_.store(false, std::memory_order_release);
    altTransport_.stop();
    altTransport_.setSource(nullptr); // takes the callback lock: the mixer is done with it
    altBufferingSource_.reset();
    altReaderSource_.reset();
}

void PlaybackEngine::crossfadeTo(bool alternate)
{
    if (!hasAlternate()) return;
    const float t = alternate ? 1.0f : 0.0f;
    if (transportSource_.isPlaying() && deviceRunning())
        mixer_.crossfadeTo(t);
    else
        mixer_.snapTo(t);
}

juce::AudioFormatReader* PlaybackEngine::createReaderFor(const juce::File& file)
{
    return AudioFileProber::openReader(formatManager_, file).release();
}

} // namespace khensu
