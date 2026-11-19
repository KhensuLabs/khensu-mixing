#pragma once
#include <khensu/session/AudioFileRef.h>
#include <khensu/targets/CreativeTarget.h>
#include <string>
#include <vector>

namespace khensu {

/// A Khensu Mixing project session.
/// Sessions are persisted to SQLite by SessionManager.
/// THREAD_CONTEXT(WorkerThread | UIThread) - involves file I/O via SessionManager.
struct Session {
    std::string id; // UUID v4
    std::string name;
    std::string created_at; // ISO 8601 UTC
    std::string updated_at; // ISO 8601 UTC
    CreativeTarget target;
    std::vector<AudioFileRef> files;
    /// WF-12: the user's note on this version ("v3 - vocal up 1 dB").
    std::string note;

    bool is_valid() const noexcept { return !id.empty() && !name.empty(); }

    /// True for a session imported from a folder of stems: its files carry
    /// track roles (files[0] is the summed mixdown, role "mix").
    bool is_multitrack() const noexcept
    {
        for (const auto& f : files)
            if (!f.track_role.empty()) return true;
        return false;
    }

    /// The files the session's analysis grades (WF-7): a multitrack
    /// session's summed mixdown alone - its stems are the stem tools' inputs,
    /// not more mixes to average into one result - else every file.
    std::vector<AudioFileRef> analysedFiles() const
    {
        if (is_multitrack())
            for (const auto& f : files)
                if (f.track_role == "mix") return {f};
        return files;
    }
};

} // namespace khensu
