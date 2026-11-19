// Deterministic end-to-end regression fixture for the shared C++ analyzer
// pipeline. Unlike individual analyzer unit tests, this pins the values that
// result after a real file is decoded and passed through every fast analyzer.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <khensu/analysis/AudioAnalyzer.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <memory>

using namespace khensu;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kNumSamples = 480000; // ten seconds for stable loudness gating
constexpr double kPi = 3.14159265358979323846;

juce::File writeGoldenFixture(juce::TemporaryFile& temporaryFile)
{
    juce::AudioBuffer<float> buffer(2, kNumSamples);
    for (int i = 0; i < kNumSamples; ++i) {
        const auto sample = static_cast<float>(0.25 * std::sin(2.0 * kPi * 1000.0 * i / kSampleRate));
        buffer.setSample(0, i, sample);
        buffer.setSample(1, i, sample);
    }

    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> output = temporaryFile.getFile().createOutputStream();
    REQUIRE(output != nullptr);
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(kSampleRate)
                             .withNumChannels(2)
                             .withBitsPerSample(24);
    auto writer = format.createWriterFor(output, options);
    REQUIRE(writer != nullptr);
    writer->writeFromAudioSampleBuffer(buffer, 0, kNumSamples);
    writer.reset();
    return temporaryFile.getFile();
}

} // namespace

TEST_CASE("AudioAnalyzer: deterministic stereo sine golden baseline", "[audio_analyzer][golden]")
{
    juce::TemporaryFile temporaryFile(".wav");
    const auto file = writeGoldenFixture(temporaryFile);
    AudioFileRef reference;
    reference.id = "golden-stereo-sine";
    reference.path = file.getFullPathName().toStdString();

    AudioAnalyzer analyzer;
    const auto analysis = analyzer.analyze(reference, "golden-session");
    REQUIRE(analysis.is_ok());
    const auto& result = analysis.value();

    CHECK(result.sample_rate == kSampleRate);
    CHECK(result.channel_layout.layout_name == "Stereo");
    CHECK_THAT(result.loudness.lufs_integrated, Catch::Matchers::WithinAbs(-12.076f, 0.15f));
    CHECK_THAT(result.loudness.true_peak_dbtp, Catch::Matchers::WithinAbs(-12.0412f, 0.15f));
    // librosa's magnitude-weighted centroid (AUDIT-054): a 1 kHz tone reads
    // ~1000 Hz (librosa.feature.spectral_centroid gives 1001.6 on this
    // signal). The old power-weighted octave-centre estimate read 1144.61.
    CHECK_THAT(result.spectral.centroid_hz, Catch::Matchers::WithinAbs(1000.0f, 5.0f));
    CHECK_THAT(result.dynamics.crest_factor_db, Catch::Matchers::WithinAbs(3.0103f, 0.05f));
    CHECK_THAT(result.stereo.width, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
    CHECK_THAT(result.stereo.correlation, Catch::Matchers::WithinAbs(1.0f, 0.0001f));
}

#include "TestNoise.h"

TEST_CASE("AudioAnalyzer: pink-noise WAV tilt matches the Python definition", "[audio_analyzer][golden][audit]")
{
    // Cross-language parity (AUDIT-009): the file is decoded and analysed end
    // to end, and the C++ tilt must land on the analytic value that Python's
    // dsp_metrics.spectral_tilt_db_per_octave reports for pink noise (~0
    // dB/oct; the old C++ convention read -3). White noise must read ~+3.
    for (double exponent : {1.0, 0.0}) {
        INFO("1/f^" << exponent);
        const auto noise = khensu::test::colouredNoise(19, exponent, 5u, 0.1);
        juce::AudioBuffer<float> buffer(2, static_cast<int>(noise.size()));
        for (int i = 0; i < buffer.getNumSamples(); ++i) {
            buffer.setSample(0, i, noise[static_cast<size_t>(i)]);
            buffer.setSample(1, i, noise[static_cast<size_t>(i)]);
        }
        juce::TemporaryFile temporaryFile(".wav");
        {
            juce::WavAudioFormat format;
            std::unique_ptr<juce::OutputStream> output = temporaryFile.getFile().createOutputStream();
            REQUIRE(output != nullptr);
            const auto options = juce::AudioFormatWriterOptions{}
                                     .withSampleRate(kSampleRate)
                                     .withNumChannels(2)
                                     .withBitsPerSample(24);
            auto writer = format.createWriterFor(output, options);
            REQUIRE(writer != nullptr);
            writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
        }
        AudioFileRef ref;
        ref.id = "pink";
        ref.path = temporaryFile.getFile().getFullPathName().toStdString();
        const auto analysis = AudioAnalyzer{}.analyze(ref, "golden-session");
        REQUIRE(analysis.is_ok());
        const float expected = exponent == 1.0 ? 0.0f : 3.0103f;
        CHECK_THAT(analysis.value().spectral.tilt_db_per_octave, Catch::Matchers::WithinAbs(expected, 0.35f));
    }
}
