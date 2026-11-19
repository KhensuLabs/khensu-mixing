// tests/ui/TheoryUiTest.cpp
// 2026-11-06 music-theory audit, desktop side: ChordsPanel chord-function
// labels (THY-05) and bass-less chroma-fallback chords (THY-08); the pitch
// editor model's manual-move preservation, snap-to-key, undo and key-aware
// note names (UIX-13 / THY-14).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "../../desktop-app/Source/ui/Dashboard/ChordsPanel.h"
#include "../../desktop-app/Source/ui/VocalWorkspace/PitchNoteGridModel.h"

using Catch::Approx;
using namespace khensu;

namespace {
ChordEvent chord(float t, const char* name, const char* roman, bool diatonic)
{
    ChordEvent e;
    e.time_sec = t;
    e.chord = name;
    e.roman = roman;
    e.diatonic = diatonic;
    e.has_diatonic = true;
    return e;
}

juce::var pitchResponse()
{
    return juce::JSON::parse(R"({"duration_sec": 4.0, "hop_sec": 0.5, "f0_midi": [],
        "notes": [
          {"id": 0, "start_sec": 0.0, "end_sec": 1.0, "midi": 61.3, "target_midi": 61},
          {"id": 1, "start_sec": 1.0, "end_sec": 2.0, "midi": 64.2, "target_midi": 64},
          {"id": 2, "start_sec": 2.0, "end_sec": 3.0, "midi": 66.1, "target_midi": 66}
        ]})");
}
} // namespace

TEST_CASE("Chord chart labels secondary, borrowed and chromatic chords (THY-05)", "[chords][music_theory][thy]")
{
    AnalysisResult r;
    ChordAnalysis ca;
    ca.key = "C";
    ca.mode = "major";
    ca.key_confidence = 0.9f;
    ca.chords = {chord(0.0f, "C", "I", true), chord(1.0f, "D7", "V7/V", false), chord(2.0f, "G", "V", true),
                 chord(3.0f, "Bb", "bVII", false), chord(4.0f, "F#", "#IV", false)};
    r.chord_analysis = ca;
    const auto text = ChordsPanel::buildText(r);
    INFO(text);
    CHECK(text.contains("V7/V  (secondary)"));   // was "(borrowed)"
    CHECK(text.contains("bVII  (borrowed)"));
    CHECK(text.contains("#IV  (chromatic)"));
    CHECK(text.contains("C       I\n")); // diatonic chords carry no label
}

TEST_CASE("Chroma-fallback notes give no slash chords (THY-08)", "[chords][music_theory][thy]")
{
    AnalysisResult r;
    r.python_data.pitch_key = "C major";
    r.python_data.downbeat_times_json = "[0.0, 2.0]";
    // A minor triad from octave-less chroma notes, all at 60 + pc: C (60) is
    // the lowest only because of that convention.
    r.python_data.pitch_notes_json = R"([
        {"start_sec": 0.1, "end_sec": 1.9, "midi_pitch": 60, "velocity": 0.5, "pitch_class_only": true},
        {"start_sec": 0.1, "end_sec": 1.9, "midi_pitch": 64, "velocity": 0.5, "pitch_class_only": true},
        {"start_sec": 0.1, "end_sec": 1.9, "midi_pitch": 69, "velocity": 0.5, "pitch_class_only": true},
        {"start_sec": 2.1, "end_sec": 3.9, "midi_pitch": 70, "velocity": 0.5, "pitch_class_only": true},
        {"start_sec": 2.1, "end_sec": 3.9, "midi_pitch": 62, "velocity": 0.5, "pitch_class_only": true},
        {"start_sec": 2.1, "end_sec": 3.9, "midi_pitch": 65, "velocity": 0.5, "pitch_class_only": true}])";
    const auto text = ChordsPanel::buildText(r);
    INFO(text);
    CHECK(text.contains("M1 (0.0s): Am\n"));   // was "Am/C"
    CHECK(text.contains("M2 (2.0s): Bb\n"));   // degree-spelled, not "A#"
}

TEST_CASE("Retune keeps manual moves; undo restores them (UIX-13)", "[vocal-edit][pitch][thy]")
{
    PitchNoteGridModel m;
    REQUIRE(m.loadFromResponse(pitchResponse()));
    m.pushUndo();
    REQUIRE(m.snapNoteTo(1, 66)); // manual move E -> F#
    CHECK(m.notes()[1].manual);
    m.retuneAll(1.0);
    CHECK(m.notes()[1].correctedMidi() == Approx(66.0)); // untouched by Retune
    CHECK(m.notes()[0].correctedMidi() == Approx(61.0));
    m.retuneAll(0.0);
    CHECK(m.notes()[1].correctedMidi() == Approx(66.0));
    CHECK(m.notes()[0].shift == Approx(0.0));

    REQUIRE(m.canUndo());
    REQUIRE(m.undo());
    CHECK(m.notes()[1].shift == Approx(0.0));
    CHECK_FALSE(m.notes()[1].manual);
    CHECK_FALSE(m.canUndo());

    // setShift restores a pre-drag state (drag returned to its start).
    REQUIRE(m.setShift(2, 0.0, false));
    CHECK_FALSE(m.notes()[2].manual);
    REQUIRE(m.resetNote(2));
}

TEST_CASE("Snap to key targets the detected key's scale (UIX-13)", "[vocal-edit][pitch][thy]")
{
    PitchNoteGridModel m;
    REQUIRE(m.loadFromResponse(pitchResponse()));
    m.setSnapToKey(true);
    CHECK_FALSE(m.snapToKey()); // no key yet
    m.setKey("C major");
    CHECK(m.snapToKey());
    // C#4 (61.3) -> nearest C-major pitch D4 (62); F#4 (66.1) -> G4 (67).
    m.retuneAll(1.0);
    CHECK(m.notes()[0].correctedMidi() == Approx(62.0));
    CHECK(m.notes()[1].correctedMidi() == Approx(64.0));
    CHECK(m.notes()[2].correctedMidi() == Approx(67.0));
    // Up from E moves a scale step (F), not a semitone.
    REQUIRE(m.nudgeNote(1, 1));
    CHECK(m.notes()[1].correctedMidi() == Approx(65.0));
    REQUIRE(m.nudgeNote(1, 1));
    CHECK(m.notes()[1].correctedMidi() == Approx(67.0));
    m.setSnapToKey(false);
    REQUIRE(m.snapNoteToTarget(0));
    CHECK(m.notes()[0].correctedMidi() == Approx(61.0)); // chromatic target again
}

TEST_CASE("Pitch editor note names follow the session key (THY-14)", "[vocal-edit][pitch][thy]")
{
    PitchNoteGridModel m;
    CHECK(m.noteName(70) == "A#4");
    m.setKey("F major");
    CHECK(m.noteName(70) == "Bb4");
    m.setKey("D minor");
    CHECK(m.noteName(61) == "C#4");
    m.setKey("unknown");
    CHECK(m.noteName(70) == "A#4");
    CHECK(PitchNoteGridModel::midiName(60) == "C4");

    REQUIRE(m.loadFromResponse(pitchResponse()));
    CHECK(m.notes()[0].detectedCents() == Approx(30.0));
    REQUIRE(m.snapNoteTo(0, 61));
    CHECK(m.notes()[0].correctedCents() == Approx(0.0).margin(1e-9));
}
