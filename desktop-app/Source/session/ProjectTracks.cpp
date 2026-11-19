// desktop-app/Source/session/ProjectTracks.cpp
#include "ProjectTracks.h"
#include "WorkspaceRecoveryState.h"

namespace khensu::project_tracks {

juce::String currentMasterPath(SessionManager& db, const Session& session, bool* isRender)
{
    if (isRender != nullptr) *isRender = false;
    const auto saved = db.getUserPreference("workspace_recovery_" + session.id);
    if (saved.is_ok() && !saved.value().empty())
        if (const auto ws = workspaceRecoveryStateFromJson(juce::String(saved.value())))
            if (ws->renderPath.isNotEmpty() && juce::File(ws->renderPath).existsAsFile()) {
                if (isRender != nullptr) *isRender = true;
                return ws->renderPath;
            }
    const auto files = session.analysedFiles();
    return files.empty() ? juce::String() : juce::String(files.front().path);
}

Track loadTrack(SessionManager& db, const std::string& sessionId)
{
    Track t;
    t.sessionId = sessionId;
    const auto loaded = db.loadSession(sessionId);
    if (loaded.is_err() || !loaded.value()) return t;
    const Session& s = *loaded.value();
    t.name = juce::String(s.name);
    t.masterPath = currentMasterPath(db, s, &t.masterIsRender);
    t.masterMissing = t.masterPath.isEmpty() || !juce::File(t.masterPath).existsAsFile();
    if (auto sum = db.latestAnalysisSummary(sessionId); sum.is_ok()) t.summary = sum.value();
    return t;
}

Snapshot load(SessionManager& db, const std::string& activeSessionId)
{
    Snapshot snap;
    if (auto all = db.listProjects(); all.is_ok()) snap.projects = all.value();
    if (activeSessionId.empty()) return snap;
    if (auto p = db.projectOfSession(activeSessionId); p.is_ok() && p.value()) {
        snap.current = p.value();
        for (const auto& sid : snap.current->session_ids) {
            auto t = loadTrack(db, sid);
            if (t.name.isNotEmpty()) snap.tracks.push_back(std::move(t));
        }
    }
    return snap;
}

juce::String nameForFiles(const std::vector<AudioFileRef>& files)
{
    if (files.empty()) return {};
    const auto parent = juce::File(juce::String(files.front().path)).getParentDirectory();
    for (const auto& f : files)
        if (juce::File(juce::String(f.path)).getParentDirectory() != parent)
            return TRANS("%n tracks").replace("%n", juce::String((int)files.size()));
    const auto name = parent.getFileName();
    return name.isNotEmpty() ? name : TRANS("%n tracks").replace("%n", juce::String((int)files.size()));
}

} // namespace khensu::project_tracks
