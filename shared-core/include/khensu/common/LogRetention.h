// shared-core/include/khensu/common/LogRetention.h
// SHIP-7: crash and terminate logs are written one file per event and were
// never removed. pruneOldLogs keeps the newest `keep` files matching a
// wildcard in one directory and deletes the rest.
#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>

namespace khensu::logs {

/// How many of each crash-log kind are kept.
inline constexpr int kKeepCrashLogs = 20;

/// Deletes all but the `keep` most recently modified files in `dir` (not its
/// subdirectories) whose names match `wildcard`. Returns how many were
/// deleted. A missing directory, or one at or under the limit, is left alone.
inline int pruneOldLogs(const juce::File& dir, const juce::String& wildcard, int keep)
{
    if (!dir.isDirectory()) return 0;
    auto files = dir.findChildFiles(juce::File::findFiles, false, wildcard);
    if (files.size() <= keep) return 0;
    std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b) {
        return a.getLastModificationTime() > b.getLastModificationTime();
    });
    int removed = 0;
    for (int i = std::max(0, keep); i < files.size(); ++i)
        if (files.getReference(i).deleteFile()) ++removed;
    return removed;
}

} // namespace khensu::logs
