// tests/shared-core/test_music_theory.cpp
// AUDIT-084 / AUDIT-050: the C++ chord fallback was sharps-only, read Am7 as
// "C", and numbered measures by counting only bars with notes (so they
// drifted); batch BPM was an arithmetic mean of octave-mismatched tempi.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <khensu/analysis/MusicTheory.h>

using namespace khensu::theory;
using Catch::Matchers::WithinAbs;

TEST_CASE("parseKey reads the service's key strings and their signature", "[music_theory]")
{
    auto am = parseKey("A minor");
    REQUIRE(am.has_value());
    CHECK(am->tonic_pc == 9);
    CHECK(am->minor);
    CHECK_FALSE(am->prefer_flats);

    auto bb = parseKey("Bb major");
    REQUIRE(bb.has_value());
    CHECK(bb->tonic_pc == 10);
    CHECK_FALSE(bb->minor);
    CHECK(bb->prefer_flats);

    auto dm = parseKey("D", "minor");
    REQUIRE(dm.has_value());
    CHECK(dm->prefer_flats); // relative of F major

    auto fs = parseKey("F#m");
    REQUIRE(fs.has_value());
    CHECK(fs->tonic_pc == 6);
    CHECK(fs->minor);
    CHECK_FALSE(fs->prefer_flats);

    CHECK(parseKey("F")->prefer_flats);
    CHECK_FALSE(parseKey("G major")->prefer_flats);
    CHECK_FALSE(parseKey("unknown").has_value());
    CHECK_FALSE(parseKey("").has_value());
    CHECK_FALSE(parseKey("N").has_value());
}

TEST_CASE("nameChord detects seventh chords before the triad inside them", "[music_theory]")
{
    // A2 C3 E3 G3: Am7, not the C major triad it contains.
    CHECK(nameChord({45, 48, 52, 55}, parseKey("C major")) == "Am7");
    CHECK(nameChord({43, 47, 50, 53}, parseKey("C major")) == "G7");
    CHECK(nameChord({48, 52, 55, 59}, parseKey("C major")) == "Cmaj7");
    CHECK(nameChord({47, 50, 53, 57}, parseKey("C major")) == "Bm7b5");
    // Plain triads still read as before.
    CHECK(nameChord({48, 52, 55}, std::nullopt) == "C");
    CHECK(nameChord({57, 60, 64}, std::nullopt) == "Am");
    // Inversion: C triad over E.
    CHECK(nameChord({52, 55, 60}, std::nullopt) == "C/E");
    CHECK(nameChord({}, std::nullopt).empty());
}

TEST_CASE("nameChord spells with flats in flat keys", "[music_theory]")
{
    // Bb D F in Bb major / F major / G minor: "Bb", never "A#".
    CHECK(nameChord({46, 50, 53}, parseKey("Bb major")) == "Bb");
    CHECK(nameChord({46, 50, 53}, parseKey("F major")) == "Bb");
    CHECK(nameChord({46, 50, 53}, parseKey("G minor")) == "Bb");
    CHECK(nameChord({51, 54, 58}, parseKey("Eb minor")) == "Ebm");
    // Sharp keys and unknown keys keep sharps.
    CHECK(nameChord({54, 58, 61}, parseKey("B major")) == "F#");
    CHECK(nameChord({46, 50, 53}, std::nullopt) == "A#");
}

TEST_CASE("segmentMeasures numbers bars from the downbeat grid without drift", "[music_theory]")
{
    // Pickup before the first downbeat, bars every 2 s; bar 3 may be silent
    // but still counts.
    const auto bars = segmentMeasures({0.5, 2.5, 4.5, 6.5}, {}, 4, 8.5);
    REQUIRE(bars.size() == 5);
    CHECK(bars[0].number == 0); // pickup
    CHECK_THAT(bars[0].end_sec, WithinAbs(0.5, 1e-9));
    CHECK(bars[1].number == 1);
    CHECK(bars[3].number == 3);
    CHECK(bars[4].number == 4);
    CHECK_THAT(bars[4].start_sec, WithinAbs(6.5, 1e-9));
    CHECK_THAT(bars[4].end_sec, WithinAbs(8.5, 1e-9));
}

TEST_CASE("segmentMeasures without downbeats groups beats by meter but leaves bars unnumbered",
          "[music_theory]")
{
    // 3/4 at 120 bpm: beats every 0.5 s, so 1.5 s windows.
    std::vector<double> beats;
    for (int i = 0; i < 12; ++i)
        beats.push_back(0.5 * i);
    const auto bars = segmentMeasures({}, beats, beatsPerBar("3/4"), 6.0);
    REQUIRE(bars.size() == 4);
    CHECK_THAT(bars[1].start_sec, WithinAbs(1.5, 1e-9));
    for (const auto& b : bars)
        CHECK(b.number == -1);

    // Nothing at all: fixed windows, unnumbered.
    const auto windows = segmentMeasures({}, {}, 0, 5.0, 2.0);
    REQUIRE(windows.size() == 3);
    CHECK(windows[2].number == -1);
    CHECK_THAT(windows[2].end_sec, WithinAbs(5.0, 1e-9));
}

TEST_CASE("beatsPerBar parses time signatures", "[music_theory]")
{
    CHECK(beatsPerBar("4/4") == 4);
    CHECK(beatsPerBar("3/4") == 3);
    CHECK(beatsPerBar("6/8") == 6);
    CHECK(beatsPerBar("") == 0);
    CHECK(beatsPerBar("x") == 0);
}

TEST_CASE("foldedTempoConsensus folds octave-mismatched detections", "[music_theory]")
{
    // 70 + 140 used to average to 105 - a tempo neither file has.
    auto tc = foldedTempoConsensus({70.0f, 140.0f});
    REQUIRE(tc.has_value());
    CHECK_THAT(tc->bpm, WithinAbs(70.0f, 0.01f));
    CHECK_THAT(tc->agreement, WithinAbs(1.0f, 1e-6f));

    // Majority octave wins: two files at ~128, one at the half-time 64.
    auto tc2 = foldedTempoConsensus({64.0f, 128.0f, 128.0f});
    REQUIRE(tc2.has_value());
    CHECK_THAT(tc2->bpm, WithinAbs(128.0f, 0.01f));

    // Genuinely different tempi: median, and low agreement.
    auto tc3 = foldedTempoConsensus({120.0f, 121.0f, 90.0f});
    REQUIRE(tc3.has_value());
    CHECK_THAT(tc3->bpm, WithinAbs(120.0f, 0.01f));
    CHECK(tc3->agreeing.size() == 2);

    CHECK_FALSE(foldedTempoConsensus({0.0f, -1.0f}).has_value());
}
