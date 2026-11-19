#pragma once
#include <optional>
#include <string>
#include <vector>

namespace khensu {

/// One detected section from section analysis.
/// Contains timing and label for a contiguous segment of the audio.
struct SectionData {
    std::string label; // "Verse 1", "Chorus 2", "Intro" (NEW-1); "A"/"B"... on the fallback grid
    std::string role;  // intro | verse | pre-chorus | chorus | bridge | outro | section; empty if unknown
    std::string group; // repetition group "A", "B", ...; empty if unknown
    float start_sec = 0.0f;
    float end_sec = 0.0f;
    float duration_sec = 0.0f;
    float lufs_integrated = 0.0f;      // 0.0f = not computed (too short or unavailable)
    float crest_db = 0.0f;             // Crest factor (dB); 0 = not computed
    float peak_dbfs = 0.0f;            // Sample peak (dBFS); 0 = not computed
    float noise_floor_db = -90.0f;     // 10th-pct RMS of 50 ms windows (dBFS)
    float harshness_score = -1.0f;     // 0-1: 2.5-5 kHz vs. 1-2.5 kHz ratio; -1 = not computed
    float sibilance_score = -1.0f;     // 0-1: 5-10 kHz vs. 1-10 kHz ratio; -1 = not computed
    float boundary_confidence = -1.0f; // 0-1: onset flux at detected boundary; -1 = not computed
    float stereo_correlation = 0.0f;   // -1..1; 0 = not computed
};

/// Loudness arc across sections - proxy for chorus/verse energy management.
/// Populated alongside SectionAnalysis from the /analyze/sections endpoint.
struct LoudnessArc {
    float chorus_verse_delta_lu = 0.0f; ///< loudest - quietest section LUFS (LU); 0 = not computed
    float max_section_lufs = 0.0f;      ///< loudest section
    float min_section_lufs = 0.0f;      ///< quietest section
    int section_count = 0;
};

/// Collection of all detected sections from /analyze/sections endpoint.
/// Populated by the Python analysis pipeline in SP-16D.
struct SectionAnalysis {
    std::vector<SectionData> sections;
    int section_count = 0;
    float total_duration_sec = 0.0f;
    LoudnessArc loudness_arc; ///< populated when >= 2 sections have valid LUFS
};

/// One dimension's comparison between target and reference track.
struct ReferenceDelta {
    std::string dimension; // e.g. "lufs", "crest_db"
    float target = 0.0f;
    float reference = 0.0f;
    float delta = 0.0f; // target - reference
};

/// One octave band of the suggested match-EQ curve.
struct MatchEqBand {
    float center_hz = 0.0f;
    float gain_db = 0.0f; // + = boost, - = cut (pulls target toward reference)
    float q = 1.0f;       // octave-ish bandwidth; user-editable on the match curve
};

/// Per-dimension deltas between target mix and reference track.
/// Populated by the /analyze/reference Python endpoint in SP-16D.
/// Positive delta = target is above reference; negative = target is below.
struct ReferenceDeltas {
    std::vector<ReferenceDelta> deltas;
    /// Corrective EQ to move the target toward the reference tonal balance.
    std::vector<MatchEqBand> match_eq;
};

} // namespace khensu
