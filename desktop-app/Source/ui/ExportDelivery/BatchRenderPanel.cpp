// desktop-app/Source/ui/ExportDelivery/BatchRenderPanel.cpp
#include "BatchRenderPanel.h"
#include <khensu/ipc/generated/ApiJobStatus.h>
#include "../shared/KhensuTheme.h"
#include "../shared/JobPoller.h"
#include "../shared/ServiceErrorHint.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <thread>
#include <khensu/ipc/generated/ApiSchema.h>

namespace af = khensu::api::fields;

namespace khensu {

namespace {
constexpr int kPollIntervalMs = 1000;
// If the service goes away mid-batch, don't poll forever with the UI stuck
// on "Rendering..." - JobPoller bounds the run of consecutive poll failures
// (frontend-review-2026-07-17.md P1, UIX-07).
constexpr int kMaxConsecutivePollErrors = 6;
// Per-file safety states beyond the job states (WIRE-10).
namespace batch_file_status {
inline constexpr const char* blocked = "blocked";   // the pre-render validator refused the chain
inline constexpr const char* rejected = "rejected"; // the post-render guardrail quarantined the output
} // namespace batch_file_status
// Mirrors the service's _AUDIO_BATCH_EXTS so the pre-run count matches what
// the server will actually pick up.
const char* kAudioWildcards = "*.wav;*.flac;*.aif;*.aiff;*.ogg;*.mp3";
} // namespace

// Every cached palette colour on this component, in one place (UI-01).
void BatchRenderPanel::applyThemeColours()
{
    chooseButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    chooseButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    runButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal);
    runButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::bgVoid);
    cancelButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentRed.withAlpha(0.35f));
    overrideButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentAmber.withAlpha(0.25f));
    overrideButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentAmber);
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    resultsLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

BatchRenderPanel::BatchRenderPanel(PanelCardController& c, const juce::String& baseUrl)
    : PanelCard(c, "Batch Render"), baseUrl_(baseUrl)
{
    applyThemeColours();
    setExplainer(TRANS("Batch Render"), TRANS("Applies the chain you most recently applied this session (chat, Fix This, "
                                 "EQ, or a recalled preset) to every audio file in a folder - the "
                                 "same render, N files. Outputs land in a khensu_batch subfolder next to the "
                                 "sources. Files are processed independently: a corrupt or unreadable take "
                                 "is reported per-file and the rest of the batch still renders."));

    chooseButton_.onClick = [this] { chooseFolder(); };
    addAndMakeVisible(chooseButton_);

    runButton_.onClick = [this] { run(); };
    runButton_.setEnabled(false);
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

#if KHENSU_TESTS
void BatchRenderPanel::setInputFolderForTesting(const juce::File& folder)
{
    folder_ = folder;
    audioFileCount_ = folder_.findChildFiles(juce::File::findFiles, false, kAudioWildcards).size();
    refreshReadiness();
}
#endif

BatchRenderPanel::~BatchRenderPanel()
{
    if (pollStop_) pollStop_->store(true); // the poller cancels the job server-side
}

std::string BatchRenderPanel::bridgeUrl() const
{
    return baseUrl_.isNotEmpty() ? baseUrl_.toStdString() : std::string(ipc::HttpServiceBridge::kDefaultBaseUrl);
}

void BatchRenderPanel::visibilityChanged()
{
    // The last-applied chain may have changed while another screen was up.
    if (isVisible() && currentJobId_.isEmpty() && !startInFlight_) refreshReadiness();
}

void BatchRenderPanel::refreshReadiness()
{
    const auto& chain = controller_.renderJobs().lastChain();

    if (folder_ == juce::File{}) {
        statusLabel_.setText(chain.empty()
                                 ? TRANS("Apply a chain first (chat, Fix This, EQ), then pick a folder.")
                                 : TRANS("Pick a folder of audio files to process."),
                             juce::dontSendNotification);
        runButton_.setEnabled(false);
        return;
    }

    juce::StringArray names;
    for (const auto& fx : chain)
        names.add(fx.effect);
    if (chain.empty()) {
        statusLabel_.setText(TRANS("%f: %n audio file(s) - apply a chain first (chat, Fix This, EQ).")
                                 .replace("%f", folder_.getFileName())
                                 .replace("%n", juce::String(audioFileCount_)),
                             juce::dontSendNotification);
        runButton_.setEnabled(false);
        return;
    }
    statusLabel_.setText(TRANS("%f: %n audio file(s)  -  chain: %c")
                             .replace("%f", folder_.getFileName())
                             .replace("%n", juce::String(audioFileCount_))
                             .replace("%c", names.joinIntoString(" -> ")),
                         juce::dontSendNotification);
    runButton_.setEnabled(audioFileCount_ > 0);
}

void BatchRenderPanel::chooseFolder()
{
    if (!currentJobId_.isEmpty() || startInFlight_) return;
    chooser_ = std::make_unique<juce::FileChooser>(TRANS("Select a folder of audio files"),
                                                   juce::File::getSpecialLocation(juce::File::userMusicDirectory));
    juce::Component::SafePointer<BatchRenderPanel> self(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [self](const juce::FileChooser& fc) {
                              if (self == nullptr) return;
                              const auto result = fc.getResult();
                              if (!result.isDirectory()) return;
                              self->folder_ = result;
                              // Non-recursive, matching the server's folder scan.
                              self->audioFileCount_ =
                                  self->folder_.findChildFiles(juce::File::findFiles, false, kAudioWildcards).size();
                              self->resultsLabel_.setText({}, juce::dontSendNotification);
                              self->refreshReadiness();
                          });
}

void BatchRenderPanel::run()
{
    if (!currentJobId_.isEmpty() || startInFlight_ || folder_ == juce::File{}) return;
    const auto chain = controller_.renderJobs().lastChain();
    // Non-empty only when that chain was model-proposed (Copilot chat).
    const juce::String decisionHash = controller_.renderJobs().lastChainDecisionHash();
    if (chain.empty()) {
        refreshReadiness();
        return;
    }

    runButton_.setEnabled(false);
    chooseButton_.setEnabled(false);
    cancelButton_.setVisible(true);
    resized();
    statusLabel_.setText(TRANS("Starting batch..."), juce::dontSendNotification);
    rejected_.clear();
    overrideButton_.setVisible(false);
    resultsLabel_.setText({}, juce::dontSendNotification);

    startInFlight_ = true;
    cancelRequestedWhileStarting_ = false;

    const juce::String inputDir = folder_.getFullPathName();
    juce::Component::SafePointer<BatchRenderPanel> self(this);
    controller_.runLongInBackground([self, inputDir, chain, decisionHash, url = bridgeUrl()]() {
        ipc::HttpServiceBridge bridge(url);
        auto result = bridge.startApplyChainBatchJob(inputDir, {}, chain, "_processed", {}, decisionHash);
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
                        bridge.cancelApplyChainBatchJob(jobId);
                    });
                }
                return;
            }

            if (result.is_err()) {
                self->chooseButton_.setEnabled(true);
                self->cancelButton_.setVisible(false);
                self->resized();
                self->statusLabel_.setText(TRANS("Error: ") + serviceErrorLine(result.error()),
                                           juce::dontSendNotification);
                self->refreshReadiness();
                return;
            }
            self->currentJobId_ = result.value();
            self->pollJob(self->currentJobId_);
        });
    });
}

void BatchRenderPanel::pollJob(const juce::String& jobId)
{
    if (pollStop_) pollStop_->store(true);
    auto stop = std::make_shared<std::atomic<bool>>(false);
    pollStop_ = stop;
    juce::Component::SafePointer<BatchRenderPanel> self(this);
    // Not session-guarded: the render belongs to the job id, not the
    // session; abandoning it would freeze its progress and controls.
    controller_.runLongInBackground([self, jobId, stop, url = bridgeUrl()]() {
        ipc::HttpServiceBridge bridge(url);
        using Poller = JobPoller<ipc::HttpServiceBridge::BatchRenderJobStatus>;
        Poller::Hooks hooks;
        hooks.poll = [&bridge](const std::string& id) { return bridge.getApplyChainBatchJobStatus(juce::String(id)); };
        hooks.cancel = [&bridge](const std::string& id) { bridge.cancelApplyChainBatchJob(juce::String(id)); };
        hooks.isTerminal = [](const auto& st) { return !khensu::api::isActiveStatus(st.status); };
        hooks.onProgress = [self, jobId](const auto& st) {
            const juce::String t = TRANS("Rendering... %d / %t").replace("%d", juce::String(st.done))
                                       .replace("%t", juce::String(st.total))
                                   + (st.failed > 0 ? "  (" + TRANS("%n failed").replace("%n", juce::String(st.failed)) + ")"
                                                    : juce::String());
            juce::MessageManager::callAsync([self, jobId, t]() {
                if (self != nullptr && jobId == self->currentJobId_)
                    self->statusLabel_.setText(t, juce::dontSendNotification);
            });
        };
        hooks.shouldStop = [stop] { return stop->load(); };
        Poller::Options opts;
        opts.pollIntervalMs = kPollIntervalMs;
        opts.maxConsecutiveErrors = kMaxConsecutivePollErrors;
        opts.deadlineSeconds = 4 * 60 * 60; // a big folder is a long batch
        const auto out = Poller::run(hooks, opts, jobId.toStdString());

        juce::MessageManager::callAsync([self, jobId, out]() {
            if (self == nullptr || jobId != self->currentJobId_) return; // cancelled meanwhile
            self->currentJobId_ = {};
            self->chooseButton_.setEnabled(true);
            self->cancelButton_.setVisible(false);
            self->resized();
            if (!out.finished() || !out.status.has_value()) {
                if (out.outcome == JobPollOutcome::ServiceLost) {
                    const auto hint = classifyServiceError(out.lastError);
                    self->statusLabel_.setText(hint.message + "  " + hint.remedy, juce::dontSendNotification);
                } else {
                    self->statusLabel_.setText(out.describe(), juce::dontSendNotification);
                }
                self->refreshReadiness();
                return;
            }
            self->finishRun(*out.status);
        });
    });
}

void BatchRenderPanel::finishRun(const ipc::HttpServiceBridge::BatchRenderJobStatus& s)
{
    if (s.status == khensu::api::job_status::cancelled) {
        statusLabel_.setText(TRANS("Cancelled."), juce::dontSendNotification);
        refreshReadiness();
        return;
    }
    if (s.status != khensu::api::job_status::completed) {
        statusLabel_.setText(TRANS("Batch failed."), juce::dontSendNotification);
        refreshReadiness();
        return;
    }

    juce::String outputDir;
    juce::StringArray failures;
    int notDelivered = 0; // failed + blocked (pre-render validator) + rejected (guardrail)
    rejected_.clear();
    if (auto* arr = s.results.getArray()) {
        for (const auto& rv : *arr) {
            const juce::String fileStatus = rv[af::BatchFileResult::status].toString();
            const juce::String fileName = juce::File(rv[af::BatchFileResult::file_path].toString()).getFileName();
            // Per-file states: the generated job-state strings plus the two
            // safety outcomes the batch now shares with single renders (WIRE-10).
            const bool failed = fileStatus == khensu::api::job_status::failed;
            const bool blocked = fileStatus == batch_file_status::blocked;
            const bool rejected = fileStatus == batch_file_status::rejected;
            if (!failed && !blocked && !rejected) {
                if (outputDir.isEmpty() && rv[af::BatchFileResult::output_path].toString().isNotEmpty())
                    outputDir = juce::File(rv[af::BatchFileResult::output_path].toString()).getParentDirectory().getFullPathName();
                continue;
            }
            ++notDelivered;
            if (rejected) {
                GuardrailRejection r;
                r.guardrailStatus = "rejected";
                r.serverRenderState = rv[af::BatchFileResult::render_state].toString();
                r.sourcePath = rv[af::BatchFileResult::file_path].toString();
                r.quarantinePath = rv[af::BatchFileResult::quarantine_path].toString();
                r.outputPath = rv[af::BatchFileResult::output_path].toString();
                r.measurements = rv[af::BatchFileResult::guardrail];
                if (r.quarantinePath.isNotEmpty()) rejected_.push_back(r);
            }
            if (failures.size() < 3) {
                const juce::String why = rejected  ? TRANS("rejected by the guardrail (quarantined)")
                                         : blocked ? TRANS("blocked by the pre-render check")
                                                   : TRANS("failed");
                const juce::String err = rv[af::BatchFileResult::error].toString();
                failures.add(fileName + ": " + why + (err.isNotEmpty() ? " - " + err : juce::String()));
            }
        }
    }
    notDelivered = juce::jmax(notDelivered, s.failed);
    if (notDelivered > 3) failures.add(TRANS("+%n more").replace("%n", juce::String(notDelivered - 3)));
    const int succeeded = juce::jmax(0, s.done - notDelivered);

    statusLabel_.setText(TRANS("%s / %t rendered -> %d")
                             .replace("%s", juce::String(succeeded))
                             .replace("%t", juce::String(s.total))
                             .replace("%d", outputDir),
                         juce::dontSendNotification);
    resultsLabel_.setColour(juce::Label::textColourId, notDelivered > 0 ? KhensuTheme::accentAmber : KhensuTheme::accentTeal);
    resultsLabel_.setText(failures.isEmpty() ? TRANS("All files rendered successfully.") : failures.joinIntoString("\n"),
                          juce::dontSendNotification);
    overrideButton_.setVisible(!rejected_.empty() && onGuardrailOverride != nullptr);
    resized();
    runButton_.setEnabled(true);
}

void BatchRenderPanel::cancelRun()
{
    if (currentJobId_.isEmpty() && !startInFlight_) return;
    if (pollStop_) pollStop_->store(true); // the poller cancels server-side

    if (startInFlight_) {
        // /start hasn't returned yet - remember the cancel so run()'s
        // completion lambda cancels server-side once the jobId arrives,
        // rather than silently letting the render start unattended
        // (frontend-review-2026-07-17.md P1).
        cancelRequestedWhileStarting_ = true;
    } else {
        const juce::String jobId = currentJobId_;
        controller_.runLongInBackground([jobId, url = bridgeUrl()]() {
            ipc::HttpServiceBridge bridge(url);
            bridge.cancelApplyChainBatchJob(jobId); // fire-and-forget
        });
    }

    currentJobId_ = {};
    chooseButton_.setEnabled(true);
    cancelButton_.setVisible(false);
    resized();
    statusLabel_.setText(TRANS("Cancelled."), juce::dontSendNotification);
    refreshReadiness();
}

void BatchRenderPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(24);
    chooseButton_.setBounds(controls.removeFromLeft(120));
    controls.removeFromLeft(6);
    runButton_.setBounds(controls.removeFromLeft(120));
    if (cancelButton_.isVisible()) {
        controls.removeFromLeft(6);
        cancelButton_.setBounds(controls.removeFromLeft(80));
    }
    if (overrideButton_.isVisible()) {
        controls.removeFromLeft(6);
        overrideButton_.setBounds(controls.removeFromLeft(90));
    }
    b.removeFromTop(4);
    statusLabel_.setBounds(b.removeFromTop(16));
    b.removeFromTop(2);
    resultsLabel_.setBounds(b);
}

void BatchRenderPanel::offerOverride()
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
    juce::Component::SafePointer<BatchRenderPanel> self(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&overrideButton_), [self](int result) {
        if (self == nullptr || result <= 0 || (size_t)result > self->rejected_.size() || !self->onGuardrailOverride) return;
        self->onGuardrailOverride(self->rejected_[(size_t)result - 1]);
    });
}

} // namespace khensu
