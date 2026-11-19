// desktop-app/Source/session/ProjectTracks.h
// FEAT-6: a project's tracks as the readiness board and the album tools see
// them - each session with its current master (the accepted render it last
// saved, else its mix) and its newest analysis summary.
// THREAD_CONTEXT(WorkerThread) - reads khensu.db; call off the message thread.
#pragma once
#include <JuceHeader.h>
#include <khensu/session/Project.h>
#include <khensu/session/SessionManager.h>
#include <optional>
#include <vector>

namespace khensu::project_tracks {

struct Track {
    std::string sessionId;
    juce::String name;
    juce::String masterPath; ///< what the album uses: the accepted render, else the mix
    bool masterIsRender = false;
    bool masterMissing = false; ///< the file is not on disk
    std::optional<SessionManager::AnalysisSummary> summary;
};

struct Snapshot {
    std::vector<Project> projects;  ///< every project, newest first
    std::optional<Project> current; ///< the active session's project
    std::vector<Track> tracks;      ///< `current`'s tracks, in order
};

/// The session's current master: the accepted render its workspace state
/// saved (when still on disk), else the file its analysis grades.
juce::String currentMasterPath(SessionManager& db, const Session& session, bool* isRender = nullptr);

/// One track's row (empty name when the session is gone).
Track loadTrack(SessionManager& db, const std::string& sessionId);

/// Every project, and the active session's project with its tracks.
Snapshot load(SessionManager& db, const std::string& activeSessionId);

/// A project name for the files of one drop: their shared folder's name,
/// else "N tracks".
juce::String nameForFiles(const std::vector<AudioFileRef>& files);

} // namespace khensu::project_tracks
