// shared-core/include/khensu/analysis/MusicTheory.h
// Small pure music-theory helpers for the C++ side: key parsing, key-aware
// pitch spelling, chord naming from a set of sounding pitches, bar/measure
// segmentation from a beat grid, and octave-folded tempo consensus.
//
// The Python service owns the full theory stack (_note_names.py /
// _harmony.py); these exist for the few places the desktop app has to derive
// something locally (the ChordsPanel fallback and chord-function labels, the
// pitch editor's key-aware note names and snap-to-key, batch aggregation).
// Spelling is a port of the service's scale-degree speller (spellDegree), so
// those paths spell and count the same way the service does.
// THREAD_CONTEXT(AnyThread) - pure functions, no shared state.
#pragma once
#include <optional>
#include <string>
#include <vector>

namespace khensu::theory {

/// A parsed key: tonic pitch class (0 = C) and mode.
struct ParsedKey {
    int tonic_pc = 0;
    bool minor = false;
    /// True when the key signature is written with flats (F, Bb, ... majors;
    /// D, G, C, F, ... minors; any tonic spelled with a 'b').
    bool prefer_flats = false;
};

/// Parses "A minor", "Bb major", "F#m", "C# min", "Eb" (major implied).
/// Returns nullopt for "", "unknown", "N" or anything unparseable.
std::optional<ParsedKey> parseKey(const std::string& keyName);

/// Same, from a separate root + mode pair (the /analyze/chords shape).
std::optional<ParsedKey> parseKey(const std::string& root, const std::string& mode);

/// Spells a pitch class (any int, wrapped mod 12) with sharps or flats.
std::string spellPitchClass(int pc, bool preferFlats);

/// The key's tonic as written ("F#" for F# major, "Gb" for Gb major).
std::string tonicName(const ParsedKey& key);

/// Spells pitch class `pc` from its scale degree in `key` - a port of the
/// service's _note_names.spell_degree (THY-08): the letter is the tonic
/// letter advanced by the degree the Roman numeral implies (b2/b3/b6/b7
/// borrowed flats in major; #3/#6/#7 raised degrees in minor; the tritone is
/// #4), so bVII in C is "Bb" (not "A#") and the leading tone of D minor is
/// "C#". In flat keys a borrowed flat that would need a double flat or a
/// Cb/Fb falls back to the natural name (A, not "Bbb", in Db major - THY-07);
/// theory-required double sharps stay (F## in G# minor).
std::string spellDegree(int pc, const ParsedKey& key);

/// Key-aware scientific pitch name (C4 = MIDI 60): spelled by scale degree
/// for `key`, sharps without one. The octave follows the written letter
/// (Cb4 == MIDI 59).
std::string midiNoteName(int midi, const std::optional<ParsedKey>& key);

/// Pitch classes of the key's scale. Major keys: the major scale. Minor
/// keys: the natural minor plus the raised leading tone (harmonic minor's
/// 7th), so a sung G# over E7 in A minor is in key.
std::vector<int> scalePitchClasses(const ParsedKey& key);

/// Nearest MIDI note to `midi` (fractional) whose pitch class is in
/// scalePitchClasses(key); ties go to the lower note.
int nearestScaleMidi(double midi, const ParsedKey& key);

/// The next scale note strictly above (`direction` > 0) or below (< 0) the
/// semitone nearest `midi`.
int stepScaleMidi(double midi, const ParsedKey& key, int direction);

/// Harmonic category of a chord symbol ("D7", "Bbm", "F#dim") in `key` - a
/// port of the service's _harmony.chord_function (THY-05).
enum class ChordFunction { Unknown, Diatonic, Secondary, Borrowed, Chromatic };
ChordFunction chordFunction(const std::string& chordSymbol, const ParsedKey& key);
/// "diatonic" / "secondary" / "borrowed" / "chromatic"; "" for Unknown.
const char* chordFunctionName(ChordFunction f) noexcept;

/// Names the chord formed by `midiPitches` (any order, duplicates allowed).
/// Seventh chords are matched before triads, so A-C-E-G reads "Am7" rather
/// than the C triad it contains; ties prefer the chord rooted on the bass
/// (lowest) note. A bass note that is not the root is written as a slash
/// ("Am7/C") unless `withBass` is false (octave-less, pitch-class-only notes
/// have no real bass). Roots and bass are spelled from their scale degree in
/// `key` (spellDegree: a Bb triad in C is "Bb", not "A#"); sharps when the
/// key is unknown. Returns "" for no pitches, or the bass note's name when no
/// chord template matches.
std::string nameChord(const std::vector<int>& midiPitches, const std::optional<ParsedKey>& key,
                      bool withBass = true);

/// Beats per bar for a "4/4" / "3/4" / "6/8" style signature; 0 if unknown.
int beatsPerBar(const std::string& timeSignature);

/// One bar (or, without a bar grid, one window) of the timeline.
struct Measure {
    double start_sec = 0.0;
    double end_sec = 0.0;
    /// 1-based bar number counted from the first downbeat; 0 for a pickup
    /// before the first downbeat. -1 when the timeline has no real bar
    /// grid (windows from beats without downbeats, or fixed time windows) -
    /// callers should label those by time, not as measure numbers.
    int number = -1;
};

/// Splits [0, durationSec] into bars. Preference order:
///  1. `downbeats` (the service's bar grid): bar n starts at downbeat n-1,
///     a pickup before the first downbeat is bar 0, and every bar is
///     numbered whether or not anything sounds in it (no drift).
///  2. `beats` grouped `beatsPerBarHint` (0 -> 4) at a time - unnumbered,
///     since the bar phase is unknown without downbeats.
///  3. fixed `fallbackWindowSec` windows - unnumbered.
std::vector<Measure> segmentMeasures(const std::vector<double>& downbeats, const std::vector<double>& beats,
                                     int beatsPerBarHint, double durationSec, double fallbackWindowSec = 2.0);

/// Octave-folded tempo consensus for several detections of related material.
/// Every value is folded into one octave, the median of the folded values is
/// taken, and the result is placed in the octave most of the raw detections
/// sit in (ties go to the first value's octave - the representative file).
/// 70 and 140 therefore agree on 70 instead of averaging to a meaningless 105.
/// Non-positive values are ignored; nullopt when none remain.
struct TempoConsensus {
    float bpm = 0.0f;
    /// Fraction [0, 1] of the detections that agree with `bpm` within 4%
    /// after octave folding.
    float agreement = 0.0f;
    /// Indices (into the input) of the agreeing detections.
    std::vector<size_t> agreeing;
};
std::optional<TempoConsensus> foldedTempoConsensus(const std::vector<float>& bpms);

} // namespace khensu::theory
