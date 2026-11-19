// desktop-app/Source/ui/Dashboard/ChordChart.h
// Pure helpers behind the Chords panel's "Copy chart" and "Export chords .mid"
// actions: chord-symbol voicing, bar alignment against the detected downbeats
// + time signature, a plain-text lead-sheet chart, and a Standard MIDI File
// with tempo + time-signature meta events.
// THREAD_CONTEXT(AnyThread) - pure functions, no UI.
#pragma once
#include <JuceHeader.h>
#include <khensu/analysis/ChordAnalysis.h>
#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace khensu::chordchart {

struct TimeSignature {
    int numerator = 4;
    int denominator = 4;
};

/// "6/8" -> {6, 8}. Anything unparseable falls back to 4/4.
inline TimeSignature parseTimeSignature(const juce::String& text)
{
    TimeSignature ts;
    const auto num = text.upToFirstOccurrenceOf("/", false, false).trim().getIntValue();
    const auto den = text.fromFirstOccurrenceOf("/", false, false).trim().getIntValue();
    if (num > 0 && num <= 32 && (den == 2 || den == 4 || den == 8 || den == 16)) {
        ts.numerator = num;
        ts.denominator = den;
    }
    return ts;
}

/// Pitch classes of a chord symbol ("Bbm7", "F#dim", "Csus4"), root first.
/// nullopt for "N.C." / unknown symbols.
inline std::optional<std::vector<int>> chordPitchClasses(const juce::String& symbol)
{
    const auto text = symbol.trim();
    if (text.isEmpty() || text == "N" || text == "N.C.") return std::nullopt;
    static const int kLetter[] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
    const auto first = juce::CharacterFunctions::toUpperCase(text[0]);
    if (first < 'A' || first > 'G') return std::nullopt;
    int root = kLetter[first - 'A'];
    int i = 1;
    while (i < text.length() && (text[i] == '#' || text[i] == 'b')) {
        root += text[i] == '#' ? 1 : -1;
        ++i;
    }
    root = ((root % 12) + 12) % 12;
    const auto quality = text.substring(i);

    std::vector<int> intervals;
    if (quality.isEmpty()) intervals = {0, 4, 7};
    else if (quality == "m") intervals = {0, 3, 7};
    else if (quality == "7") intervals = {0, 4, 7, 10};
    else if (quality == "maj7") intervals = {0, 4, 7, 11};
    else if (quality == "m7") intervals = {0, 3, 7, 10};
    else if (quality == "dim") intervals = {0, 3, 6};
    else if (quality == "dim7") intervals = {0, 3, 6, 9};
    else if (quality == "m7b5") intervals = {0, 3, 6, 10};
    else if (quality == "aug") intervals = {0, 4, 8};
    else if (quality == "sus2") intervals = {0, 2, 7};
    else if (quality == "sus4") intervals = {0, 5, 7};
    else return std::nullopt;

    std::vector<int> pcs;
    for (int iv : intervals) pcs.push_back((root + iv) % 12);
    return pcs;
}

/// Block-chord voicing in a mid register: root in C3..B3 (MIDI 48-59), the
/// other chord tones stacked above it in close position.
inline std::vector<int> voiceChord(const std::vector<int>& pitchClasses)
{
    std::vector<int> notes;
    if (pitchClasses.empty()) return notes;
    const int rootNote = 48 + pitchClasses.front();
    notes.push_back(rootNote);
    for (size_t i = 1; i < pitchClasses.size(); ++i) {
        int n = 48 + pitchClasses[i];
        while (n <= notes.back()) n += 12;
        notes.push_back(n);
    }
    return notes;
}

/// Bar start times. Prefers the detected downbeats; otherwise derives bars
/// from the tempo (one tracked beat per numerator count - the service tracks
/// 6/8 at an eighth pulse), falling back to 2 s bars when even BPM is unknown.
inline std::vector<double> barStarts(const std::vector<double>& downbeats, TimeSignature ts, double bpm,
                                     double durationSec)
{
    std::vector<double> bars;
    if (downbeats.size() >= 2) {
        bars = downbeats;
        std::sort(bars.begin(), bars.end());
        // Anything before the first downbeat is a pickup - give it its own bar.
        if (bars.front() > 0.25) bars.insert(bars.begin(), 0.0);
        const double lastLen = bars[bars.size() - 1] - bars[bars.size() - 2];
        while (lastLen > 0.05 && bars.back() < durationSec) bars.push_back(bars.back() + lastLen);
        return bars;
    }
    const double barLen = bpm > 1.0 ? ts.numerator * 60.0 / bpm : 2.0;
    for (double t = 0.0; t < juce::jmax(durationSec, barLen); t += barLen) bars.push_back(t);
    return bars;
}

/// Index of the bar containing time `t` (0 if before the first bar).
inline size_t barIndexFor(const std::vector<double>& bars, double t)
{
    if (bars.empty()) return 0;
    auto it = std::upper_bound(bars.begin(), bars.end(), t + 1.0e-3);
    return it == bars.begin() ? 0 : static_cast<size_t>(std::distance(bars.begin(), it) - 1);
}

/// Consecutive repeats collapsed - the chart and MIDI only care about changes.
inline std::vector<ChordEvent> chordChanges(const std::vector<ChordEvent>& chords)
{
    std::vector<ChordEvent> out;
    for (const auto& c : chords)
        if (out.empty() || out.back().chord != c.chord) out.push_back(c);
    return out;
}

struct ChartContext {
    juce::String title;
    juce::String key;   ///< "C major"
    juce::String camelot;
    TimeSignature timeSignature;
    double bpm = 0.0;
    std::vector<double> downbeats;
    double durationSec = 0.0;
};

/// Plain-text, bar-aligned chord chart:
///   | C (I) | F (IV) G (V) | % | ...   (4 bars per line, "%" = repeat bar)
inline juce::String buildChart(const ChordAnalysis& ca, const ChartContext& ctx)
{
    const auto changes = chordChanges(ca.chords);
    double duration = ctx.durationSec;
    if (!changes.empty()) duration = juce::jmax(duration, static_cast<double>(changes.back().time_sec) + 0.5);
    const auto bars = barStarts(ctx.downbeats, ctx.timeSignature, ctx.bpm, duration);

    std::vector<juce::StringArray> cells(bars.empty() ? 1 : bars.size());
    for (const auto& c : changes) {
        const auto idx = juce::jmin(barIndexFor(bars, c.time_sec), cells.size() - 1);
        juce::String cell(c.chord);
        if (!c.roman.empty()) cell << " (" << juce::String(c.roman) << ")";
        cells[idx].add(cell);
    }
    // Trim trailing empty bars.
    while (cells.size() > 1 && cells.back().isEmpty()) cells.pop_back();

    juce::String out;
    if (ctx.title.isNotEmpty()) out << ctx.title << "\n";
    juce::StringArray header;
    if (ctx.key.isNotEmpty()) header.add("Key: " + ctx.key);
    if (ctx.camelot.isNotEmpty()) header.add("Camelot: " + ctx.camelot);
    header.add("Time: " + juce::String(ctx.timeSignature.numerator) + "/" + juce::String(ctx.timeSignature.denominator));
    if (ctx.bpm > 0.0) header.add("Tempo: " + juce::String(ctx.bpm, 1) + " BPM");
    out << header.joinIntoString("   ") << "\n\n";

    constexpr int kBarsPerLine = 4;
    for (size_t i = 0; i < cells.size(); ++i) {
        if (i % kBarsPerLine == 0) {
            if (i > 0) out << "|\n";
            out << juce::String(static_cast<int>(i + 1)).paddedLeft(' ', 3) << " ";
        }
        out << "| " << (cells[i].isEmpty() ? juce::String("%") : cells[i].joinIntoString("  ")) << " ";
    }
    out << "|\n";
    return out;
}

/// Standard MIDI File (type 1): track 0 = tempo + time signature, track 1 =
/// block chords, each held until the next change (the last for one bar).
inline juce::MidiFile buildMidi(const ChordAnalysis& ca, const ChartContext& ctx)
{
    constexpr int kTicksPerQuarter = 480;
    // Tempo meta events are in quarter notes; the tracked pulse is the
    // signature's denominator (a 6/8 pulse is an eighth).
    const double pulseBpm = ctx.bpm > 1.0 ? ctx.bpm : 120.0;
    const double quarterBpm = pulseBpm * 4.0 / ctx.timeSignature.denominator;
    const double ticksPerSecond = quarterBpm / 60.0 * kTicksPerQuarter;
    auto toTicks = [&](double sec) { return std::round(juce::jmax(0.0, sec) * ticksPerSecond); };

    juce::MidiMessageSequence meta;
    meta.addEvent(juce::MidiMessage::tempoMetaEvent(static_cast<int>(std::round(60000000.0 / quarterBpm))), 0.0);
    meta.addEvent(juce::MidiMessage::timeSignatureMetaEvent(ctx.timeSignature.numerator,
                                                            ctx.timeSignature.denominator),
                  0.0);
    if (ctx.title.isNotEmpty()) meta.addEvent(juce::MidiMessage::textMetaEvent(3, ctx.title), 0.0);
    meta.addEvent(juce::MidiMessage::endOfTrack(), 0.0);

    const auto changes = chordChanges(ca.chords);
    const double barSec = ctx.timeSignature.numerator * 60.0 / pulseBpm;
    juce::MidiMessageSequence chords;
    chords.addEvent(juce::MidiMessage::textMetaEvent(3, "Chords"), 0.0);
    double lastEnd = 0.0;
    for (size_t i = 0; i < changes.size(); ++i) {
        const auto pcs = chordPitchClasses(juce::String(changes[i].chord));
        if (!pcs) continue; // N.C. - silence until the next chord
        const double start = changes[i].time_sec;
        const double end = i + 1 < changes.size() ? static_cast<double>(changes[i + 1].time_sec) : start + barSec;
        if (end <= start) continue;
        for (int note : voiceChord(*pcs)) {
            chords.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(90)), toTicks(start));
            chords.addEvent(juce::MidiMessage::noteOff(1, note), toTicks(end) - 1.0);
        }
        lastEnd = juce::jmax(lastEnd, end);
    }
    chords.updateMatchedPairs();
    chords.addEvent(juce::MidiMessage::endOfTrack(), toTicks(lastEnd));

    juce::MidiFile file;
    file.setTicksPerQuarterNote(kTicksPerQuarter);
    file.addTrack(meta);
    file.addTrack(chords);
    return file;
}

} // namespace khensu::chordchart
