// tests/ui/ChordChartTest.cpp
// Chord chart / chord MIDI export helpers behind the Chords panel.
#include <catch2/catch_test_macros.hpp>
#include "ui/Dashboard/ChordChart.h"

using namespace khensu;

namespace {
ChordAnalysis makeProgression()
{
    ChordAnalysis ca;
    ca.key = "C";
    ca.mode = "major";
    ca.chords = {
        {0.0f, "C", 0.9f, "I", true, true},     {2.0f, "C", 0.9f, "I", true, true},
        {4.0f, "F", 0.8f, "IV", true, true},    {5.0f, "G", 0.8f, "V", true, true},
        {6.0f, "N.C.", 0.0f, "", true, false},  {8.0f, "Bb", 0.7f, "bVII", false, true},
    };
    return ca;
}
} // namespace

TEST_CASE("chordchart: time signature parsing falls back to 4/4", "[chordchart]")
{
    CHECK(chordchart::parseTimeSignature("6/8").numerator == 6);
    CHECK(chordchart::parseTimeSignature("6/8").denominator == 8);
    CHECK(chordchart::parseTimeSignature("3/4").numerator == 3);
    CHECK(chordchart::parseTimeSignature("").numerator == 4);
    CHECK(chordchart::parseTimeSignature("garbage").denominator == 4);
}

TEST_CASE("chordchart: chord symbols voice in a mid register", "[chordchart]")
{
    auto am = chordchart::chordPitchClasses("Am");
    REQUIRE(am.has_value());
    CHECK(*am == std::vector<int>{9, 0, 4});
    const auto notes = chordchart::voiceChord(*am);
    REQUIRE(notes.size() == 3);
    CHECK(notes[0] == 57); // A3
    CHECK(notes[1] == 60); // C4
    CHECK(notes[2] == 64); // E4

    CHECK(chordchart::chordPitchClasses("Bbm7").has_value());
    CHECK(chordchart::chordPitchClasses("F#dim").has_value());
    CHECK_FALSE(chordchart::chordPitchClasses("N.C.").has_value());
    CHECK_FALSE(chordchart::chordPitchClasses("Hmaj").has_value());
}

TEST_CASE("chordchart: chart is bar-aligned with Roman numerals and repeat bars", "[chordchart]")
{
    chordchart::ChartContext ctx;
    ctx.title = "Song";
    ctx.key = "C major";
    ctx.timeSignature = {4, 4};
    ctx.bpm = 120.0;                         // 2 s bars
    ctx.downbeats = {0.0, 2.0, 4.0, 6.0, 8.0}; // explicit downbeats win
    const auto chart = chordchart::buildChart(makeProgression(), ctx);

    CHECK(chart.contains("Key: C major"));
    CHECK(chart.contains("Time: 4/4"));
    CHECK(chart.contains("| C (I) |"));       // bar 1
    CHECK(chart.contains("| % |"));           // bar 2 repeats (C collapsed)
    CHECK(chart.contains("F (IV)  G (V)"));   // bar 3 holds two changes
    CHECK(chart.contains("Bb (bVII)"));
}

TEST_CASE("chordchart: MIDI export carries tempo + time signature and block chords", "[chordchart]")
{
    chordchart::ChartContext ctx;
    ctx.timeSignature = {3, 4};
    ctx.bpm = 90.0;
    const auto midi = chordchart::buildMidi(makeProgression(), ctx);

    REQUIRE(midi.getNumTracks() == 2);
    bool tempo = false, timeSig = false;
    const auto* meta = midi.getTrack(0);
    for (int i = 0; i < meta->getNumEvents(); ++i) {
        const auto& m = meta->getEventPointer(i)->message;
        if (m.isTempoMetaEvent()) {
            tempo = true;
            CHECK(std::abs(m.getTempoSecondsPerQuarterNote() - 60.0 / 90.0) < 1.0e-3);
        }
        if (m.isTimeSignatureMetaEvent()) {
            int n = 0, d = 0;
            m.getTimeSignatureInfo(n, d);
            timeSig = n == 3 && d == 4;
        }
    }
    CHECK(tempo);
    CHECK(timeSig);

    int noteOns = 0;
    const auto* chords = midi.getTrack(1);
    for (int i = 0; i < chords->getNumEvents(); ++i)
        if (chords->getEventPointer(i)->message.isNoteOn()) ++noteOns;
    // C, F, G, Bb triads (C repeat collapsed, N.C. silent) = 4 x 3 notes.
    CHECK(noteOns == 12);
}
