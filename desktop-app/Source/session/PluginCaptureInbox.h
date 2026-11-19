// desktop-app/Source/session/PluginCaptureInbox.h
// FEAT-7: the desktop half of a plugin capture. The plugin writes the audio
// and a manifest into <data dir>/captures/ (it never writes khensu.db); this
// turns each pending manifest into a session - with the target of the
// session the plugin followed - and marks it imported.
// THREAD_CONTEXT(WorkerThread) - reads and writes khensu.db and the folder.
#pragma once
#include <JuceHeader.h>
#include <khensu/session/SessionManager.h>
#include <vector>

namespace khensu::capture_inbox {

struct Imported {
    std::string sessionId;
    juce::String name;
};

/// Imports every pending capture in `dir` (oldest first). A manifest whose
/// audio is missing or unreadable is set aside as imported, not retried
/// forever. Returns what became sessions.
std::vector<Imported> importPending(SessionManager& db, const juce::File& dir);

} // namespace khensu::capture_inbox
