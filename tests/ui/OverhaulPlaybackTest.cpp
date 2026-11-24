// tests/ui/OverhaulPlaybackTest.cpp
// UI-6 / UI-5 / UI-9: the A/B offset text and its "matched" rule, the
// equal-power crossfade law, the live loudness integrator behind the desktop
// meters, and the preset bar's store and stepping.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>
#include "session/PlaybackEngine.h"
#include "session/PlaybackMeterTap.h"
#include "session/PlaybackSourceController.h"
#include "ui/MixMasterEngine/PresetStore.h"
#include "ui/shared/AbControlStrip.h"

using namespace khensu;

TEST_CASE("A/B offset: B against A, 'matched' only when A's loudness is known (UI-6)", "[ui6][ab]")
{
    PlaybackSourceController s;
    CHECK_FALSE(s.levelOffset(-12.0f).available);
    CHECK(ab_text::offsetLine(s.levelOffset(-12.0f)).contains("Fix something"));

    s.processedPath = "b.wav";
    auto o = s.levelOffset(-12.0f);
    CHECK(o.measuring); // B not measured yet: held muted
    CHECK(ab_text::offsetLine(o).contains("Measuring"));

    s.processedLufs = -13.8f;
    o = s.levelOffset(-12.0f);
    REQUIRE(o.bMinusADb.has_value());
    CHECK(*o.bMinusADb == Catch::Approx(-1.8f));
    CHECK(o.matched);
    CHECK(ab_text::offsetLine(o) == "B is -1.8 dB vs A (matched)");

    s.processedLufs = -10.5f;
    CHECK(ab_text::offsetLine(s.levelOffset(-12.0f)) == "B is +1.5 dB vs A (matched)");

    // A's loudness unknown: nothing to match B to - never "matched".
    o = s.levelOffset(std::nullopt);
    CHECK_FALSE(o.matched);
    CHECK_FALSE(o.bMinusADb.has_value());
    CHECK(ab_text::offsetLine(o).contains("not matched"));
}

TEST_CASE("A/B crossfade follows the equal-power law (UI-6)", "[ui6][crossfade]")
{
    float a = 0.0f, b = 0.0f;
    PlaybackEngine::equalPowerGains(0.0f, a, b);
    CHECK(a == 1.0f);
    CHECK(b == 0.0f);
    PlaybackEngine::equalPowerGains(1.0f, a, b);
    CHECK(a == 0.0f);
    CHECK(b == 1.0f);
    for (float t = 0.0f; t <= 1.0f; t += 0.05f) {
        PlaybackEngine::equalPowerGains(t, a, b);
        CHECK(a * a + b * b == Catch::Approx(1.0f).margin(1.0e-5)); // constant power through the fade
        CHECK(a >= 0.0f);
        CHECK(b >= 0.0f);
    }
    PlaybackEngine::equalPowerGains(0.5f, a, b);
    CHECK(a == Catch::Approx(std::sqrt(0.5f))); // -3 dB each at the midpoint
    CHECK(b == Catch::Approx(std::sqrt(0.5f)));
    PlaybackEngine::equalPowerGains(-1.0f, a, b); // clamped
    CHECK(a == 1.0f);
    CHECK(PlaybackEngine::kCrossfadeMs >= 40.0);
    CHECK(PlaybackEngine::kCrossfadeMs <= 60.0);
}

TEST_CASE("Live loudness: a full-scale 1 kHz sine in both channels reads about 0 LUFS and 0 dBTP (UI-5)", "[ui5][meters]")
{
    constexpr double sr = 48000.0;
    PlaybackMeterTap tap;
    tap.prepare(sr);
    juce::AudioBuffer<float> buffer(2, 480);
    double phase = 0.0;
    LiveLoudness live;
    std::array<PlaybackMeterTap::Bucket, 64> out{};
    for (int block = 0; block < 100; ++block) { // 1 s
        for (int i = 0; i < buffer.getNumSamples(); ++i) {
            const float x = (float)std::sin(phase);
            phase += juce::MathConstants<double>::twoPi * 997.0 / sr;
            buffer.setSample(0, i, x);
            buffer.setSample(1, i, x);
        }
        tap.process(juce::AudioSourceChannelInfo(&buffer, 0, buffer.getNumSamples()));
        const int n = tap.pop(out.data(), (int)out.size());
        for (int k = 0; k < n; ++k)
            live.add(out[(size_t)k]);
    }
    // BS.1770: a 0 dBFS ~1 kHz sine reads -3.01 LUFS per channel; two
    // channels add +3.01, so about 0 LUFS.
    CHECK(live.momentary() == Catch::Approx(0.0f).margin(0.6f));
    CHECK(live.integrated() == Catch::Approx(0.0f).margin(0.6f));
    CHECK(live.truePeakDb(0) == Catch::Approx(0.0f).margin(0.3f));
    CHECK(live.correlation() == Catch::Approx(1.0f).margin(0.001f));

    live.reset();
    CHECK(live.integrated() == LiveLoudness::kSilence);
}

TEST_CASE("Live loudness: opposite-polarity channels read -1 correlation (UI-5)", "[ui5][meters]")
{
    PlaybackMeterTap tap;
    tap.prepare(48000.0);
    juce::AudioBuffer<float> buffer(2, 4800);
    for (int i = 0; i < buffer.getNumSamples(); ++i) {
        const float x = 0.5f * (float)std::sin(i * 0.05);
        buffer.setSample(0, i, x);
        buffer.setSample(1, i, -x);
    }
    tap.process(juce::AudioSourceChannelInfo(&buffer, 0, buffer.getNumSamples()));
    std::array<PlaybackMeterTap::Bucket, 8> out{};
    LiveLoudness live;
    const int n = tap.pop(out.data(), (int)out.size());
    REQUIRE(n == 1);
    live.add(out[0]);
    CHECK(live.correlation() == Catch::Approx(-1.0f).margin(0.001f));
}

TEST_CASE("The inter-sample peak estimate sees peaks between samples (UI-5)", "[ui5][meters]")
{
    // fs/4 sine at 45 degrees: every sample is 0.707, the waveform reaches 1.0.
    PlaybackMeterTap::PeakState st;
    float peak = 0.0f;
    for (int i = 0; i < 64; ++i)
        peak = juce::jmax(peak, PlaybackMeterTap::interpolatedPeakOf(
                                    st, (float)std::sin(juce::MathConstants<double>::halfPi * i + 0.785398)));
    CHECK(peak > 0.85f);
    CHECK(peak < 1.1f);
}

TEST_CASE("Preset store: save, list, load and delete round-trip; arrows wrap (UI-9)", "[ui9][presets]")
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("presets", "");
    REQUIRE(dir.createDirectory());
    preset_store::Chain chain{preset_store::effect("Limiter", {{"threshold_db", -1.0f}})};
    CHECK_FALSE(preset_store::save("", chain, {}, dir).has_value());
    CHECK_FALSE(preset_store::save("Empty", {}, {}, dir).has_value());
    const auto saved = preset_store::save("Master safety", chain, {}, dir);
    REQUIRE(saved.has_value());
    const auto all = preset_store::list(dir);
    REQUIRE(all.size() == 1);
    CHECK(all[0].name == "Master safety");
    CHECK(all[0].effectCount == 1);
    const auto loaded = preset_store::load(all[0], dir);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].effect == "Limiter");
    CHECK(loaded[0].parameters.at("threshold_db") == Catch::Approx(-1.0f));
    CHECK(preset_store::remove(all[0], dir));
    CHECK(preset_store::list(dir).empty());
    dir.deleteRecursively();

    CHECK_FALSE(preset_store::factory().empty());
    preset_store::Entry factory;
    factory.name = preset_store::factory().front().first;
    factory.factory = true;
    CHECK_FALSE(preset_store::load(factory).empty());
    CHECK_FALSE(preset_store::remove(factory)); // factory presets are never deleted

    CHECK(preset_store::stepIndex(-1, 3, +1) == 0);
    CHECK(preset_store::stepIndex(-1, 3, -1) == 2);
    CHECK(preset_store::stepIndex(2, 3, +1) == 0);
    CHECK(preset_store::stepIndex(0, 3, -1) == 2);
    CHECK(preset_store::stepIndex(0, 0, +1) == -1);
}
