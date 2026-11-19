#include "IssueTimeline.h"

#include <algorithm>

namespace khensu::timeline {

std::vector<TimedIssue> issuesFromRecommendations(const std::vector<Recommendation>& recs, double offsetSec,
                                                  double spanStartSec, double spanEndSec, IssueSource source,
                                                  const std::string& fileId, const std::string& regionName)
{
    std::vector<TimedIssue> out;
    if (spanEndSec <= spanStartSec) return out;
    for (const auto& rec : recs) {
        if (rec.type == RecommendationType::Strength || rec.severity <= 0.0f) continue;
        if (!rec.abstained_reason.empty()) continue; // the recommender declined to make the claim

        double start = spanStartSec;
        double end = spanEndSec;
        if (rec.time_range && rec.time_range->is_valid() && rec.time_range->duration() > 0.0f) {
            start = offsetSec + static_cast<double>(rec.time_range->start_sec);
            end = offsetSec + static_cast<double>(rec.time_range->end_sec);
            start = std::max(start, spanStartSec);
            end = std::min(end, spanEndSec);
            if (end <= start) continue; // the flagged moment lies outside this span
        }

        TimedIssue issue;
        issue.startSec = start;
        issue.endSec = end;
        issue.severity = rec.severity;
        issue.key = issueCodeOf(rec, fileId);
        issue.title = rec.what;
        issue.action = rec.suggested_action;
        issue.regionName = regionName;
        issue.source = source;
        issue.suggestedParameters = rec.suggested_parameters;
        out.push_back(std::move(issue));
    }
    return out;
}

std::vector<TimedIssue> mapSourceIssuesToRegion(const std::vector<TimedIssue>& sourceIssues, double startInSource,
                                                double durationInSource, double startInPlayback,
                                                const std::string& regionName)
{
    std::vector<TimedIssue> out;
    if (!(durationInSource > 0.0)) return out;
    const double regionEndInSource = startInSource + durationInSource;
    const double offset = startInPlayback - startInSource;
    for (const auto& issue : sourceIssues) {
        const double s = std::max(issue.startSec, startInSource);
        const double e = std::min(issue.endSec, regionEndInSource);
        if (e <= s) continue;
        TimedIssue mapped = issue;
        mapped.startSec = s + offset;
        mapped.endSec = e + offset;
        if (!regionName.empty()) mapped.regionName = regionName;
        out.push_back(std::move(mapped));
    }
    return out;
}

void IssueTimeline::replaceWindow(IssueSource source, double startSec, double endSec, std::vector<TimedIssue> issues)
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        issues_.erase(std::remove_if(issues_.begin(), issues_.end(),
                                     [&](const TimedIssue& i) {
                                         const double mid = 0.5 * (i.startSec + i.endSec);
                                         return i.source == source && mid >= startSec && mid < endSec;
                                     }),
                      issues_.end());
        for (auto& issue : issues) issues_.push_back(std::move(issue));
        trimLocked();
    }
    generation_.fetch_add(1, std::memory_order_acq_rel);
}

void IssueTimeline::replaceAll(IssueSource source, std::vector<TimedIssue> issues)
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        issues_.erase(std::remove_if(issues_.begin(), issues_.end(),
                                     [source](const TimedIssue& i) { return i.source == source; }),
                      issues_.end());
        for (auto& issue : issues) issues_.push_back(std::move(issue));
        trimLocked();
    }
    generation_.fetch_add(1, std::memory_order_acq_rel);
}

void IssueTimeline::clear(IssueSource source) { replaceAll(source, {}); }

void IssueTimeline::clearAll()
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        issues_.clear();
    }
    generation_.fetch_add(1, std::memory_order_acq_rel);
}

void IssueTimeline::trimLocked()
{
    if (issues_.size() <= kMaxIssues) return;
    // Over the cap: keep the most severe findings, not merely the newest.
    std::stable_sort(issues_.begin(), issues_.end(),
                     [](const TimedIssue& a, const TimedIssue& b) { return a.severity > b.severity; });
    issues_.resize(kMaxIssues);
}

std::vector<TimedIssue> IssueTimeline::snapshot() const
{
    std::vector<TimedIssue> copy;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        copy = issues_;
    }
    std::stable_sort(copy.begin(), copy.end(),
                     [](const TimedIssue& a, const TimedIssue& b) { return a.startSec < b.startSec; });
    return copy;
}

std::vector<TimedIssue> IssueTimeline::mergedSnapshot() const { return merge(snapshot()); }

std::vector<TimedIssue> IssueTimeline::merge(std::vector<TimedIssue> issues)
{
    std::stable_sort(issues.begin(), issues.end(), [](const TimedIssue& a, const TimedIssue& b) {
        if (a.source != b.source) return a.source < b.source;
        if (a.regionName != b.regionName) return a.regionName < b.regionName;
        if (a.key != b.key) return a.key < b.key;
        return a.startSec < b.startSec;
    });
    std::vector<TimedIssue> merged;
    for (auto& issue : issues) {
        if (!merged.empty()) {
            auto& last = merged.back();
            if (last.source == issue.source && last.key == issue.key && last.regionName == issue.regionName
                && issue.startSec <= last.endSec + kMergeGapSec) {
                last.endSec = std::max(last.endSec, issue.endSec);
                if (issue.severity > last.severity) {
                    // The worst window speaks for the span (its text and fix).
                    last.severity = issue.severity;
                    last.title = issue.title;
                    last.action = issue.action;
                    last.suggestedParameters = issue.suggestedParameters;
                }
                continue;
            }
        }
        merged.push_back(std::move(issue));
    }
    std::stable_sort(merged.begin(), merged.end(),
                     [](const TimedIssue& a, const TimedIssue& b) { return a.startSec < b.startSec; });
    return merged;
}

size_t IssueTimeline::size() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return issues_.size();
}

} // namespace khensu::timeline
