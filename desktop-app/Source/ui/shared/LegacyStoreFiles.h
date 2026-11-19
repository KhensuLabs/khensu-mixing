#pragma once
#include <JuceHeader.h>

namespace khensu::legacy_store {

/// PKG-4: the DB unification's leftovers in `configDir` - feedback.db,
/// service_feedback.db and provenance_events.db as `.bak` / `.migrated`
/// (+ -wal/-shm) - once they are older than `maxAge`. Only those names:
/// other `.bak` files in the folder (an unreadable C2PA sidecar's) are not
/// this upgrade's.
inline juce::Array<juce::File> retirable(const juce::File& configDir, juce::Time now, juce::RelativeTime maxAge)
{
    juce::Array<juce::File> files;
    for (const char* store : {"feedback.db", "service_feedback.db", "provenance_events.db"})
        for (const char* stage : {".bak", ".migrated"})
            for (const char* sidecar : {"", "-wal", "-shm"}) {
                const auto f = configDir.getChildFile(juce::String(store) + stage + sidecar);
                if (f.existsAsFile() && now - f.getLastModificationTime() > maxAge) files.add(f);
            }
    return files;
}

} // namespace khensu::legacy_store
