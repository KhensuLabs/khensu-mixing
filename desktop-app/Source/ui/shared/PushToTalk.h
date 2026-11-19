// desktop-app/Source/ui/shared/PushToTalk.h
#pragma once
#include <JuceHeader.h>
#include <vector>

namespace khensu::push_to_talk {

/// CE-AG-4 push-to-talk state machine (pure, unit-tested). Two ways to talk:
///   * hold: press the mic button, speak, release -> send;
///   * tap:  a press shorter than kTapMs latches recording on, the next
///           press (or a keyboard / accessibility activation) stops and sends.
/// Keyboard activation (Space / Enter on the focused button) toggles.
class Machine {
  public:
    enum class State { Idle, Recording, Transcribing };
    enum class Action { None, StartRecording, StopAndSend };

    /// A press shorter than this is a tap (latch) rather than a hold.
    static constexpr double kTapMs = 300.0;

    State state() const noexcept { return state_; }
    bool latched() const noexcept { return latched_; }

    Action press(double nowMs) noexcept
    {
        if (state_ == State::Idle) {
            state_ = State::Recording;
            pressMs_ = nowMs;
            latched_ = false;
            ignoreRelease_ = false;
            return Action::StartRecording;
        }
        if (state_ == State::Recording && latched_) { // second tap stops
            ignoreRelease_ = true;
            return stopAndSend();
        }
        return Action::None;
    }

    Action release(double nowMs) noexcept
    {
        if (ignoreRelease_) {
            ignoreRelease_ = false;
            return Action::None;
        }
        if (state_ != State::Recording || latched_) return Action::None;
        if (nowMs - pressMs_ < kTapMs) {
            latched_ = true; // a tap: keep recording until the next tap
            return Action::None;
        }
        return stopAndSend();
    }

    /// Keyboard / accessibility activation of the button: a toggle.
    Action activate() noexcept
    {
        if (state_ == State::Idle) {
            state_ = State::Recording;
            latched_ = true;
            ignoreRelease_ = false;
            return Action::StartRecording;
        }
        if (state_ == State::Recording) return stopAndSend();
        return Action::None;
    }

    /// Recording could not start (no input device) or the request finished.
    void reset() noexcept
    {
        state_ = State::Idle;
        latched_ = false;
        ignoreRelease_ = false;
    }

  private:
    Action stopAndSend() noexcept
    {
        state_ = State::Transcribing;
        latched_ = false;
        return Action::StopAndSend;
    }

    State state_ = State::Idle;
    double pressMs_ = 0.0;
    bool latched_ = false;
    bool ignoreRelease_ = false;
};

/// Shortest recording worth sending (seconds).
inline constexpr double kMinSeconds = 0.4;
/// Longest recording kept (seconds); the recorder stops filling after this.
inline constexpr double kMaxSeconds = 30.0;

/// Writes mono float samples as a 16-bit PCM WAV. Returns "" on success or
/// the reason it failed. Pure apart from the file IO.
inline juce::String writeWav(const juce::File& file, const std::vector<float>& samples, double sampleRate)
{
    if (sampleRate <= 0.0) return "invalid sample rate";
    if (samples.empty()) return "nothing was recorded";
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> out = file.createOutputStream();
    if (out == nullptr) return "could not create " + file.getFullPathName();
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(
        out, juce::AudioFormatWriterOptions{}.withSampleRate(sampleRate).withNumChannels(1).withBitsPerSample(16));
    if (writer == nullptr) return "could not create a WAV writer";
    const float* channels[] = {samples.data()};
    if (!writer->writeFromFloatArrays(channels, 1, (int)samples.size())) return "could not write the recording";
    return {};
}

/// User-facing message for a failed /command/voice call, by HTTP status.
inline juce::String voiceErrorMessage(int httpStatus, const juce::String& detail)
{
    // 503 is both "optional extra missing" and "shutting down": only the
    // former names the ASR extra in its detail.
    if (httpStatus == 503 && (detail.containsIgnoreCase("asr") || detail.containsIgnoreCase("whisper")))
        return TRANS("Speech recognition is not available, so type the instruction instead. Voice commands need "
                     "the service's [asr] extra (faster-whisper).")
               + " (" + detail + ")";
    if (httpStatus == 503)
        return detail.isNotEmpty() ? detail : TRANS("The service is not ready for voice commands.");
    if (httpStatus == 422)
        return detail.isNotEmpty() ? detail : TRANS("No speech was recognised in the recording.");
    if (httpStatus == 403)
        return TRANS("Downloading the speech model is turned off in Settings (online features).");
    return detail.isNotEmpty() ? detail : TRANS("The voice command failed.");
}

} // namespace khensu::push_to_talk
