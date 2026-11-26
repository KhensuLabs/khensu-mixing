// desktop-app/Source/ui/shared/DecisionNotices.h
// Single-action, non-destructive decisions that used to be modal dialogs
// ("the service gave up - Restart?", "the session's file is missing -
// Locate?", "import this bundle?") and are now notices on the app-wide
// AppNoticeBar: one action button, dismissible, and a stable id so a repeat
// replaces the queued notice instead of stacking a second one.
// Destructive or multi-choice prompts (delete, overwrite, discard, reset)
// and forms stay dialogs.
#pragma once
#include <JuceHeader.h>
#include "AppNoticeBar.h"
#include <functional>
#include <utility>

namespace khensu::decision_notices {

/// The engine-state notice's id. The gave-up notice reuses it: it is the same
/// state with its cause, and the next status change withdraws it.
inline constexpr const char* kEngineStateId = "analysis-engine-state";
inline constexpr const char* kMissingSessionFileId = "missing-session-file";
inline constexpr const char* kBundleImportId = "bundle-import";

/// Posts `notice`, replacing a queued notice with the same id.
inline void postReplacing(AppNoticeBar& bar, AppNoticeBar::Notice notice)
{
    bar.withdraw(notice.id);
    bar.post(std::move(notice));
}

/// The local analysis service gave up restarting. `restart` is what the old
/// dialog's "Restart Service" did.
inline AppNoticeBar::Notice serviceGaveUp(const juce::String& cause, std::function<void()> restart)
{
    AppNoticeBar::Notice n;
    n.id = kEngineStateId;
    n.text = TRANS("The local analysis service failed to start after several attempts.") + " " + cause;
    n.actionLabel = TRANS("Restart Service");
    n.onAction = std::move(restart);
    return n;
}

/// The open session's audio file is missing. `locate` (the old dialog's
/// "Locate File...") runs only while `stillSameSession()` holds: the notice
/// can outlive the session it was posted for.
inline AppNoticeBar::Notice missingSessionFile(const juce::String& missingPath,
                                               std::function<bool()> stillSameSession,
                                               std::function<void()> locate)
{
    AppNoticeBar::Notice n;
    n.id = kMissingSessionFileId;
    n.text = TRANS("This session's audio file could not be found (it may have been moved or deleted):") + " " +
             missingPath;
    n.actionLabel = TRANS("Locate File...");
    n.onAction = [stillSameSession = std::move(stillSameSession), locate = std::move(locate)] {
        if (stillSameSession && !stillSameSession()) return;
        if (locate) locate();
    };
    return n;
}

/// One line describing a session bundle before it is imported.
inline juce::String bundleSummary(const juce::String& name, int audioFiles, int comments, int versions)
{
    juce::String s = TRANS("Import session bundle") + " \"" + name + "\": " + juce::String(audioFiles) +
                     TRANS(" audio file(s), ") + juce::String(comments) + TRANS(" comment(s), ") +
                     juce::String(versions) + TRANS(" version(s).") + " ";
    s += audioFiles == 0 ? TRANS("No audio: its notes attach to the open session.")
                         : TRANS("Its audio becomes a new session.");
    return s;
}

/// "Import this bundle?" - `import` is what the old dialog's "Import" did.
inline AppNoticeBar::Notice importBundle(const juce::String& summary, std::function<void()> import)
{
    AppNoticeBar::Notice n;
    n.id = kBundleImportId;
    n.text = summary;
    n.actionLabel = TRANS("Import");
    n.onAction = std::move(import);
    return n;
}

} // namespace khensu::decision_notices
