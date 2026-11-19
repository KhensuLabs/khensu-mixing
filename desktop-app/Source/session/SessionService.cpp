// desktop-app/Source/session/SessionService.cpp
#include "SessionService.h"

namespace khensu {

SessionService::StoreOpenResult SessionService::openStore(const std::string& dbPath, int attempts)
{
    StoreOpenResult out;
    for (int attempt = 0; attempt < attempts && out.manager == nullptr; ++attempt) {
        try {
            out.manager = std::make_shared<SessionManager>(dbPath);
        } catch (const SessionManager::SchemaTooNewError& e) {
            out.failure = e.what(); // a newer build's DB: not transient, don't retry
            break;
        } catch (const std::exception& e) {
            out.failure = e.what();
            juce::Logger::writeToLog("Session DB open attempt " + juce::String(attempt + 1) +
                                     " failed: " + out.failure);
            if (attempt + 1 < attempts) juce::Thread::sleep(250 << attempt);
        }
    }
    if (out.manager == nullptr) {
        out.degraded = true;
        juce::Logger::writeToLog("Session DB unavailable - running in degraded in-memory mode: " + out.failure);
        out.manager = std::make_shared<SessionManager>(":memory:");
    }
    return out;
}

std::optional<Session> SessionService::create(const juce::String& name, const std::vector<AudioFileRef>& files,
                                              const CreativeTarget& target)
{
    auto created = manager().createSession(name.toStdString(), target);
    if (!created.is_ok()) return std::nullopt;

    Session session = created.value();
    session.files = files;
    if (!manager().saveSession(session).is_ok()) {
        manager().deleteSession(session.id); // don't leave an orphaned record
        return std::nullopt;
    }
    return session;
}

SessionService::LoadOutcome SessionService::load(const std::string& id)
{
    LoadOutcome out;
    auto loaded = manager().loadSession(id);
    if (loaded.is_err()) {
        out.status = LoadStatus::Error;
        out.error = loaded.error();
        return out;
    }
    if (!loaded.value().has_value()) return out; // NotFound
    out.status = LoadStatus::Loaded;
    out.session = std::move(*loaded.value());
    return out;
}

std::optional<Session> SessionService::relinkSource(const juce::String& newPath)
{
    if (!state_.session || state_.session->files.empty()) return std::nullopt;
    state_.session->files[0].path = newPath.toStdString();
    return *state_.session;
}

std::optional<Session> SessionService::replaceTarget(const CreativeTarget& target)
{
    if (!state_.session) return std::nullopt;
    state_.session->target = target;
    return *state_.session;
}

std::vector<Session> SessionService::listRecent(int limit)
{
    auto sessions = manager().listSessions(limit);
    recentLoadFailed_ = sessions.is_err();
    return sessions.is_ok() ? std::move(sessions.value()) : std::vector<Session>{};
}

std::vector<SessionManager::AnalysisSummary> SessionService::listRecentAnalysisSummaries(int limit)
{
    auto summaries = manager().listRecentAnalysisSummaries(limit);
    recentAnalysisSummariesLoadFailed_ = summaries.is_err();
    return summaries.is_ok() ? std::move(summaries.value()) : std::vector<SessionManager::AnalysisSummary>{};
}

} // namespace khensu
