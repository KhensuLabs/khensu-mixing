// plugin-copilot/Source/CaptureToDesktop.h
// FEAT-7: writes a finished capture for the desktop app - a 32-bit float WAV
// and its manifest in <data dir>/captures/ (khensu/session/PluginCapture.h).
// The desktop app makes a session of it; the plugin never writes khensu.db.
// THREAD_CONTEXT(WorkerThread) - file writes.
#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <khensu/session/PluginCapture.h>

namespace khensu::copilot::capture_to_desktop {

/// Writes the first `samples` samples of `audio`. `label` names the session
/// ("8 bars", "Loop"). Returns the manifest, or a failed Result.
inline juce::Result write(const juce::AudioBuffer<float>& audio, juce::int64 samples, double sampleRate, double bpm,
                          const juce::String& followedSessionId, const juce::String& label, juce::File& manifestOut)
{
    if (samples <= 0 || sampleRate <= 0.0) return juce::Result::fail("Nothing was recorded.");
    const auto dir = plugin_capture::capturesDirectory();
    const auto now = juce::Time::getCurrentTime();
    const auto stem = "capture_" + now.formatted("%Y%m%d-%H%M%S");
    const auto wav = dir.getNonexistentChildFile(stem, ".wav", false);

    {
        std::unique_ptr<juce::OutputStream> stream = wav.createOutputStream();
        if (stream == nullptr) return juce::Result::fail("Could not write to " + dir.getFullPathName());
        juce::WavAudioFormat format;
        auto writer = format.createWriterFor(
            stream, juce::AudioFormatWriterOptions{}
                        .withSampleRate(sampleRate)
                        .withNumChannels(audio.getNumChannels())
                        .withBitsPerSample(32)
                        .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if (writer == nullptr) return juce::Result::fail("Could not create the WAV writer.");
        if (!writer->writeFromAudioSampleBuffer(
                audio, 0, static_cast<int>(juce::jmin<juce::int64>(samples, audio.getNumSamples()))))
            return juce::Result::fail("Writing the capture failed.");
    }

    plugin_capture::Manifest m;
    m.audioFile = wav.getFileName();
    m.name = "Plugin capture " + now.formatted("%H:%M") + " (" + label + ")";
    m.followedSessionId = followedSessionId;
    m.bpm = bpm;
    m.sampleRate = sampleRate;
    m.channels = audio.getNumChannels();
    m.seconds = static_cast<double>(samples) / sampleRate;
    m.createdAt = now.toISO8601(true);
    // Written last, under its final name: the desktop app only sees a
    // manifest whose audio is complete.
    const auto tmp = wav.withFileExtension(".tmp");
    if (!tmp.replaceWithText(plugin_capture::toJson(m))) return juce::Result::fail("Could not write the manifest.");
    manifestOut = wav.getSiblingFile(wav.getFileNameWithoutExtension() + plugin_capture::kPendingSuffix);
    if (!tmp.moveFileTo(manifestOut)) return juce::Result::fail("Could not write the manifest.");
    return juce::Result::ok();
}

} // namespace khensu::copilot::capture_to_desktop
