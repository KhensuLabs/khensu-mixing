// tests/shared-core/test_audio_analyzer.cpp
// Audit CQ-05: AudioAnalyzer previously discarded AudioFormatReader::read()'s
// success result and never checked for non-finite samples, so a truncated/
// corrupt read or NaN/Inf audio could still produce an apparently successful
// analysis. These pin the fail-closed behavior added for both cases.
#include <catch2/catch_test_macros.hpp>
#include <khensu/analysis/AudioAnalyzer.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>

using namespace khensu;

namespace {

juce::File tempWavPath(const char* name)
{
    auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(name);
    file.deleteFile();
    return file;
}

juce::File writeValidWav()
{
    auto file = tempWavPath("khensu_audio_analyzer_valid.wav");
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    REQUIRE(stream != nullptr);

    const int sampleRate = 44100;
    const int channels = 2;
    const int numSamples = sampleRate; // 1 second

    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(static_cast<double>(sampleRate))
                             .withNumChannels(channels)
                             .withBitsPerSample(16);
    auto writer = fmt.createWriterFor(stream, options);
    REQUIRE(writer != nullptr);

    juce::AudioBuffer<float> buffer(channels, numSamples);
    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < numSamples; ++i)
            buffer.setSample(ch, i, 0.25f * std::sin(2.0f * 3.14159265f * 440.0f * (float) i / (float) sampleRate));

    writer->writeFromAudioSampleBuffer(buffer, 0, numSamples);
    writer.reset();
    return file;
}

juce::File writeFloatWavWithNaN()
{
    auto file = tempWavPath("khensu_audio_analyzer_nan.wav");
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    REQUIRE(stream != nullptr);

    const int sampleRate = 44100;
    const int channels = 2;
    const int numSamples = 1000;

    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(static_cast<double>(sampleRate))
                             .withNumChannels(channels)
                             .withBitsPerSample(32)
                             .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto writer = fmt.createWriterFor(stream, options);
    REQUIRE(writer != nullptr);

    juce::AudioBuffer<float> buffer(channels, numSamples);
    buffer.clear();
    buffer.setSample(0, 500, std::numeric_limits<float>::quiet_NaN());

    writer->writeFromAudioSampleBuffer(buffer, 0, numSamples);
    writer.reset();
    return file;
}

} // namespace

TEST_CASE("AudioAnalyzer::analyze succeeds on a valid file", "[audio_analyzer]")
{
    auto wav = writeValidWav();
    AudioFileRef ref;
    ref.id = "f1";
    ref.path = wav.getFullPathName().toStdString();

    AudioAnalyzer analyzer;
    auto result = analyzer.analyze(ref, "session-1");
    REQUIRE(result.is_ok());
    // UI-13: the measured rate must be published on the result so UI code
    // (HeatSpectrumComponent's FFT-bin-to-Hz mapping) can stop assuming 44100.
    CHECK(result.value().sample_rate == 44100.0);
}

// Note: a genuinely truncated WAV file is not exercisable through JUCE's own
// WAV reader — juce_WavAudioFormat.cpp's readSamples() zero-pads any bytes
// missing past physical EOF and unconditionally returns true, by design. The
// read()-return-value check added alongside the NaN guard below still
// matters for other decoders (e.g. FLAC/OGG) that do surface a genuine I/O
// failure as false; there is just no way to pin that specific case against
// the WAV codec used here.

TEST_CASE("AudioAnalyzer::analyze rejects non-finite (NaN) audio samples", "[audio_analyzer]")
{
    auto wav = writeFloatWavWithNaN();
    AudioFileRef ref;
    ref.id = "f3";
    ref.path = wav.getFullPathName().toStdString();

    AudioAnalyzer analyzer;
    auto result = analyzer.analyze(ref, "session-1");
    CHECK(result.is_err());
}
