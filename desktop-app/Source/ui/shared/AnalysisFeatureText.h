// desktop-app/Source/ui/shared/AnalysisFeatureText.h
// Pure presentation helpers for the analyses surfaced from formerly orphan
// service routes (audit 2026-11-06 §9.2): groove timing, harmony timeline,
// reverb tail, stem phase, vocal-harmony MIDI and fingerprint compare. Kept
// header-only and free of components so the wording, units and thresholds
// are unit-tested (tests/ui/OrphanRouteFeaturesTest.cpp). ASCII only.
// THREAD_CONTEXT(AnyThread)
#pragma once
#include <JuceHeader.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <algorithm>
#include <cmath>

namespace khensu::feature_text {

/// The panel colour for a reading, resolved against the theme by the panel
/// (red = severe, amber = moderate, blue = minor/info, green = strength).
enum class Tone { Good, Info, Moderate, Severe, Neutral };

// ---- Groove timing ---------------------------------------------------------

/// Swing as the long note's share of an off-beat pair, in percent: 50 % =
/// straight 8ths, 66.7 % = triplet swing (the MPC/DAW "swing %" convention).
/// `ratio` is the service's long:short swing_ratio.
inline double swingPercent(double ratio)
{
    if (!(ratio > 0.0) || !std::isfinite(ratio)) return 50.0;
    return 100.0 * ratio / (1.0 + ratio);
}

/// Named feel for a swing ratio.
inline juce::String swingFeel(double ratio)
{
    const double pct = swingPercent(ratio);
    if (pct < 47.0) return TRANS("reversed (short-long)");
    if (pct < 53.0) return TRANS("straight");
    if (pct < 58.0) return TRANS("light swing");
    if (pct < 63.0) return TRANS("medium swing");
    return TRANS("hard / triplet swing");
}

/// "Pocket" wording: where the median onset sits against the grid.
inline juce::String pocketText(const juce::String& pocket, double offsetMs)
{
    const juce::String ms = juce::String(std::abs(offsetMs), 1) + " ms";
    if (pocket == "ahead_of_grid") return TRANS("Ahead of the grid (pushing) by") + " " + ms;
    if (pocket == "behind_grid") return TRANS("Behind the grid (laid back) by") + " " + ms;
    if (pocket == "on_grid") return TRANS("On the grid") + " (" + (offsetMs >= 0.0 ? "+" : "-") + ms + ")";
    if (pocket == "insufficient_onsets") return TRANS("Too few clear onsets to measure the pocket.");
    return pocket.isEmpty() ? juce::String() : pocket;
}

/// Tightness of the timing spread (std dev of onset offsets, ms).
inline juce::String spreadText(double stdMs)
{
    const juce::String ms = juce::String(stdMs, 1) + " ms";
    if (stdMs < 8.0) return TRANS("Tight") + " (" + ms + " " + TRANS("spread") + ")";
    if (stdMs < 18.0) return TRANS("Human / relaxed") + " (" + ms + " " + TRANS("spread") + ")";
    return TRANS("Loose") + " (" + ms + " " + TRANS("spread") + ")";
}

inline Tone spreadTone(double stdMs)
{
    if (stdMs < 8.0) return Tone::Good;
    if (stdMs < 18.0) return Tone::Info;
    return Tone::Moderate;
}

/// "16th grid" / "8th grid" / "triplet grid"; "" for none.
inline juce::String gridText(const juce::String& subdivision)
{
    if (subdivision.isEmpty() || subdivision == "none") return {};
    return subdivision + " " + TRANS("grid");
}

/// One-paragraph summary of a groove result.
inline juce::String grooveSummary(const ipc::HttpServiceBridge::GrooveTimingResult& g)
{
    if (g.pocket == "insufficient_onsets") return pocketText(g.pocket, 0.0);
    juce::String s;
    if (g.bpm > 0.0) s << juce::String(g.bpm, 1) << " BPM";
    if (const auto grid = gridText(g.gridSubdivision); grid.isNotEmpty()) s << (s.isEmpty() ? "" : ", ") << grid;
    s << "\n" << TRANS("Swing") << ": " << juce::String(juce::roundToInt(swingPercent(g.swingRatio))) << "% ("
      << swingFeel(g.swingRatio) << ")";
    s << "\n" << TRANS("Pocket") << ": " << pocketText(g.pocket, g.gridOffsetMs);
    s << "\n" << TRANS("Timing") << ": " << spreadText(g.timingStdMs);
    s << "\n" << juce::String(juce::roundToInt(g.aheadFraction * 100.0)) << "% " << TRANS("early") << ", "
      << juce::String(juce::roundToInt(g.behindFraction * 100.0)) << "% " << TRANS("late") << " (>12 ms)";
    return s;
}

// ---- Harmony timeline ------------------------------------------------------

/// "F# minor" from a key point.
inline juce::String keyName(const ipc::HttpServiceBridge::HarmonyTimelineResult::KeyPoint& k)
{
    return k.mode.isEmpty() ? k.key : k.key + " " + k.mode;
}

/// The span the timeline strip covers: the session duration when known,
/// else a little past the last event (never 0).
inline double timelineSpan(const ipc::HttpServiceBridge::HarmonyTimelineResult& h, double durationSec)
{
    if (durationSec > 0.0) return durationSec;
    double last = 0.0;
    for (const auto& c : h.progression)
        last = std::max(last, c.timeSec);
    for (const auto& k : h.keyChanges)
        last = std::max(last, k.timeSec);
    return last > 0.0 ? last + 2.0 : 1.0;
}

/// x position of `timeSec` on a strip of `width` px covering [0, span].
inline float timelineX(double timeSec, double span, float width)
{
    if (!(span > 0.0)) return 0.0f;
    return static_cast<float>(std::clamp(timeSec / span, 0.0, 1.0)) * width;
}

/// The key in force at `timeSec` (initial key, then the latest change).
inline juce::String keyAt(const ipc::HttpServiceBridge::HarmonyTimelineResult& h, double timeSec)
{
    juce::String key = h.initialKey.has_value() ? keyName(*h.initialKey) : juce::String();
    for (const auto& k : h.keyChanges)
        if (k.timeSec <= timeSec) key = keyName(k);
    return key;
}

/// Status line: "12 chord changes, 1 key change - starts in A minor".
inline juce::String harmonySummary(const ipc::HttpServiceBridge::HarmonyTimelineResult& h)
{
    if (h.progression.empty()) return TRANS("No chords detected (too little pitched content).");
    juce::String s;
    s << juce::String(static_cast<int>(h.progression.size())) << " " << TRANS("chord changes") << ", "
      << juce::String(static_cast<int>(h.keyChanges.size())) << " "
      << (h.keyChanges.size() == 1 ? TRANS("key change") : TRANS("key changes"));
    if (h.initialKey.has_value()) s << " - " << TRANS("starts in") << " " << keyName(*h.initialKey);
    return s;
}

// ---- Reverb tail -----------------------------------------------------------

inline juce::String rt60Text(const std::optional<double>& rt60Sec)
{
    if (!rt60Sec.has_value()) return TRANS("RT60: not measurable");
    return "RT60 " + juce::String(*rt60Sec, 2) + " s";
}

/// Rough room character for an RT60 (seconds).
inline juce::String spaceCharacter(const std::optional<double>& rt60Sec)
{
    if (!rt60Sec.has_value()) return {};
    const double rt = *rt60Sec;
    if (rt < 0.3) return TRANS("dry / small room");
    if (rt < 0.8) return TRANS("room-sized");
    if (rt < 1.6) return TRANS("hall / plate");
    return TRANS("long hall / ambient");
}

inline juce::String tailLabelText(const juce::String& label)
{
    if (label == "pronounced") return TRANS("Pronounced tail between hits");
    if (label == "moderate") return TRANS("Moderate tail between hits");
    if (label == "dry_or_short") return TRANS("Dry / short tail between hits");
    if (label == "insufficient_audio") return TRANS("Too little audio to measure.");
    return label;
}

inline Tone tailTone(const juce::String& label)
{
    if (label == "pronounced") return Tone::Moderate; // washy: check masking / clarity
    if (label == "moderate") return Tone::Good;
    if (label == "dry_or_short") return Tone::Info;
    return Tone::Neutral;
}

// ---- Stem phase ------------------------------------------------------------

/// "vocals / drums  +0.12  decorrelated (normal)".
inline juce::String stemPairLine(const ipc::HttpServiceBridge::StemPhaseResult::Pair& p)
{
    juce::String meaning;
    if (p.polarity == "inverted")
        meaning = TRANS("inverted - they cancel when summed; check polarity / alignment");
    else if (p.polarity == "aligned")
        meaning = TRANS("strongly correlated - shared content (bleed or doubled parts)");
    else
        meaning = TRANS("independent (normal)");
    return p.left + " / " + p.right + "  " + (p.correlation >= 0.0 ? "+" : "") + juce::String(p.correlation, 2) + "  " +
           meaning;
}

inline Tone stemPairTone(const juce::String& polarity)
{
    if (polarity == "inverted") return Tone::Severe;
    if (polarity == "aligned") return Tone::Moderate;
    return Tone::Good;
}

/// The worst pair first (inverted, then aligned, then the rest by |corr|).
inline void sortStemPairs(std::vector<ipc::HttpServiceBridge::StemPhaseResult::Pair>& pairs)
{
    auto rank = [](const juce::String& polarity) { return polarity == "inverted" ? 0 : polarity == "aligned" ? 1 : 2; };
    std::stable_sort(pairs.begin(), pairs.end(), [&](const auto& a, const auto& b) {
        if (rank(a.polarity) != rank(b.polarity)) return rank(a.polarity) < rank(b.polarity);
        return std::abs(a.correlation) > std::abs(b.correlation);
    });
}

// ---- Fingerprint compare ---------------------------------------------------

inline juce::String fingerprintVerdictText(const juce::String& verdict)
{
    if (verdict == "same_recording") return TRANS("Same recording");
    if (verdict == "same_song_different_version") return TRANS("Same song, different version / master");
    if (verdict == "different") return TRANS("Different recordings");
    return verdict;
}

inline juce::String fingerprintSummary(const ipc::HttpServiceBridge::FingerprintCompareResult& f,
                                       const juce::String& otherFileName)
{
    return fingerprintVerdictText(f.verdict) + " - " + juce::String(juce::roundToInt(f.similarity * 100.0)) + "% " +
           TRANS("similar to") + " " + otherFileName + " (" + TRANS("local chroma fingerprint") + ")";
}

// ---- Vocal harmony MIDI ----------------------------------------------------

/// "a 3rd above" etc. from the request/response interval + direction.
inline juce::String harmonyIntervalText(const juce::String& interval, const juce::String& direction)
{
    const juce::String iv = interval == "fifth" ? TRANS("a 5th") : TRANS("a 3rd");
    return iv + " " + (direction == "below" ? TRANS("below") : TRANS("above"));
}

/// Multi-line result: where it went, what key/tempo it used and why.
inline juce::String vocalHarmonySummary(const ipc::HttpServiceBridge::VocalHarmonyResult& v)
{
    juce::String s;
    s << TRANS("Wrote") << " " << juce::String(v.eventCount) << " " << TRANS("harmony notes") << " ("
      << harmonyIntervalText(v.interval, v.direction) << ")";
    s << "\n" << TRANS("Key") << ": " << v.key << (v.mode.isNotEmpty() ? " " + v.mode : juce::String())
      << (v.keySource == "detected" ? " (" + TRANS("detected") + ")" : juce::String());
    if (v.bpm > 0.0)
        s << "   " << juce::String(v.bpm, 1) << " BPM"
          << (v.bpmSource == "detected" ? " (" + TRANS("detected") + ")" : juce::String());
    if (v.lowQualityExtraction)
        s << "\n" << TRANS("Low-confidence note extraction - check the line before using it.");
    if (v.outputPath.isNotEmpty()) s << "\n" << TRANS("Saved to") << ": " << v.outputPath;
    return s;
}

/// A default .mid name next to the source: <stem>_harmony_<interval>_<dir>.mid
inline juce::File defaultHarmonyMidiFile(const juce::File& source, const juce::String& interval,
                                         const juce::String& direction)
{
    return source.getSiblingFile(source.getFileNameWithoutExtension() + "_harmony_" + interval + "_" + direction +
                                 ".mid");
}

} // namespace khensu::feature_text
