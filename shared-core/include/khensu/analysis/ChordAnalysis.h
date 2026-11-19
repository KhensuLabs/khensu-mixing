// shared-core/include/khensu/analysis/ChordAnalysis.h
// Chord progression analysis result from the /analyze/chords Python endpoint.
// THREAD_CONTEXT(AnyThread) - plain data struct.
#pragma once
#include <string>
#include <vector>

namespace khensu {

/// One detected chord event with timestamp and confidence.
struct ChordEvent {
    float time_sec = 0.0f;   ///< Onset time in seconds
    std::string chord;       ///< Root + quality, e.g. "C", "Am", "F#m"
    float confidence = 0.0f; ///< Cosine similarity vs. chord template [0, 1]
    std::string roman;       ///< Roman numeral vs. the key ("vi", "V7", "bVII"); "" for N.C. / unknown key
    bool diatonic = false;   ///< All chord tones in the key (only meaningful when has_diatonic)
    bool has_diatonic = false;
    /// Harmonic function vs. the key from the service ("diatonic",
    /// "secondary", "borrowed", "chromatic"); "" when absent (older service,
    /// N.C. or unknown key). Preferred over the diatonic flag for labelling.
    std::string function;
};

/// Full chord analysis result for one audio file.
struct ChordAnalysis {
    std::vector<ChordEvent> chords; ///< Detected chord events in temporal order
    std::string key;                ///< Overall key root, e.g. "A"
    std::string mode;               ///< "major" or "minor"
    float key_confidence = 0.0f;    ///< Confidence of key detection [0, 1]
    std::string camelot;            ///< Camelot wheel code, e.g. "8B"; "" when unknown
    std::string relative_key;       ///< e.g. "A minor" for C major; "" when unknown
    std::string parallel_key;       ///< e.g. "C minor" for C major; "" when unknown
    int circle_of_fifths_index = -1; ///< 0..11 clockwise from C major / A minor; -1 unknown
};

} // namespace khensu
