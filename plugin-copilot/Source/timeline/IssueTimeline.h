#pragma once

#include <JuceHeader.h>
#include <khensu/recommendations/Recommendation.h>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace khensu::timeline {

/// Where a timed finding came from. Live = the rolling analysis of the audio
/// the plugin processed (CE-DAW-3); Ara = an ARA audio source read through the
/// host (CE-DAW-1), mapped onto its playback region.
enum class IssueSource { Live, Ara };

/// One finding pinned to the host timeline (project seconds).
/// THREAD_CONTEXT(AnyThread) - plain value type.
struct TimedIssue {
    double startSec = 0.0;
    double endSec = 0.0;
    float severity = 0.0f;       ///< 0-100, as Recommendation::severity
    std::string key;             ///< stable issue code (issueCodeOf), merges repeats
    std::string title;           ///< Recommendation::what
    std::string action;          ///< Recommendation::suggested_action
    std::string regionName;      ///< ARA: the playback region / audio source name
    IssueSource source = IssueSource::Live;
    /// The fix's concrete targets (Recommendation::suggested_parameters), the
    /// input of "Write fix as automation" (CE-DAW-4).
    std::map<std::string, float> suggestedParameters;

    double durationSec() const noexcept { return endSec - startSec; }
};

/// Builds timed issues from recommendations measured on audio that starts at
/// `offsetSec` on the timeline and spans [spanStartSec, spanEndSec). A
/// recommendation with a time_range (relative to the measured audio) is
/// placed there, clipped to the span; one without a range covers the span.
/// Strengths and zero-severity cards are skipped.
std::vector<TimedIssue> issuesFromRecommendations(const std::vector<Recommendation>& recs, double offsetSec,
                                                  double spanStartSec, double spanEndSec, IssueSource source,
                                                  const std::string& fileId, const std::string& regionName = {});

/// ARA (CE-DAW-1): maps findings measured in an audio source's own time onto
/// one playback region that plays [startInSource, startInSource +
/// durationInSource) of it at startInPlayback on the host timeline (no time
/// stretch: the plug-in declares no playback transformations). Issues outside
/// the region are dropped, partial overlaps are clipped.
std::vector<TimedIssue> mapSourceIssuesToRegion(const std::vector<TimedIssue>& sourceIssues, double startInSource,
                                                double durationInSource, double startInPlayback,
                                                const std::string& regionName);

/// Thread-safe store of timed findings. Never touched by the audio thread:
/// the live worker and the ARA analysis jobs write, the editor reads.
class IssueTimeline {
  public:
    static constexpr size_t kMaxIssues = 2000;
    /// Adjacent windows reporting the same issue within this gap are shown as
    /// one span by mergedSnapshot().
    static constexpr double kMergeGapSec = 0.75;

    /// Replaces every `source` issue whose MIDPOINT lies inside
    /// [startSec, endSec) with `issues`: re-measuring a window supersedes what
    /// was measured there before (windows of a later pass need not line up
    /// with the earlier ones), so a fixed problem disappears on the next pass.
    void replaceWindow(IssueSource source, double startSec, double endSec, std::vector<TimedIssue> issues);
    /// Replaces every issue of `source` (an ARA document re-mapped its regions).
    void replaceAll(IssueSource source, std::vector<TimedIssue> issues);
    void clear(IssueSource source);
    void clearAll();

    /// Every stored issue, sorted by start time.
    std::vector<TimedIssue> snapshot() const;
    /// Same-key, same-source issues that touch or overlap (gap <= kMergeGapSec)
    /// merged into one span carrying the highest severity; sorted by start.
    std::vector<TimedIssue> mergedSnapshot() const;
    static std::vector<TimedIssue> merge(std::vector<TimedIssue> issues);

    /// Bumped on every change; the editor repaints when it moves.
    uint64_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }
    size_t size() const;

  private:
    void trimLocked();

    mutable std::mutex mutex_;
    std::vector<TimedIssue> issues_;
    std::atomic<uint64_t> generation_{0};
};

} // namespace khensu::timeline
