// desktop-app/Source/AppController_Bounce.cpp
// ENH-1: a new bounce of the open session's track (session/BounceWatcher.h)
// becomes a version of the session - registered in the render tree as a
// render of the original, so it is the current version (B in the A/B), the
// fix check (FEAT-1) compares it with the original's findings, the issue
// queue shows what it resolved, and Mix / Master's What Changed card diffs
// it. A notice says it happened and, once checked, what changed.
#include "AppController.h"
#include "analysis/FixVerification.h"
#include "ui/shared/RenderPaths.h"

namespace khensu {

namespace {
constexpr const char* kBounceFilePref = "bounce_watch_file";     // "off" = off (default on)
constexpr const char* kBounceFolderPref = "bounce_watch_folder"; // "on" = on (default off)
constexpr const char* kBounceNoticeId = "bounce-new";
} // namespace

void AppController::wireBounceWatcher()
{
    BounceWatcher::Settings settings;
    if (auto v = sessionManager().getUserPreference(kBounceFilePref); v.is_ok())
        settings.watchFile = v.value() != "off";
    if (auto v = sessionManager().getUserPreference(kBounceFolderPref); v.is_ok())
        settings.watchFolder = v.value() == "on";
    bounceWatcher_.setSettings(settings);
    bounceWatcher_.sourceProvider = [this] { return hasActiveSession() ? activeSourcePath() : juce::String(); };
    bounceWatcher_.isKnownVersion = [this](const juce::File& f) {
        for (const auto& node : renders_.renderTree())
            if (juce::File(node.path) == f) return true;
        return false;
    };
    bounceWatcher_.onNewBounce = [this](const juce::File& bounce, bool inPlace) { handleNewBounce(bounce, inPlace); };
    analysis_.addVerificationListener(&bounceWatcher_, [this] { onBounceChecked(); });
    bounceWatcher_.start();
}

void AppController::setBounceWatch(bool watchFile, bool watchFolder)
{
    sessionManager().setUserPreference(kBounceFilePref, watchFile ? "on" : "off");
    sessionManager().setUserPreference(kBounceFolderPref, watchFolder ? "on" : "off");
    bounceWatcher_.setSettings({watchFile, watchFolder});
    juce::String text = !watchFile && !watchFolder
                            ? TRANS("New bounces are no longer watched.")
                            : TRANS("Watching for new bounces:") + " " +
                                  (watchFile ? TRANS("the session's file") : juce::String()) +
                                  (watchFile && watchFolder ? " " + TRANS("and") + " " : juce::String()) +
                                  (watchFolder ? TRANS("newer versions beside it (song_v2...)") : juce::String()) + ".";
    noticeBar_.withdraw("bounce-watch-setting");
    noticeBar_.post({"bounce-watch-setting", text, {}, {}, {}});
}

void AppController::handleNewBounce(const juce::File& bounce, bool inPlace)
{
    if (!hasActiveSession()) return;
    const juce::String sessionId = sessions_.activeSessionId();
    const juce::String source = activeSourcePath();
    const auto announce = [this, bounce] {
        noticeBar_.withdraw(kBounceNoticeId);
        noticeBar_.post({kBounceNoticeId,
                         TRANS("New bounce of this track: %f - checking it against the findings...")
                             .replace("%f", bounce.getFileName()),
                         {},
                         {},
                         {}});
    };
    if (!inPlace) {
        // A new file beside the original: it is the version itself.
        announce();
        pendingBounceCheck_ = bounce.getFullPathName();
        renders_.registerRenderOf(pendingBounceCheck_, source, sessionId);
        return;
    }
    // Bounced over the session's own file: keep that content as a version
    // (a copy beside it, named like the app's renders), so the next bounce
    // over it can't change what this version holds. Copied off the message
    // thread - a bounce is tens of MB.
    announce();
    const juce::File copy = uniqueRenderFile(bounce, "_bounce");
    juce::Component::SafePointer<AppController> self(this);
    runInBackground([self, bounce, copy, sessionId, source] {
        const bool ok = bounce.copyFileTo(copy);
        juce::MessageManager::callAsync([self, ok, copy, sessionId, source] {
            if (self == nullptr) return;
            if (self->sessions_.activeSessionId() != sessionId) { // left the session meanwhile
                if (ok) copy.deleteFile();
                return;
            }
            if (!ok) {
                self->noticeBar_.withdraw(kBounceNoticeId);
                self->noticeBar_.post(
                    {kBounceNoticeId,
                     TRANS("A new bounce of this track arrived but could not be copied as a version."),
                     {},
                     {},
                     {}});
                return;
            }
            self->pendingBounceCheck_ = copy.getFullPathName();
            self->renders_.registerRenderOf(self->pendingBounceCheck_, source, sessionId);
        });
    });
}

void AppController::onBounceChecked()
{
    if (pendingBounceCheck_.isEmpty()) return;
    const auto* check = analysis_.currentVerification();
    if (check == nullptr || juce::String(check->renderPath) != pendingBounceCheck_) return;
    pendingBounceCheck_.clear();
    using fix_verification::Outcome;
    const int resolved = check->count(Outcome::Resolved);
    const int added = check->count(Outcome::New);
    const int open = check->count(Outcome::Improved) + check->count(Outcome::Unchanged) + check->count(Outcome::Worse);
    AppNoticeBar::Notice notice;
    notice.id = kBounceNoticeId;
    notice.text = TRANS("New bounce checked: %r resolved, %n new, %o still open.")
                      .replace("%r", juce::String(resolved))
                      .replace("%n", juce::String(added))
                      .replace("%o", juce::String(open));
    notice.actionLabel = TRANS("What changed");
    juce::Component::SafePointer<AppController> self(this);
    notice.onAction = [self] {
        if (self != nullptr) self->showScreen(ScreenType::MixMaster); // the What Changed card diffs A and B
    };
    noticeBar_.withdraw(kBounceNoticeId);
    noticeBar_.post(std::move(notice));
}

} // namespace khensu
