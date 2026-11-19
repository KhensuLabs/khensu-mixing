// tests/shared-core/test_music_theory_degree.cpp
// THY-05/07/08/14 (2026-11-06 audit): the C++ MusicTheory port spells by
// scale degree and classifies chord function exactly like the service's
// _note_names.spell_degree / _harmony.chord_function (the same cases are
// asserted in local-services/tests/test_music_theory_known_answers.py).
#include <catch2/catch_test_macros.hpp>
#include <khensu/analysis/MusicTheory.h>

using namespace khensu::theory;

namespace {
ParsedKey key(const char* name)
{
    auto k = parseKey(name);
    REQUIRE(k.has_value());
    return *k;
}
} // namespace

TEST_CASE("spellDegree follows the scale degree, not the key signature", "[music_theory][thy]")
{
    CHECK(spellDegree(10, key("C major")) == "Bb"); // bVII, was "A#"
    CHECK(spellDegree(8, key("C major")) == "Ab");  // bVI
    CHECK(spellDegree(6, key("C major")) == "F#");  // #iv
    CHECK(spellDegree(6, key("G minor")) == "F#");  // raised leading tone, not "Gb"
    CHECK(spellDegree(1, key("D minor")) == "C#");
    CHECK(spellDegree(7, key("G# minor")) == "F##"); // theory-required double sharp
    CHECK(spellDegree(10, key("F major")) == "Bb");
    CHECK(spellDegree(10, key("F# major")) == "A#"); // the key's own tonic spelling
    CHECK(spellDegree(10, key("Gb major")) == "Bb");
}

TEST_CASE("Flat keys fall back to a single accidental (THY-07)", "[music_theory][thy]")
{
    CHECK(spellDegree(2, key("Db major")) == "D");  // was "Ebb"
    CHECK(spellDegree(9, key("Db major")) == "A");  // was "Bbb"
    CHECK(spellDegree(4, key("Db major")) == "E");  // was "Fb"
    CHECK(spellDegree(7, key("Gb major")) == "G");  // was "Abb"
    CHECK(spellDegree(11, key("Gb major")) == "Cb"); // diatonic 4th keeps Cb
    CHECK(spellDegree(11, key("Bb major")) == "B");  // was "Cb"
}

TEST_CASE("nameChord spells a borrowed Bb triad in C as Bb, not A#", "[music_theory][thy]")
{
    CHECK(nameChord({46, 50, 53}, parseKey("C major")) == "Bb");
    CHECK(nameChord({44, 48, 51}, parseKey("C major")) == "Ab");
    CHECK(nameChord({57, 61, 64}, parseKey("Db major")) == "A"); // not "Bbb"
    // Without a real bass (pitch-class-only notes) there is no slash chord.
    CHECK(nameChord({60, 64, 69}, parseKey("C major")) == "Am/C");
    CHECK(nameChord({60, 64, 69}, parseKey("C major"), false) == "Am");
}

TEST_CASE("midiNoteName is key-aware with the octave following the letter", "[music_theory][thy]")
{
    CHECK(midiNoteName(60, std::nullopt) == "C4");
    CHECK(midiNoteName(70, std::nullopt) == "A#4");
    CHECK(midiNoteName(70, parseKey("F major")) == "Bb4");
    CHECK(midiNoteName(61, parseKey("D minor")) == "C#4");
    CHECK(midiNoteName(59, parseKey("Gb major")) == "Cb4"); // octave follows the letter
    CHECK(midiNoteName(-1, std::nullopt) == "B-2");
}

TEST_CASE("chordFunction matches the service's categories (THY-05/10)", "[music_theory][thy]")
{
    const auto c = key("C major");
    CHECK(chordFunction("C", c) == ChordFunction::Diatonic);
    CHECK(chordFunction("G7", c) == ChordFunction::Diatonic);
    CHECK(chordFunction("D7", c) == ChordFunction::Secondary);
    CHECK(chordFunction("E7", c) == ChordFunction::Secondary);
    CHECK(chordFunction("F#dim", c) == ChordFunction::Secondary);
    CHECK(chordFunction("D", c) == ChordFunction::Secondary);
    CHECK(chordFunction("Bb", c) == ChordFunction::Borrowed);
    CHECK(chordFunction("Ab", c) == ChordFunction::Borrowed);
    CHECK(chordFunction("Fm", c) == ChordFunction::Borrowed);
    CHECK(chordFunction("Bdim7", c) == ChordFunction::Borrowed);
    CHECK(chordFunction("F#", c) == ChordFunction::Chromatic);
    CHECK(chordFunction("Db", c) == ChordFunction::Chromatic);
    const auto am = key("A minor");
    CHECK(chordFunction("E7", am) == ChordFunction::Diatonic);
    CHECK(chordFunction("G#dim", am) == ChordFunction::Diatonic);
    CHECK(chordFunction("B7", am) == ChordFunction::Secondary);
    CHECK(chordFunction("D7", am) == ChordFunction::Secondary); // V7/VII
    CHECK(chordFunction("D", am) == ChordFunction::Borrowed);
    CHECK(chordFunction("A", am) == ChordFunction::Borrowed);
    CHECK(chordFunction("Bb", am) == ChordFunction::Chromatic);
    CHECK(chordFunction("N", c) == ChordFunction::Unknown);
    CHECK(chordFunction("Xq", c) == ChordFunction::Unknown);
    CHECK(std::string(chordFunctionName(ChordFunction::Secondary)) == "secondary");
}

TEST_CASE("Scale snapping uses the key, with the leading tone in minor", "[music_theory][thy]")
{
    const auto c = key("C major");
    CHECK(nearestScaleMidi(60.8, c) == 60); // a sharp C reads C, not C#
    CHECK(nearestScaleMidi(61.6, c) == 62);
    CHECK(nearestScaleMidi(66.0, c) == 65); // F# tie -> lower (F)
    const auto am = key("A minor");
    CHECK(nearestScaleMidi(68.1, am) == 68); // G# stays (raised leading tone)
    CHECK(stepScaleMidi(64.0, c, +1) == 65);   // E -> F
    CHECK(stepScaleMidi(65.0, c, -1) == 64);
    CHECK(stepScaleMidi(60.0, c, -1) == 59);
}
