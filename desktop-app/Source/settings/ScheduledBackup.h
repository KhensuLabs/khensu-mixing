// desktop-app/Source/settings/ScheduledBackup.h
// Pure policy helpers for the automatic daily database backup (audit
// DATA-02). Header-only and free of AppController/SessionManager so the
// due-check and rotation are unit-testable in KhensuTests.
#pragma once
#include <JuceHeader.h>

namespace khensu::scheduled_backup {

inline constexpr int kKeepNewest = 7;
inline constexpr double kIntervalHours = 24.0;
inline constexpr const char* kFilePrefix = "khensu-backup-";

/// True when a scheduled backup should run now: never succeeded before,
/// an unparseable timestamp (fail toward backing up), a timestamp in the
/// future (clock moved back / settings copied from another machine - waiting
/// for it would silently skip backups until then), or >= 24 h elapsed.
inline bool isBackupDue(const juce::String& lastSuccessIso, juce::Time now)
{
    // fromISO8601 jasserts on input without a leading "YYYY-"; a corrupt
    // settings value is expected input here, not a programming error.
    const bool looksIso = lastSuccessIso.length() >= 10 && lastSuccessIso.substring(0, 4).containsOnly("0123456789")
                          && lastSuccessIso[4] == '-';
    if (!looksIso) return true;
    const auto last = juce::Time::fromISO8601(lastSuccessIso);
    if (last == juce::Time()) return true; // unparseable -> treat as never
    if (last > now) return true;
    return (now - last).inHours() >= kIntervalHours;
}

/// Deterministic backup filename for `now`, e.g.
/// "khensu-backup-20260807-153000.db".
inline juce::String backupFileName(juce::Time now)
{
    return juce::String(kFilePrefix) + now.formatted("%Y%m%d-%H%M%S") + ".db";
}

/// Deletes the oldest scheduled-backup files beyond `keep`, matching only
/// this module's filename pattern (a user's manual exports are untouched).
/// Returns the number deleted. Sort is by filename, which the timestamp
/// format makes chronological - no mtime trust needed.
inline int pruneOldBackups(const juce::File& dir, int keep = kKeepNewest)
{
    juce::StringArray names;
    for (const auto& f : dir.findChildFiles(juce::File::findFiles, false,
                                            juce::String(kFilePrefix) + "*.db"))
        names.add(f.getFileName());
    names.sort(false);
    int deleted = 0;
    for (int i = 0; i < names.size() - keep; ++i)
        if (dir.getChildFile(names[i]).deleteFile()) ++deleted;
    return deleted;
}

} // namespace khensu::scheduled_backup
