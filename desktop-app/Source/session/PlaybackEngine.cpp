// desktop-app/Source/session/PlaybackEngine.cpp
#include "PlaybackEngine.h"
#include <cmath>
#include <khensu/session/AudioFileProber.h>

namespace khensu {

void PlaybackEngine::FadeGate::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    sampleRate_.store(sampleRate, std::memory_order_release);
    input_.prepareToPlay(samplesPerBlockExpected, sampleRate);
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
}

PlaybackEngine::PlaybackEngine()
{
    readAheadThread_.startThread(juce::Thread::Priority::high);
    deviceManager_.initialiseWithDefaultDevices(0, 2);
    deviceManager_.addAudioCallback(&sourcePlayer_);
    sourcePlayer_.setSource(&fadeGate_);
    formatManager_.registerBasicFormats();
}

PlaybackEngine::~PlaybackEngine()
{
    transportSource_.setSource(nullptr);
    sourcePlayer_.setSource(nullptr);
    deviceManager_.removeAudioCallback(&sourcePlayer_);
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
    }
    fadeGate_.snapTo(0.0f);
    transportSource_.start();
    fadeGate_.fadeTo(1.0f);
}

void PlaybackEngine::pause()
{
    fadeOutAndWait();
    transportSource_.stop();
}

void PlaybackEngine::stop()
{
    fadeOutAndWait();
    transportSource_.stop();
    transportSource_.setPosition(0.0);
}

void PlaybackEngine::setPosition(double seconds) { transportSource_.setPosition(seconds); }
double PlaybackEngine::getPosition() const { return transportSource_.getCurrentPosition(); }
double PlaybackEngine::getLength() const { return transportSource_.getLengthInSeconds(); }
bool PlaybackEngine::isPlaying() const { return transportSource_.isPlaying(); }
void PlaybackEngine::setGain(float linear) { transportSource_.setGain(linear); }

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
    transportSource_.setSource(nullptr);
    bufferingSource_.reset();
    readerSource_.reset();
}

juce::AudioFormatReader* PlaybackEngine::createReaderFor(const juce::File& file)
{
    return AudioFileProber::openReader(formatManager_, file).release();
}

} // namespace khensu
