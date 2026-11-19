// desktop-app/Source/ui/shared/FindingCalibrationCache.h
// CE-TRU-1: which finding codes are calibrated against a labelled corpus.
// The service's GET /calibration/findings is fetched once, lazily, in the
// background; until it arrives (or when no table was ever built - the
// honest default) every finding reads "heuristic". Thread-safe.
#pragma once
#include <JuceHeader.h>
#include <functional>
#include <optional>

namespace khensu {

class FindingCalibrationCache {
  public:
    struct Entry {
        int n = 0;
        double precision = 0.0;
        double recall = 0.0;
    };

    /// The calibration row for an issue code, if the table covers it.
    static std::optional<Entry> lookup(const juce::String& issueCode);

    /// "calibrated (n=42)" or "heuristic" - the card's basis tag (ASCII).
    static juce::String basisTag(const juce::String& issueCode);

    /// Tooltip text explaining the tag for this code.
    static juce::String basisTooltip(const juce::String& issueCode);
    /// The basis sentence alone (calibrated / heuristic), without NEW-2's evidence.
    static juce::String basisOnly(const juce::String& issueCode);

    /// Replace the table from a CalibrationTableResponse (tests, the fetch).
    static void setFromVar(const juce::var& response);

    /// Fetch the table once (retried at most once a minute after a failure),
    /// running the HTTP call through `runInBackground`.
    static void ensureFetched(const std::function<void(std::function<void()>)>& runInBackground);

    static void resetForTests();
};

} // namespace khensu
