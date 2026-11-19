// tests/ui/VocalEditingModelsTest.cpp
// Pure-logic coverage for the vocal editing tools: pitch note grid,
// per-word clip gain, and doubles/ad-lib timing alignment.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "ui/VocalWorkspace/PitchNoteGridModel.h"
#include "ui/VocalWorkspace/TakeAlignmentModel.h"
#include "ui/VocalWorkspace/WordClipGainModel.h"

using Catch::Approx;
using namespace khensu;

namespace {
juce::var parse(const char* json) { return juce::JSON::parse(juce::String(json)); }

juce::var pitchResponse()
{
    return parse(R"({"duration_sec": 3.0, "hop_sec": 0.5,
        "f0_midi": [57.3, 57.2, null, 64.6, 64.5, null],
        "notes": [
          {"id": 1, "start_sec": 1.5, "end_sec": 2.5, "midi": 64.6, "target_midi": 65},
          {"id": 0, "start_sec": 0.0, "end_sec": 1.0, "midi": 57.25, "target_midi": 57},
          {"id": 2, "start_sec": 2.6, "end_sec": 2.6, "midi": 60.0, "target_midi": 60}
        ]})");
}
} // namespace

TEST_CASE("Pitch note grid parses notes, curve gaps, and sorts by time", "[vocal-edit][pitch]")
{
    PitchNoteGridModel m;
    REQUIRE(m.loadFromResponse(pitchResponse()));
    REQUIRE(m.notes().size() == 2); // zero-length note dropped
    CHECK(m.notes()[0].id == 0);
    CHECK(m.curve().size() == 6);
    CHECK_FALSE(m.curve()[2].has_value());
    CHECK(*m.curve()[3] == Approx(64.6));
    CHECK(m.noteAt(0.5) == 0);
    CHECK(m.noteAt(1.2) == -1);
    CHECK(m.corrections().empty());
}

TEST_CASE("Retune amount pulls notes toward the nearest semitone", "[vocal-edit][pitch]")
{
    PitchNoteGridModel m;
    REQUIRE(m.loadFromResponse(pitchResponse()));
    m.retuneAll(1.0);
    CHECK(m.notes()[0].correctedMidi() == Approx(57.0));
    CHECK(m.notes()[1].correctedMidi() == Approx(65.0));
    m.retuneAll(0.5);
    CHECK(m.notes()[0].shift == Approx(-0.125));
    m.retuneAll(0.0);
    CHECK(m.corrections().empty());
}

TEST_CASE("Note nudges snap to semitones, clamp, and reset", "[vocal-edit][pitch]")
{
    PitchNoteGridModel m;
    REQUIRE(m.loadFromResponse(pitchResponse()));
    REQUIRE(m.nudgeNote(0, 2)); // 57.25 rounds to 57, +2 -> 59
    CHECK(m.notes()[0].correctedMidi() == Approx(59.0));
    m.nudgeNote(0, 40);
    CHECK(m.notes()[0].shift == Approx(PitchNoteGridModel::kMaxShift));
    REQUIRE(m.resetNote(0));
    CHECK(m.notes()[0].shift == 0.0);
    CHECK_FALSE(m.nudgeNote(9, 1));

    m.snapNoteTo(1, 67);
    const auto c = m.corrections();
    REQUIRE(c.size() == 1);
    CHECK(c[0].startSec == Approx(1.5));
    CHECK(c[0].shiftSemitones == Approx(2.4));
    const auto v = m.correctionsAsVar();
    REQUIRE(v.size() == 1);
    CHECK(static_cast<double>(v[0]["shift_semitones"]) == Approx(2.4));
    CHECK(m.displayRange().getStart() <= 55);
    CHECK(m.displayRange().getEnd() >= 69);
    CHECK(PitchNoteGridModel::midiName(60) == "C4");
    CHECK(PitchNoteGridModel::midiName(69) == "A4");
}

TEST_CASE("Word clip gain drawing assigns stroke levels to crossed words", "[vocal-edit][wordgain]")
{
    WordClipGainModel m;
    m.setWords({{"world", 1.0, 1.4, 0}, {"hello", 0.0, 0.5, 0}, {"bad", 2.0, 2.0, 0}, {"there", 0.5, 0.9, 0}});
    REQUIRE(m.words().size() == 3);
    CHECK(m.words()[0].text == "hello");

    // A flat stroke at -6 dB across the first two words.
    CHECK(m.drawStroke(0.0, -6.0, 0.95, -6.0) == 2);
    CHECK(m.words()[0].gainDb == Approx(-6.0));
    CHECK(m.words()[1].gainDb == Approx(-6.0));
    CHECK(m.words()[2].gainDb == 0.0);

    // Adjacent equal-gain words merge into one region.
    auto r = m.regions();
    REQUIRE(r.size() == 1);
    CHECK(r[0].startSec == Approx(0.0));
    CHECK(r[0].endSec == Approx(0.9));

    // Point stroke on a single word, clamped and snapped to 0.5 dB.
    m.drawStroke(1.2, 30.0, 1.2, 30.0);
    CHECK(m.words()[2].gainDb == Approx(WordClipGainModel::kMaxGainDb));
    m.nudge(2, -0.3);
    CHECK(m.words()[2].gainDb == Approx(11.5));
    CHECK(m.regionsAsVar().size() == 2);
    m.resetAll();
    CHECK(m.regions().empty());
}

TEST_CASE("Take alignment request and response round-trip", "[vocal-edit][align]")
{
    TakeAlignmentRequest req;
    CHECK_FALSE(req.isValid());
    req.referencePath = "C:/lead.wav";
    req.takePaths = {"C:/dbl1.wav", "C:/dbl2.wav"};
    req.quantize = true;
    req.gridDivision = 8;
    req.strength = 1.7;
    REQUIRE(req.isValid());
    const auto v = req.toVar();
    CHECK(v["mode"].toString() == "quantize");
    CHECK(v["take_paths"].size() == 2);
    CHECK(static_cast<double>(v["strength"]) == Approx(1.0));
    CHECK(v["bpm"].isVoid());
    req.gridDivision = 12;
    CHECK_FALSE(req.isValid());

    const auto res = TakeAlignmentResult::fromVar(parse(R"({"mode": "align", "bpm": null, "takes": [
        {"path": "C:/dbl1.wav", "mean_abs_offset_ms_before": 31.0, "mean_abs_offset_ms_after": 1.5,
         "rendered_path": "C:/dbl1_aligned.wav",
         "segments": [{"start_sec": 0.0, "end_sec": 1.0, "shift_ms": -30.0},
                      {"start_sec": 1.0, "end_sec": 2.5, "shift_ms": 12.0}]},
        {"path": "C:/dbl2.wav", "mean_abs_offset_ms_before": 5.0, "mean_abs_offset_ms_after": 5.0,
         "rendered_path": null, "segments": []}]})"));
    REQUIRE(res.takes.size() == 2);
    CHECK(res.bpm == 0.0);
    CHECK(res.takes[0].improvementMs() == Approx(29.5));
    CHECK(res.takes[1].renderedPath.isEmpty());
    CHECK(res.maxAbsShiftMs() == Approx(30.0));
    CHECK(res.durationSec() == Approx(2.5));
    CHECK(res.summary().contains("dbl1: 31.0 -> 1.5 ms"));
}
