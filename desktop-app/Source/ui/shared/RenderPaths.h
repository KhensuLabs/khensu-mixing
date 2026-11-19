// desktop-app/Source/ui/shared/RenderPaths.h
#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <regex>

namespace khensu {

/// A fresh output file for a render of `source`: "<name><suffix>_<timestamp>-<n>.wav"
/// next to the source, made unique if that name already exists.
///
/// Fixed names ("_eq.wav", "_matched.wav", ...) overwrote earlier renders that
/// render history, undo and A/B still referenced, so undoing to an older node
/// silently played the newest audio - and overwrote a file the transport
/// still had open (AUDIT-018). The per-process counter keeps two renders
/// started within the same second (a macro drag queues several) from being
/// handed the same not-yet-written name.
inline juce::File uniqueRenderFile(const juce::File& source, const juce::String& suffix)
{
    static std::atomic<juce::uint32> counter{0};
    const juce::String stamp = juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S") + "-"
                               + juce::String(counter.fetch_add(1, std::memory_order_relaxed) % 1000u);
    const juce::File candidate =
        source.getSiblingFile(source.getFileNameWithoutExtension() + suffix + "_" + stamp + ".wav");
    return candidate.exists() ? candidate.getNonexistentSibling(false) : candidate;
}

/// True if `candidate` is a render THIS APP wrote for `source` via
/// uniqueRenderFile(): same directory, a .wav whose name is the source's name
/// + suffix + "_<YYYYMMDD-HHMMSS>[-n]" (+ an optional getNonexistentSibling
/// "_n" / " (n)"), and not the source itself. Session deletion only removes
/// files that pass this AND are in the session's render history, so a user's
/// own file is never deleted (AUDIT-058 policy).
inline bool isAppRenderFileFor(const juce::File& candidate, const juce::File& source)
{
    if (candidate == source || candidate.getParentDirectory() != source.getParentDirectory()) return false;
    if (!candidate.hasFileExtension("wav")) return false;
    const juce::String name = candidate.getFileNameWithoutExtension();
    const juce::String stem = source.getFileNameWithoutExtension();
    if (stem.isEmpty() || !name.startsWith(stem)) return false;
    static const std::regex kRenderTail(R"(^.*_\d{8}-\d{6}(-\d{1,3})?(_\d+| \(\d+\))?$)");
    return std::regex_match(name.substring(stem.length()).toStdString(), kRenderTail);
}

} // namespace khensu
