// tests/shared-core/test_audio_file_prober.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <khensu/session/AudioFileProber.h>
#include <juce_audio_formats/juce_audio_formats.h>

using namespace khensu;

namespace {
// Writes a 1-second 44.1kHz stereo 16-bit WAV sine to a temp file and returns it.
juce::File writeTempWav()
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory);
    auto file = dir.getChildFile("khensu_prober_test.wav");
    file.deleteFile();

    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    REQUIRE(stream != nullptr);

    const int sampleRate = 44100;
    const int channels = 2;
    const int bitDepth = 16;
    const int numSamples = sampleRate; // 1 second

    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(static_cast<double>(sampleRate))
                             .withNumChannels(channels)
                             .withBitsPerSample(bitDepth);
    auto writer = fmt.createWriterFor(stream, options);
    REQUIRE(writer != nullptr);

    juce::AudioBuffer<float> buffer(channels, numSamples);
    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < numSamples; ++i)
            buffer.setSample(ch, i, 0.25f * std::sin(2.0f * 3.14159265f * 440.0f * (float)i / (float)sampleRate));

    writer->writeFromAudioSampleBuffer(buffer, 0, numSamples);
    writer.reset(); // flush + close (also deletes the stream)
    return file;
}
} // namespace

TEST_CASE("AudioFileProber::probe fills metadata for a real WAV", "[prober]")
{
    auto wav = writeTempWav();

    AudioFileProber prober;
    auto result = prober.probe(wav.getFullPathName().toStdString());

    REQUIRE(result.is_ok());
    const AudioFileRef& ref = result.value();

    CHECK_FALSE(ref.id.empty());
    CHECK(ref.id.length() == 36); // dashed UUID v4: 8-4-4-4-12
    CHECK(ref.path == wav.getFullPathName().toStdString());
    CHECK(ref.format == "wav");
    CHECK(ref.sample_rate == 44100);
    CHECK(ref.channels == 2);
    CHECK(ref.bit_depth == 16);
    CHECK(ref.file_size_bytes > 0);
    CHECK_THAT(ref.duration_seconds, Catch::Matchers::WithinAbs(1.0, 0.05));

    wav.deleteFile();
}

TEST_CASE("AudioFileProber::probe errors on a missing file", "[prober]")
{
    AudioFileProber prober;
    auto result = prober.probe("Z:/definitely/not/here.wav");
    CHECK(result.is_err());
}

TEST_CASE("AudioFileProber::probe errors on a non-audio file", "[prober]")
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory);
    auto txt = dir.getChildFile("khensu_prober_not_audio.txt");
    txt.replaceWithText("this is not audio");

    AudioFileProber prober;
    auto result = prober.probe(txt.getFullPathName().toStdString());
    CHECK(result.is_err());

    txt.deleteFile();
}
