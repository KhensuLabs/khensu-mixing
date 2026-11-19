#include <catch2/catch_test_macros.hpp>
#include <khensu/analysis/AudioAnalyzer.h>
#include <khensu/analysis/ChannelLayout.h>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cmath>
#include <memory>
#include <utility>
#include <string>

using namespace khensu;

namespace {

constexpr double kPiLocal = 3.14159265358979323846;

/// Writes a short WAV with `numChannels` channels of quiet sine into `tmp`.
juce::File writeWav(juce::TemporaryFile& tmp, int numChannels, double fs = 48000.0,
                    int numSamples = 48000)
{
    juce::AudioBuffer<float> buffer(numChannels, numSamples);
    buffer.clear();
    for (int c = 0; c < numChannels; ++c)
        for (int i = 0; i < numSamples; ++i)
            buffer.setSample(c, i,
                             static_cast<float>(0.25 * std::sin(2.0 * kPiLocal * 440.0 * i / fs)));

    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::OutputStream> out = tmp.getFile().createOutputStream();
    REQUIRE(out != nullptr);
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(fs)
                             .withNumChannels(numChannels)
                             .withBitsPerSample(24);
    // Takes std::unique_ptr<OutputStream>& and assumes ownership on success.
    std::unique_ptr<juce::AudioFormatWriter> writer = fmt.createWriterFor(out, options);
    REQUIRE(writer != nullptr);
    writer->writeFromAudioSampleBuffer(buffer, 0, numSamples);
    writer.reset(); // flush + close before the analyzer reads the file back
    return tmp.getFile();
}

AudioFileRef refFor(const juce::File& f)
{
    AudioFileRef r;
    r.id = "test-file";
    r.path = f.getFullPathName().toStdString();
    return r;
}

} // namespace

TEST_CASE("AudioAnalyzer: rejects an unsupported channel layout before measuring",
          "[audio_analyzer][layout]")
{
    juce::TemporaryFile tmp(".wav");
    const auto file = writeWav(tmp, 4); // quad - deliberately unsupported

    AudioAnalyzer analyzer;
    auto result = analyzer.analyze(refFor(file), "session-1");

    REQUIRE(result.is_err());
    // The message must be actionable: name the count and the supported set.
    CHECK(result.error().find("4") != std::string::npos);
    CHECK(result.error().find("Mono") != std::string::npos);
}

TEST_CASE("AudioAnalyzer: accepts stereo and reports the layout", "[audio_analyzer][layout]")
{
    juce::TemporaryFile tmp(".wav");
    const auto file = writeWav(tmp, 2);

    AudioAnalyzer analyzer;
    auto result = analyzer.analyze(refFor(file), "session-1");

    REQUIRE(result.is_ok());
    CHECK(result.value().channel_layout.layout_name == "Stereo");
    CHECK(result.value().channel_layout.channel_count == 2);
    CHECK_FALSE(result.value().channel_layout.lfe_excluded);
}

TEST_CASE("AudioAnalyzer: accepts 5.1 and marks LFE as excluded", "[audio_analyzer][layout]")
{
    juce::TemporaryFile tmp(".wav");
    const auto file = writeWav(tmp, 6);

    AudioAnalyzer analyzer;
    auto result = analyzer.analyze(refFor(file), "session-1");

    REQUIRE(result.is_ok());
    CHECK(result.value().channel_layout.layout_name == "5.1");
    CHECK(result.value().channel_layout.channel_count == 6);
    CHECK(result.value().channel_layout.lfe_excluded);
}
