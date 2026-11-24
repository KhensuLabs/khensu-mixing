// desktop-app/Source/ui/shared/TimelineCache.h
// VIZ-2: one measured TrackTimeline per file for every view that draws time
// (the Analysis timeline, the rack's limiter history, the before/after views,
// the report), so a file is decoded and measured once. Keyed by path plus
// size and modification time, so a re-rendered file is measured again. Holds
// the last few; thread-safe (measured on a worker, read on the UI thread).
// THREAD_CONTEXT(Any)
#pragma once
#include <JuceHeader.h>
#include <deque>
#include <memory>
#include <mutex>
#include <khensu/analysis/TrackTimeline.h>

namespace khensu::timeline_cache {

using Ptr = std::shared_ptr<const TrackTimeline>;

namespace detail {
struct Entry {
    juce::String key;
    Ptr timeline;
};
inline std::mutex& mutex()
{
    static std::mutex m;
    return m;
}
inline std::deque<Entry>& entries()
{
    static std::deque<Entry> e;
    return e;
}
inline juce::String keyOf(const juce::String& path)
{
    const juce::File f(path);
    return path + "|" + juce::String(f.getSize()) + "|" + juce::String(f.getLastModificationTime().toMilliseconds());
}
} // namespace detail

inline constexpr size_t kCapacity = 6;

/// The cached timeline of `path`, or nullptr.
inline Ptr find(const juce::String& path)
{
    if (path.isEmpty()) return nullptr;
    const auto key = detail::keyOf(path);
    std::lock_guard<std::mutex> lock(detail::mutex());
    for (const auto& e : detail::entries())
        if (e.key == key) return e.timeline;
    return nullptr;
}

/// The cached timeline, or measures it now (call on a worker thread).
inline Ptr measure(const juce::String& path)
{
    if (auto hit = find(path)) return hit;
    auto tl = std::make_shared<const TrackTimeline>(measureTrackTimelineFile(path.toStdString()));
    if (tl->empty()) return nullptr;
    std::lock_guard<std::mutex> lock(detail::mutex());
    auto& e = detail::entries();
    e.push_front({detail::keyOf(path), tl});
    while (e.size() > kCapacity)
        e.pop_back();
    return tl;
}

} // namespace khensu::timeline_cache
