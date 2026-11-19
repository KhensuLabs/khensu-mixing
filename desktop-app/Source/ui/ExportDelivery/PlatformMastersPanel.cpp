// desktop-app/Source/ui/ExportDelivery/PlatformMastersPanel.cpp
#include "PlatformMastersPanel.h"
#include <khensu/session/Session.h>
#include <khensu/ipc/generated/ApiJobStatus.h>
#include "../shared/KhensuTheme.h"
#include "../shared/JobPoller.h"
#include "../shared/SessionAudioPaths.h"
#include "../shared/ServiceErrorHint.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <thread>
#include <khensu/ipc/generated/ApiSchema.h>

namespace af = khensu::api::fields;

namespace khensu {

namespace {
constexpr int kPollIntervalMs = 1000;
// Per-target entry states of a platform-masters result (WIRE-10): the
// generated job states where they apply, plus the one entry-only state
// (platform_render.py: written and peak-safe, but short of the target).
namespace entry_status {
inline constexpr const char* completed = khensu::api::job_status::completed;
inline constexpr const char* could_not_reach = "could_not_reach";
inline constexpr const char* rejected = "rejected"; // guardrail refused it: no file at output_path, see quarantine_path
} // namespace entry_status
} // namespace

// Every cached palette colour on this component, in one place (UI-01).
void PlatformMastersPanel::applyThemeColours()
{
    runButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal);
    runButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::bgVoid);
    cancelButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentRed.withAlpha(0.35f));
    overrideButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentAmber.withAlpha(0.25f));
    overrideButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentAmber);
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    resultsLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

PlatformMastersPanel::PlatformMastersPanel(PanelCardController& c, const juce::String& baseUrl)
    : PanelCard(c, "Platform Masters"), baseUrl_(baseUrl)
{
    applyThemeColours();
    setExplainer(TRANS("Platform Masters"),
                 TRANS("Safety variants: one click renders a version of your master for each delivery target "
                       "(Spotify, Apple Music, club/DJ, YouTube) that keeps your loudness and is only made "
                       "true-peak safe for that platform's ceiling, so it won't clip after lossy encoding. "
                       "Tick \"Match each platform's loudness\" to also bring each one to the platform's "
                       "target (Spotify -14, Apple Music -16, club -9, YouTube -14); the platforms turn louder "
                       "masters down anyway. Every render runs through the post-render guardrail. Outputs land "
                       "in a platform_masters folder next to the source."));
    matchLoudnessToggle_.setTooltip(TRANS("Also normalise each version to its platform's loudness target"));
    addAndMakeVisible(matchLoudnessToggle_);

    runButton_.onClick = [this] { run(); };
    addAndMakeVisible(runButton_);

    cancelButton_.onClick = [this] { cancelRun(); };
    cancelButton_.setVisible(false);
    addChildComponent(cancelButton_);

    statusLabel_.setFont(KhensuTheme::labelFont(10.0f));
    addAndMakeVisible(statusLabel_);

    overrideButton_.setTooltip(TRANS("The guardrail rejected an output and quarantined it. Override it with an "
                                     "expert justification to promote the quarantined file."));
    overrideButton_.onClick = [this] { offerOverride(); };
    overrideButton_.setVisible(false);
    addChildComponent(overrideButton_);

    resultsLabel_.setFont(KhensuTheme::labelFont(9.5f));
    resultsLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(resultsLabel_);

    refreshReadiness();
}

PlatformMastersPanel::~PlatformMastersPanel()
{
    if (pollStop_) pollStop_->store(true); // the poller cancels the job server-side
}

std::string PlatformMastersPanel::bridgeUrl() const
{
    return baseUrl_.isNotEmpty() ? baseUrl_.toStdString() : std::string(ipc::HttpServiceBridge::kDefaultBaseUrl);
}

void PlatformMastersPanel::visibilityChanged()
{
    if (isVisible() && currentJobId_.isEmpty() && !startInFlight_) refreshReadiness();
}

void PlatformMastersPanel::refreshIfIdle()
{
    if (currentJobId_.isEmpty() && !startInFlight_) refreshReadiness();
}

void PlatformMastersPanel::refreshReadiness()
{
    const bool ready = controller_.hasActiveSession() && !controller_.getActiveSession()->files.empty();
    runButton_.setEnabled(ready);
    statusLabel_.setText(ready ? TRANS("Renders the current master to all delivery targets.")
                               : TRANS("Load a session first."),
                         juce::dontSendNotification);
}

void PlatformMastersPanel::run()
{
    if (!currentJobId_.isEmpty() || startInFlight_) return;
    if (!controller_.hasActiveSession() || controller_.getActiveSession()->files.empty()) {
        refreshReadiness();
        return;
    }
    // "The current master" = what Export writes: the accepted render when
    // there is one, not always the original mix (UIX-06).
    const juce::String src = session_audio::exportSourcePath(controller_);
    runSourcePath_ = src;
    rejected_.clear();
    overrideButton_.setVisible(false);

    runButton_.setEnabled(false);
    cancelButton_.setVisible(true);
    resized();
    statusLabel_.setText(TRANS("Starting..."), juce::dontSendNotification);
    resultsLabel_.setText({}, juce::dontSendNotification);

    startInFlight_ = true;
    cancelRequestedWhileStarting_ = false;

    juce::Component::SafePointer<PlatformMastersPanel> self(this);
    const bool matchLoudness = matchLoudnessToggle_.getToggleState();
    controller_.runLongInBackground([self, src, matchLoudness, url = bridgeUrl()]() {
        ipc::HttpServiceBridge bridge(url);
        auto result = bridge.startPlatformMastersJob(src, {}, {}, matchLoudness);
        juce::MessageManager::callAsync([self, result = std::move(result)]() mutable {
            if (self == nullptr) return;
            self->startInFlight_ = false;
            // No session-epoch guard here: the server job exists either way,
            // and dropping this result would leave it rendering unwatched
            // with the panel's Cancel/Run controls stuck.
            if (self->cancelRequestedWhileStarting_) {
                // Cancelled while /start was in flight: cancelRun() already
                // reset the UI. Just cancel server-side now that we finally
                // have a jobId, instead of letting it render unattended.
                self->cancelRequestedWhileStarting_ = false;
                if (result.is_ok()) {
                    const auto jobId = result.value();
                    self->controller_.runInBackground([jobId, url = self->bridgeUrl()]() {
                        ipc::HttpServiceBridge bridge(url);
                        bridge.cancelPlatformMastersJob(jobId);
                    });
                }
                return;
            }

            if (result.is_err()) {
                self->cancelButton_.setVisible(false);
                self->resized();
                self->statusLabel_.setText(serviceErrorLine(result.error()), juce::dontSendNotification);
                self->refreshReadiness();
                return;
            }
            self->currentJobId_ = result.value();
            self->pollJob(self->currentJobId_);
        });
    });
}

void PlatformMastersPanel::pollJob(const juce::String& jobId)
{
    if (pollStop_) pollStop_->store(true);
    auto stop = std::make_shared<std::atomic<bool>>(false);
    pollStop_ = stop;
    juce::Component::SafePointer<PlatformMastersPanel> self(this);
    // Not session-guarded: the render belongs to the job id, not the session;
    // abandoning it would freeze its progress and controls.
    controller_.runLongInBackground([self, jobId, stop, url = bridgeUrl()]() {
        ipc::HttpServiceBridge bridge(url);
        using Poller = JobPoller<ipc::HttpServiceBridge::PlatformMastersJobStatus>;
        Poller::Hooks hooks;
        hooks.poll = [&bridge](const std::string& id) { return bridge.getPlatformMastersJobStatus(juce::String(id)); };
        hooks.cancel = [&bridge](const std::string& id) { bridge.cancelPlatformMastersJob(juce::String(id)); };
        hooks.isTerminal = [](const auto& st) { return !khensu::api::isActiveStatus(st.status); };
        hooks.onProgress = [self, jobId](const auto& st) {
            juce::String t = TRANS("Rendering... %d / %t").replace("%d", juce::String(st.done))
                                 .replace("%t", juce::String(st.total));
            if (st.current.isNotEmpty()) t += "  (" + st.current + ")";
            juce::MessageManager::callAsync([self, jobId, t]() {
                if (self != nullptr && jobId == self->currentJobId_)
                    self->statusLabel_.setText(t, juce::dontSendNotification);
            });
        };
        hooks.shouldStop = [stop] { return stop->load(); };
        Poller::Options opts;
        opts.pollIntervalMs = kPollIntervalMs;
        opts.maxConsecutiveErrors = 6;
        opts.deadlineSeconds = 60 * 60;
        const auto out = Poller::run(hooks, opts, jobId.toStdString());

        juce::MessageManager::callAsync([self, jobId, out]() {
            if (self == nullptr || jobId != self->currentJobId_) return; // cancelled meanwhile
            self->currentJobId_ = {};
            self->cancelButton_.setVisible(false);
            self->resized();
            if (!out.finished() || !out.status.has_value()) {
                self->statusLabel_.setText(out.describe(), juce::dontSendNotification);
                self->refreshReadiness();
                return;
            }
            self->finishRun(out.status->status, out.status->results);
        });
    });
}

void PlatformMastersPanel::finishRun(const juce::String& status, const juce::var& results)
{
    if (status == khensu::api::job_status::cancelled) {
        statusLabel_.setText(TRANS("Cancelled."), juce::dontSendNotification);
        refreshReadiness();
        return;
    }
    if (status != khensu::api::job_status::completed) {
        statusLabel_.setText(TRANS("Render failed."), juce::dontSendNotification);
        refreshReadiness();
        return;
    }

    // One line per target: name, achieved LUFS / true peak, guardrail flag.
    juce::String outputDir;
    juce::StringArray lines;
    int flagged = 0;
    rejected_.clear();
    const juce::String sessionId =
        controller_.getActiveSession() != nullptr ? juce::String(controller_.getActiveSession()->id) : juce::String();
    // How hard the limiter worked to reach a target (null when none was needed).
    auto limiting = [](const juce::var& rv) {
        const juce::var gr = rv[af::PlatformMasterEntry::max_gain_reduction_db];
        if (!(gr.isDouble() || gr.isInt() || gr.isInt64()) || std::abs((double)gr) < 0.05) return juce::String();
        return "   " + TRANS("limiting %g dB").replace("%g", juce::String(std::abs((double)gr), 1));
    };
    // achieved_* is null when the master couldn't be measured
    // (platform_render.py) - show n/a rather than a fake 0.0.
    auto num1 = [](const juce::var& v) {
        return v.isDouble() || v.isInt() || v.isInt64() ? juce::String((double)v, 1) : TRANS("n/a");
    };
    if (auto* arr = results.getArray()) {
        for (const auto& rv : *arr) {
            const juce::String name = rv[af::PlatformMasterEntry::platform_name].toString();
            const juce::String entryStatus = rv[af::PlatformMasterEntry::status].toString();
            if (entryStatus == entry_status::rejected) {
                // Nothing was delivered: the master is quarantined. Offer the
                // expert override instead of pointing at a missing file.
                GuardrailRejection r;
                r.guardrailStatus = "rejected";
                r.serverRenderState = rv[af::PlatformMasterEntry::render_state].toString();
                r.sourcePath = runSourcePath_;
                r.quarantinePath = rv[af::PlatformMasterEntry::quarantine_path].toString();
                r.outputPath = rv[af::PlatformMasterEntry::output_path].toString();
                r.measurements = rv[af::PlatformMasterEntry::guardrail];
                r.sessionId = sessionId;
                if (r.quarantinePath.isNotEmpty()) rejected_.push_back(r);
                lines.add(name + ":  (!) " + TRANS("rejected by the guardrail - quarantined, not delivered") + limiting(rv));
                ++flagged;
                continue;
            }
            if (entryStatus == entry_status::could_not_reach) {
                // The master was written and is peak-safe; it just
                // landed more than 0.5 LU short of the platform target.
                if (outputDir.isEmpty() && rv[af::PlatformMasterEntry::output_path].toString().isNotEmpty())
                    outputDir = juce::File(rv[af::PlatformMasterEntry::output_path].toString()).getParentDirectory().getFullPathName();
                lines.add(name + ":  " + num1(rv[af::PlatformMasterEntry::achieved_lufs]) + " LUFS  /  "
                          + num1(rv[af::PlatformMasterEntry::achieved_true_peak_dbtp]) + " dBTP   (!) "
                          + TRANS("missed target by") + " "
                          + juce::String(std::abs((double)rv[af::PlatformMasterEntry::loudness_miss_lu]), 1) + " LU"
                          + limiting(rv));
                ++flagged;
                continue;
            }
            if (entryStatus != entry_status::completed) {
                lines.add(name + ": " + TRANS("failed") + " - " + rv[af::PlatformMasterEntry::error].toString());
                ++flagged;
                continue;
            }
            if (outputDir.isEmpty() && rv[af::PlatformMasterEntry::output_path].toString().isNotEmpty())
                outputDir = juce::File(rv[af::PlatformMasterEntry::output_path].toString()).getParentDirectory().getFullPathName();
            const bool guardOk = rv[af::PlatformMasterEntry::guardrail].isObject() ? (bool)rv[af::PlatformMasterEntry::guardrail][af::RenderGuardrail::ok] : true;
            if (!guardOk) ++flagged;
            lines.add(name + ":  " + num1(rv[af::PlatformMasterEntry::achieved_lufs]) + " LUFS  /  " + num1(rv[af::PlatformMasterEntry::achieved_true_peak_dbtp])
                      + " dBTP" + limiting(rv) + (guardOk ? juce::String() : "   (!) " + TRANS("guardrail")));
        }
    }

    statusLabel_.setText(TRANS("Rendered %n master(s) -> %d").replace("%n", juce::String(lines.size())).replace("%d", outputDir),
                         juce::dontSendNotification);
    resultsLabel_.setColour(juce::Label::textColourId, flagged > 0 ? KhensuTheme::accentAmber : KhensuTheme::accentTeal);
    resultsLabel_.setText(lines.joinIntoString("\n"), juce::dontSendNotification);
    overrideButton_.setVisible(!rejected_.empty() && onGuardrailOverride != nullptr);
    resized();
    runButton_.setEnabled(true);
}

void PlatformMastersPanel::cancelRun()
{
    if (currentJobId_.isEmpty() && !startInFlight_) return;
    if (pollStop_) pollStop_->store(true); // the poller cancels server-side

    if (startInFlight_) {
        cancelRequestedWhileStarting_ = true;
    } else {
        const juce::String jobId = currentJobId_;
        controller_.runLongInBackground([jobId, url = bridgeUrl()]() {
            ipc::HttpServiceBridge bridge(url);
            bridge.cancelPlatformMastersJob(jobId); // fire-and-forget (idempotent with the poller's cancel)
        });
    }

    currentJobId_ = {};
    cancelButton_.setVisible(false);
    resized();
    statusLabel_.setText(TRANS("Cancelled."), juce::dontSendNotification);
    refreshReadiness();
}

void PlatformMastersPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(24);
    runButton_.setBounds(controls.removeFromLeft(180));
    if (cancelButton_.isVisible()) {
        controls.removeFromLeft(6);
        cancelButton_.setBounds(controls.removeFromLeft(80));
    }
    if (overrideButton_.isVisible()) {
        controls.removeFromLeft(6);
        overrideButton_.setBounds(controls.removeFromLeft(90));
    }
    b.removeFromTop(4);
    matchLoudnessToggle_.setBounds(b.removeFromTop(22).removeFromLeft(280));
    b.removeFromTop(2);
    statusLabel_.setBounds(b.removeFromTop(16));
    b.removeFromTop(2);
    resultsLabel_.setBounds(b);
}

void PlatformMastersPanel::offerOverride()
{
    if (rejected_.empty() || !onGuardrailOverride) return;
    if (rejected_.size() == 1) {
        onGuardrailOverride(rejected_.front());
        return;
    }
    juce::PopupMenu menu;
    menu.addSectionHeader(TRANS("Override which rejected output?"));
    for (size_t i = 0; i < rejected_.size(); ++i)
        menu.addItem((int)i + 1, juce::File(rejected_[i].quarantinePath).getFileName());
    juce::Component::SafePointer<PlatformMastersPanel> self(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&overrideButton_), [self](int result) {
        if (self == nullptr || result <= 0 || (size_t)result > self->rejected_.size() || !self->onGuardrailOverride) return;
        self->onGuardrailOverride(self->rejected_[(size_t)result - 1]);
    });
}

} // namespace khensu
