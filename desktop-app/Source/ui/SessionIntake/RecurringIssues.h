// desktop-app/Source/ui/SessionIntake/RecurringIssues.h
// AUTO-5: which findings come back across the user's tracks. Pure and
// header-only: the panel feeds it each scanned session's measured findings
// (/agent/findings), newest session first, and shows what it returns.
#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <string>
#include <vector>

namespace khensu::recurring_issues {

struct Finding {
    juce::String code;  ///< "agent:mud_rel_db"
    juce::String title; ///< "Low-mid build-up (mud)"
    double severity = 0.0;
};

struct SessionFindings {
    std::string sessionId;
    juce::String sessionName;
    juce::String updatedAt; ///< ISO-8601; sorts as text
    std::vector<Finding> findings;
};

enum class Trend { MoreOften, Steady, LessOften };

struct Recurring {
    juce::String code, title;
    int count = 0;         ///< tracks it was found in
    int scanned = 0;       ///< tracks scanned
    juce::String lastSeen; ///< the newest such track's updatedAt
    std::string latestSessionId;
    juce::String latestSessionName;
    double meanSeverity = 0.0;
    Trend trend = Trend::Steady;
};

/// How often a code turns up in the newer half of the tracks against the
/// older half; a gap of more than `margin` (as a share of tracks) is a trend.
inline Trend trendOf(const std::vector<bool>& foundNewestFirst, double margin = 0.15)
{
    const size_t n = foundNewestFirst.size();
    if (n < 4) return Trend::Steady; // too few to say
    const size_t newer = n / 2;
    int a = 0, b = 0;
    for (size_t i = 0; i < n; ++i)
        (i < newer ? a : b) += foundNewestFirst[i] ? 1 : 0;
    const double rateNew = static_cast<double>(a) / static_cast<double>(newer);
    const double rateOld = static_cast<double>(b) / static_cast<double>(n - newer);
    if (rateNew - rateOld > margin) return Trend::MoreOften;
    if (rateOld - rateNew > margin) return Trend::LessOften;
    return Trend::Steady;
}

/// The findings seen in at least `minCount` tracks, most frequent first
/// (then the most severe). `sessions` is newest first.
inline std::vector<Recurring> aggregate(const std::vector<SessionFindings>& sessions, int minCount = 2)
{
    std::vector<Recurring> out;
    std::vector<size_t> lastCounted; // per entry of `out`: the session index it was last counted in
    const int scanned = static_cast<int>(sessions.size());
    for (size_t si = 0; si < sessions.size(); ++si) {
        const auto& s = sessions[si];
        for (const auto& f : s.findings) {
            auto it = std::find_if(out.begin(), out.end(), [&](const Recurring& r) { return r.code == f.code; });
            if (it == out.end()) {
                Recurring r;
                r.code = f.code;
                r.title = f.title.isNotEmpty() ? f.title : f.code;
                out.push_back(r);
                lastCounted.push_back(sessions.size()); // none yet
                it = out.end() - 1;
            }
            // The same code twice in one track counts once.
            auto& counted = lastCounted[static_cast<size_t>(it - out.begin())];
            if (counted == si) continue;
            counted = si;
            if (it->count == 0 || s.updatedAt > it->lastSeen) {
                it->lastSeen = s.updatedAt;
                it->latestSessionId = s.sessionId;
                it->latestSessionName = s.sessionName;
            }
            it->meanSeverity += f.severity;
            ++it->count;
        }
    }
    for (auto& r : out) {
        r.scanned = scanned;
        if (r.count > 0) r.meanSeverity /= r.count;
        std::vector<bool> found;
        for (const auto& s : sessions)
            found.push_back(
                std::any_of(s.findings.begin(), s.findings.end(), [&](const Finding& f) { return f.code == r.code; }));
        r.trend = trendOf(found);
    }
    out.erase(std::remove_if(out.begin(), out.end(), [minCount](const Recurring& r) { return r.count < minCount; }),
              out.end());
    std::stable_sort(out.begin(), out.end(), [](const Recurring& a, const Recurring& b) {
        if (a.count != b.count) return a.count > b.count;
        return a.meanSeverity > b.meanSeverity;
    });
    return out;
}

inline juce::String trendText(Trend t)
{
    switch (t) {
    case Trend::MoreOften:
        return TRANS("more often lately");
    case Trend::LessOften:
        return TRANS("less often lately");
    case Trend::Steady:
        break;
    }
    return TRANS("steady");
}

} // namespace khensu::recurring_issues
