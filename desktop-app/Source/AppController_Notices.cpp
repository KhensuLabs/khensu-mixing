// desktop-app/Source/AppController_Notices.cpp
// Launch-time notices: "a crash report was saved last time" and "a new
// version is available". Both are app-wide, not session-scoped, so they take
// no session epoch - switching tracks must not drop them.
#include "AppController.h"
#include "ui/shared/AbxTrialOverlay.h"
#include "ui/shared/ChainReasoningOverlay.h"
#include "ui/shared/ExplainerOverlay.h"
#include "ui/shared/MetricDiffOverlay.h"
#include "ui/shared/OnboardingWizard.h"
#include "ui/shared/ReferenceTrackRitualOverlay.h"
#include "CrashReporter.h"
#include "UpdateInstaller.h"
#include "UpdateNotice.h"
#include "settings/OnlineSettings.h"
#include "ui/shared/ServiceErrorHint.h"
#include "ui/shared/ServiceStatusText.h"
#include <khensu/common/AppPaths.h>
#include <khensu/common/Version.h>
#include <khensu/session/PluginCapture.h>
#include "session/PluginCaptureInbox.h"

namespace khensu {

namespace {
juce::File configDir()
{
    return juce::File(juce::String(AppPaths::dataDirectory()));
}
} // namespace

void AppController::scanPluginCaptures()
{
    if (captureScanRunning_->exchange(true)) return; // the last scan is still running
    auto db = sessions_.sharedManager();
    if (db == nullptr || !db->isOpen()) {
        captureScanRunning_->store(false);
        return;
    }
    const auto dir = plugin_capture::capturesDirectory();
    auto running = captureScanRunning_;
    juce::Component::SafePointer<AppController> self(this);
    runInBackground([self, db, dir, running] {
        // Cheap when idle: one folder listing.
        auto imported = plugin_capture::pending(dir).isEmpty() ? std::vector<capture_inbox::Imported>{}
                                                                : capture_inbox::importPending(*db, dir);
        running->store(false);
        if (imported.empty()) return;
        juce::MessageManager::callAsync([self, imported] {
            if (self == nullptr) return;
            for (const auto& cap : imported) {
                AppNoticeBar::Notice n;
                n.id = "plugin-capture-" + juce::String(cap.sessionId);
                n.text = TRANS("A capture from the plugin is ready as a session: %n").replace("%n", cap.name);
                n.actionLabel = TRANS("Open");
                const auto id = cap.sessionId;
                n.onAction = [self, id] {
                    if (self != nullptr) self->loadSessionById(id);
                };
                self->noticeBar_.post(std::move(n));
            }
        });
    });
}

void AppController::postCrashNoticeIfAny()
{
    const auto dir = crash::defaultCrashesDir();
    const auto pending = crash::pendingNotice(dir);
    if (!pending.show) return;

    AppNoticeBar::Notice n;
    n.id = "crash-report";
    n.text = TRANS("Khensu closed unexpectedly last time - a crash report was saved locally (nothing was sent).");
    n.actionLabel = TRANS("Open folder");
    n.onAction = [dir, report = pending.newestReport] {
        crash::acknowledge(dir);
        if (report.existsAsFile())
            report.revealToUser();
        else
            dir.startAsProcess();
    };
    n.onDismiss = [dir] { crash::acknowledge(dir); };
    noticeBar_.post(std::move(n));
}

void AppController::updateServiceBanner(PythonStatus status, int restartAttempt)
{
    static const juce::String kId = "analysis-engine-state";
    const int generation = ++serviceBannerGeneration_;
    const juce::String text = service_status::bannerText(status, restartAttempt);
    if (text.isEmpty()) {
        noticeBar_.withdraw(kId);
        return;
    }
    juce::Component::SafePointer<AppController> self(this);
    auto show = [self, generation, status, text] {
        if (self == nullptr || self->serviceBannerGeneration_ != generation) return; // state moved on
        self->noticeBar_.withdraw(kId);
        AppNoticeBar::Notice n;
        n.id = kId;
        n.text = text;
        if (service_status::bannerOffersRestart(status)) {
            n.actionLabel = TRANS("Restart");
            n.onAction = [self] {
                if (self != nullptr) self->lifecycle_.restartPythonService();
            };
        }
        self->noticeBar_.post(std::move(n));
    };
    // A normal cold start is usually over in a few seconds: only a start that
    // is taking a while earns a banner, so a healthy launch doesn't flash one.
    if (status == PythonStatus::Starting && restartAttempt == 0)
        juce::Timer::callAfterDelay(3000, show);
    else
        show();
}

void AppController::updateSessionChrome()
{
    if (!sessionHealthStripEnabled_.has_value())
        sessionHealthStripEnabled_ = OnlineSettings::load(configDir()).showSessionHealthStrip;
    const bool session = hasActiveSession();
    const bool health = session && *sessionHealthStripEnabled_;
    if (mixHealthRollupStrip_.isVisible() == session && sessionHealthStrip_.isVisible() == health) return;
    mixHealthRollupStrip_.setVisible(session);
    sessionHealthStrip_.setVisible(health);
    resized();
}

void AppController::toggleSessionHealthStrip()
{
    if (!sessionHealthStripEnabled_.has_value())
        sessionHealthStripEnabled_ = OnlineSettings::load(configDir()).showSessionHealthStrip;
    const bool on = !*sessionHealthStripEnabled_;
    sessionHealthStripEnabled_ = on;
    OnlineSettings::update(configDir(), [on](OnlineSettings& s) { s.showSessionHealthStrip = on; });
    updateSessionChrome();
    showExplainer(TRANS("Session health strip"), TRANS("Display"),
                  on ? TRANS("Shown above the transport while a session is open: session time, time since your "
                             "last break, ear fatigue and how many changes you made in the last 10 minutes.")
                     : TRANS("Hidden. Turn it back on from the command palette (Ctrl+K)."));
}

void AppController::checkForAppUpdates()
{
    if (manualUpdateCheckInFlight_) return;
    manualUpdateCheckInFlight_ = true;
    startUpdateCheck(true);
}

void AppController::startUpdateCheck(bool manual)
{
    juce::Component::SafePointer<AppController> self(this);
    runInBackground([self, manual] {
        // The service applies the online toggles (master + update_check) and
        // the hourly throttle (skipped when the user asked); "disabled" means
        // no request left the machine.
        auto result = ipc::HttpServiceBridge{}.checkForUpdates(KHENSU_VERSION_STRING, manual);
        juce::MessageManager::callAsync([self, manual, result] {
            if (self == nullptr) return;
            if (manual) {
                self->manualUpdateCheckInFlight_ = false;
                self->noticeBar_.withdraw("update-check-result"); // the last answer, if still shown
            }
            if (result.is_err()) {
                // Launch check: offline / old service, say nothing. Asked for: say why.
                if (manual) {
                    AppNoticeBar::Notice n;
                    n.id = "update-check-result";
                    n.text = TRANS("Could not check for updates: ") + serviceErrorLine(result.error());
                    self->noticeBar_.post(std::move(n));
                }
                return;
            }
            const auto& update = result.value();
            const auto settings = OnlineSettings::load(configDir());

            // v1.0.0: the service downloads the installer in the background.
            // An installed copy installs it when the app closes; "Restart now"
            // does that at once and reopens the new version.
            const bool installedCopy = update_installer::isInstalledCopy(
                juce::File::getSpecialLocation(juce::File::currentExecutableFile));
            if (update.updateAvailable() && installedCopy && settings.autoDownloadUpdates) {
                if (update.installerReady() &&
                    update_installer::setPending(juce::File(update.installerPath), update.latestVersion)) {
                    self->noticeBar_.withdraw("update-downloading");
                    AppNoticeBar::Notice n;
                    n.id = "update-ready-" + update.latestVersion;
                    n.text = update_notice::readyText(update);
                    n.actionLabel = TRANS("Restart now");
                    n.onAction = [] {
                        if (update_installer::launchPending(/*relaunch=*/true))
                            if (auto* app = juce::JUCEApplicationBase::getInstance()) app->systemRequestedQuit();
                    };
                    self->noticeBar_.post(std::move(n));
                    return;
                }
                if (update.downloadState == "downloading") {
                    AppNoticeBar::Notice n;
                    n.id = "update-downloading";
                    n.text = update_notice::downloadingText(update);
                    self->noticeBar_.post(std::move(n));
                    // Ask again shortly: the answer comes from the service's
                    // cache and the download's state on disk - no new request.
                    if (self->updateDownloadPolls_++ < 180)
                        juce::Timer::callAfterDelay(20000, [self] {
                            if (self != nullptr) self->startUpdateCheck(false);
                        });
                    return;
                }
                // "failed" or no installer for this OS: offer the release page below.
            }

            // A manual check shows a newer release even if it was dismissed.
            const bool show = manual ? update.updateAvailable()
                                     : update_notice::shouldShow(update, settings.dismissedUpdateVersion);
            if (!show) {
                if (manual) {
                    AppNoticeBar::Notice n;
                    n.id = "update-check-result";
                    n.text = update_notice::manualText(update);
                    self->noticeBar_.post(std::move(n));
                }
                return;
            }

            AppNoticeBar::Notice n;
            n.id = "update-" + update.latestVersion;
            n.text = update_notice::text(update);
            if (update_notice::hasSafeDownload(update)) {
                // The browser downloads the installer; running it is the user's step.
                n.actionLabel = TRANS("Download installer");
                n.onAction = [url = update.downloadUrl] { juce::URL(url).launchInDefaultBrowser(); };
            } else if (update_notice::hasSafeLink(update)) {
                n.actionLabel = TRANS("View release");
                n.onAction = [url = update.url] { juce::URL(url).launchInDefaultBrowser(); };
            }
            // Dismissing hides this version for good; a later release shows again.
            n.onDismiss = [version = update.latestVersion] {
                OnlineSettings::update(configDir(), [&version](OnlineSettings& s) {
                    s.dismissedUpdateVersion = version;
                });
            };
            self->noticeBar_.post(std::move(n));
        });
    });
}

} // namespace khensu
