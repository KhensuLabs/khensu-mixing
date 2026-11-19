// tests/shared-core/test_perf_bench.cpp
// Fast-analysis throughput budgets for AudioAnalyzer (loudness + spectral +
// dynamics + stereo + artifacts in one file pass). Hidden tag: run with
//   KhensuCoreTests.exe "[.bench]"
// Budgets are ~3-4x the 2026-10-17 baseline on an i7-11700K; see
// docs/archive/2026-10-17-performance-baseline.md. Each case writes a fresh WAV of
// a synthetic mix (chords, kick/snare, noise) so analyzers can't exit early.
#include <catch2/catch_test_macros.hpp>
#include <khensu/analysis/AudioAnalyzer.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>

using namespace khensu;

namespace {

juce::File writeBenchMix(double seconds, int sampleRate, unsigned seed)
{
    auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                    .getChildFile("khensu_bench_" + juce::String(static_cast<int>(seconds)) + "s_" +
                                  juce::String(sampleRate) + "_" + juce::String(seed) + ".wav");
    file.deleteFile();
    const int n = static_cast<int>(seconds * sampleRate);
    juce::AudioBuffer<float> buf(2, n);
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    const double beat = 0.5; // 120 BPM
    const double roots[] = {110.0, 87.31, 130.81, 98.0};
    double phase = 0.0;
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const double root = roots[static_cast<int>(t / (4 * beat)) % 4];
        phase += 2.0 * juce::MathConstants<double>::pi * root / sampleRate;
        double l = 0.12 * std::sin(phase) + 0.08 * std::sin(1.26 * phase) + 0.08 * std::sin(1.5 * phase);
        double r = 0.12 * std::sin(phase + 0.3) + 0.08 * std::sin(1.26 * phase + 0.3) + 0.08 * std::sin(1.5 * phase);
        const double tb = std::fmod(t, beat);
        if (tb < 0.12) {
            const double kick = 0.6 * std::sin(2 * juce::MathConstants<double>::pi * (50 + 80 * std::exp(-tb * 30)) * tb) *
                                std::exp(-tb * 25);
            l += kick;
            r += kick;
        }
        l += 0.01 * noise(rng);
        r += 0.01 * noise(rng);
        const double swell = 0.8 + 0.2 * std::sin(2 * juce::MathConstants<double>::pi * t / 60.0);
        buf.setSample(0, i, static_cast<float>(juce::jlimit(-0.99, 0.99, l * swell * 0.7)));
        buf.setSample(1, i, static_cast<float>(juce::jlimit(-0.99, 0.99, r * swell * 0.7)));
    }
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    REQUIRE(stream != nullptr);
    auto writer = fmt.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                  .withSampleRate(static_cast<double>(sampleRate))
                                                  .withNumChannels(2)
                                                  .withBitsPerSample(24));
    REQUIRE(writer != nullptr);
    writer->writeFromAudioSampleBuffer(buf, 0, n);
    writer.reset();
    return file;
}

/// Best-of-two wall time of AudioAnalyzer::analyze on fresh files.
double bestAnalyzeSeconds(double seconds, int sampleRate)
{
    double best = 1e9;
    for (unsigned rep = 0; rep < 2; ++rep) {
        const auto wav = writeBenchMix(seconds, sampleRate, rep + 1);
        AudioFileRef ref;
        ref.id = "bench";
        ref.path = wav.getFullPathName().toStdString();
        AudioAnalyzer analyzer;
        const auto t0 = std::chrono::steady_clock::now();
        const auto result = analyzer.analyze(ref, "bench");
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        REQUIRE(result.is_ok());
        wav.deleteFile();
        best = std::min(best, s);
    }
    return best;
}

} // namespace

TEST_CASE("Bench: AudioAnalyzer fast-analysis throughput stays within budget", "[.bench][perf]")
{
    struct Case {
        double seconds;
        int sampleRate;
        double budgetSeconds;
    };
    // Budgets: ~3-4x the measured baseline (see the doc above).
    // Baseline 0.32-0.34 / 0.36 / 1.98 / 2.06 / 7.0-11.7 s (~85-90x realtime;
    // the 10 min case varies most with machine load).
    const Case cases[] = {{30.0, 44100, 1.5}, {30.0, 48000, 1.5}, {180.0, 44100, 8.0},
                          {180.0, 48000, 8.0}, {600.0, 48000, 40.0}};
    for (const auto& c : cases) {
        const double s = bestAnalyzeSeconds(c.seconds, c.sampleRate);
        std::cout << "[bench] AudioAnalyzer " << c.seconds << " s @ " << c.sampleRate << " Hz: " << s
                  << " s (" << (c.seconds / s) << "x realtime, budget " << c.budgetSeconds << " s)\n";
        CHECK(s <= c.budgetSeconds);
    }
}
