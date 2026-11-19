// desktop-app/Source/ui/shared/VoiceRecorder.cpp
#include "VoiceRecorder.h"
#include <cmath>

namespace khensu {

VoiceRecorder::~VoiceRecorder() { stop(); }

juce::String VoiceRecorder::start(double maxSeconds)
{
    stop();
    deviceManager_ = std::make_unique<juce::AudioDeviceManager>();
    // Input only: one channel wanted, no outputs, no saved state. This is a
    // second manager next to PlaybackEngine's, so playback is untouched.
    const juce::String error = deviceManager_->initialise(1, 0, nullptr, true);
    auto* device = deviceManager_->getCurrentAudioDevice();
    if (device == nullptr || device->getActiveInputChannels().countNumberOfSetBits() == 0) {
        deviceManager_.reset();
        if (error.isNotEmpty()) return TRANS("Could not open the microphone: ") + error;
        return TRANS("No microphone was found. Connect or enable an audio input device (Windows Sound settings), "
                     "or type the instruction instead.");
    }
    sampleRate_ = device->getCurrentSampleRate();
    if (sampleRate_ <= 0.0) sampleRate_ = 48000.0;
    buffer_.assign((size_t)(juce::jmax(1.0, maxSeconds) * sampleRate_), 0.0f);
    written_.store(0, std::memory_order_release);
    level_.store(0.0f, std::memory_order_relaxed);
    recording_ = true;
    deviceManager_->addAudioCallback(this);
    return {};
}

std::vector<float> VoiceRecorder::stop()
{
    std::vector<float> out;
    if (deviceManager_ != nullptr) {
        // removeAudioCallback waits for a running callback to return, so the
        // buffer is safe to read afterwards.
        deviceManager_->removeAudioCallback(this);
        deviceManager_->closeAudioDevice();
        deviceManager_.reset();
    }
    if (recording_) {
        const size_t n = juce::jmin(written_.load(std::memory_order_acquire), buffer_.size());
        out.assign(buffer_.begin(), buffer_.begin() + (std::ptrdiff_t)n);
    }
    recording_ = false;
    buffer_.clear();
    buffer_.shrink_to_fit();
    written_.store(0, std::memory_order_release);
    level_.store(0.0f, std::memory_order_relaxed);
    return out;
}

void VoiceRecorder::audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                                     float* const* outputChannelData, int numOutputChannels,
                                                     int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr) juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);

    int live = 0;
    for (int ch = 0; ch < numInputChannels; ++ch)
        if (inputChannelData[ch] != nullptr) ++live;
    if (live == 0 || numSamples <= 0) return;

    size_t pos = written_.load(std::memory_order_relaxed);
    const size_t capacity = buffer_.size();
    const float gain = 1.0f / (float)live;
    float peak = 0.0f;
    for (int i = 0; i < numSamples; ++i) {
        float sum = 0.0f;
        for (int ch = 0; ch < numInputChannels; ++ch)
            if (inputChannelData[ch] != nullptr) sum += inputChannelData[ch][i];
        const float s = sum * gain;
        peak = juce::jmax(peak, std::abs(s));
        if (pos < capacity) buffer_[pos++] = s;
    }
    written_.store(pos, std::memory_order_release);
    level_.store(peak, std::memory_order_relaxed);
}

} // namespace khensu
