#pragma once
#include "../../services/IIssues.h"
#include <algorithm>
#include <vector>

namespace khensu::issue_playlist {

/// FEAT-8: the issues that happen at a time, in time order - the indices into
/// `issues` of the open ones with a time range, by start time.
inline std::vector<size_t> timedOrder(const std::vector<IIssues::Issue>& issues)
{
    std::vector<size_t> order;
    for (size_t i = 0; i < issues.size(); ++i)
        if (!issues[i].settled() && issues[i].rec.time_range.has_value()) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&issues](size_t a, size_t b) {
        return issues[a].rec.time_range->start_sec < issues[b].rec.time_range->start_sec;
    });
    return order;
}

/// The position in `order` after (or before) `current`; wraps; -1 when empty.
/// `current` -1 starts at the first (or, going back, the last).
inline int step(int current, int count, bool forward)
{
    if (count <= 0) return -1;
    if (current < 0) return forward ? 0 : count - 1;
    return forward ? (current + 1) % count : (current - 1 + count) % count;
}

/// The region a timed finding is auditioned over: half a second either side,
/// at least two seconds long.
inline std::pair<double, double> region(const TimeRange& t)
{
    double start = std::max(0.0, (double)t.start_sec - 0.5);
    double end = std::max((double)t.end_sec + 0.5, start + 2.0);
    return {start, end};
}

} // namespace khensu::issue_playlist
