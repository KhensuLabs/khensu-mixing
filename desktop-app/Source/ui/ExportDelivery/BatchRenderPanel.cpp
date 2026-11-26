// desktop-app/Source/ui/ExportDelivery/BatchRenderPanel.cpp
#include "BatchRenderPanel.h"
#include <khensu/ipc/generated/ApiJobStatus.h>
#include "../shared/KhensuTheme.h"
#include "../shared/JobPoller.h"
#include "../shared/ServiceErrorHint.h"
#include "../shared/ButtonRole.h"
#include "MasterMacros.h"
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

    // ENH-4: the mode picker.
    modeBox_.addItem(TRANS("Same chain for every file"), kSameChainMode);
    modeBox_.addItem(TRANS("Each file on its own (analyse, fix, master)"), kPerFileMode);
    modeBox_.setSelectedId(kSameChainMode, juce::dontSendNotification);
    modeBox_.setTitle(TRANS("Batch mode"));
    modeBox_.setTooltip(TRANS("Same chain: your last applied chain on every file. Each file on its own: every file "
                              "is analysed and its own findings with a verified fix are fixed (each move kept only "
                              "when it helped), then mastered if you ask."));
    modeBox_.onChange = [this] { setPerFileMode(perFileMode()); };
    addAndMakeVisible(modeBox_);
    masterToggle_.setToggleState(true, juce::dontSendNotification);
    masterToggle_.setTooltip(TRANS("After fixing, master each file toward the session's genre (or neutral targets "
                                   "without a confirmed genre) in the chosen style."));
    addChildComponent(masterToggle_);
    for (int i = 0; i < 4; ++i)
        styleBox_.addItem(juce::translate(i == 0 ? NEEDS_TRANS("Warm") : i == 1 ? NEEDS_TRANS("Balanced")
                                                    : i == 2 ? NEEDS_TRANS("Open") : NEEDS_TRANS("Loud")),
                          i + 1);
    styleBox_.setSelectedId(2, juce::dontSendNotification);
    styleBox_.setTitle(TRANS("Mastering style"));
    styleBox_.setTooltip(TRANS("The mastering style for every file (Balanced: the genre's own targets)."));
    masterToggle_.onClick = [this] { styleBox_.setEnabled(masterToggle_.getToggleState()); };
    addChildComponent(styleBox_);
    addChildComponent(smartResults_);

    chooseButton_.onClick = [this] { chooseFolder(); };
    addAndMakeVisible(chooseButton_);

    runButton_.onClick = [this] { run(); };
    button_role::markPrimary(runButton_);
    runButton_.setEnabled(false);
    addAndMakeVisible(runButton_);

    cancelButton_.onClick = [this] { cancelRun(); };
    cancelButton_.setVisible(false);
    addChildComponent(cancelButton_);

    statusLabel_.setFont(KhensuTheme::labelFont(KhensuTheme::kFontCaption));
    addAndMakeVisible(statusLabel_);

    overrideButton_.setTooltip(TRANS("The guardrail rejected an output and quarantined it. Override it with an "
                                     "expert justification to promote the quarantined file."));
    overrideButton_.onClick = [this] { offerOverride(); };
    overrideButton_.setVisible(false);
    addChildComponent(overrideButton_);

    resultsLabel_.setFont(KhensuTheme::labelFont(KhensuTheme::kFontCaption));
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
    if (perFileMode()) {
        if (folder_ == juce::File{}) {
            statusLabel_.setText(TRANS("Pick a folder: each file is analysed, fixed and mastered on its own."),
                                 juce::dontSendNotification);
            runButton_.setEnabled(false);
            return;
        }
        statusLabel_.setText(TRANS("%f: %n audio file(s), each on its own terms.")
                                 .replace("%f", folder_.getFileName())
                                 .replace("%n", juce::String(audioFileCount_)),
                             juce::dontSendNotification);
        runButton_.setEnabled(audioFileCount_ > 0);
        return;
    }
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
    if (perFileMode()) return runPerFile();
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
    if (smartJobId_.isNotEmpty() || (startInFlight_ && perFileMode())) {
        if (pollStop_) pollStop_->store(true);
        if (startInFlight_) cancelRequestedWhileStarting_ = true;
        if (smartJobId_.isNotEmpty()) {
            const auto jobId = smartJobId_;
            controller_.runLongInBackground([jobId, url = bridgeUrl()] { ipc::HttpServiceBridge(url).cancelSmartBatch(jobId); });
        }
        smartJobId_ = {};
        currentJobId_ = {};
        chooseButton_.setEnabled(true);
        modeBox_.setEnabled(true);
        cancelButton_.setVisible(false);
        resized();
        statusLabel_.setText(TRANS("Cancelled - the file in progress stops at its next step."), juce::dontSendNotification);
        runButton_.setEnabled(audioFileCount_ > 0);
        return;
    }
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
    auto modeRow = b.removeFromTop(KhensuTheme::buttonHSmall);
    modeBox_.setBounds(modeRow.removeFromLeft(juce::jmin(300, modeRow.getWidth() / 2)));
    if (masterToggle_.isVisible()) {
        modeRow.removeFromLeft(8);
        masterToggle_.setBounds(modeRow.removeFromLeft(110));
        modeRow.removeFromLeft(4);
        styleBox_.setBounds(modeRow.removeFromLeft(juce::jmin(120, modeRow.getWidth())));
    }
    b.removeFromTop(4);
    auto controls = b.removeFromTop(KhensuTheme::buttonHSmall);
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
    resultsLabel_.setBounds(perFileMode() ? juce::Rectangle<int>() : b);
    smartResults_.setBounds(perFileMode() ? b : juce::Rectangle<int>());
}

// ---- ENH-4: each file on its own -------------------------------------------

void BatchRenderPanel::setPerFileMode(bool perFile)
{
    if (modeBox_.getSelectedId() != (perFile ? kPerFileMode : kSameChainMode))
        modeBox_.setSelectedId(perFile ? kPerFileMode : kSameChainMode, juce::dontSendNotification);
    masterToggle_.setVisible(perFile);
    styleBox_.setVisible(perFile);
    smartResults_.setVisible(perFile);
    resultsLabel_.setVisible(!perFile);
    refreshReadiness();
    resized();
}

DeliveryResult BatchRenderPanel::smartResultsTable(const ipc::HttpServiceBridge::SmartBatchStatus& s)
{
    auto lufs = [](const std::optional<double>& v) { return v ? juce::String(*v, 1) : juce::String("-"); };
    DeliveryResult table;
    for (const auto& r : s.results) {
        const juce::String name = juce::File(r.filePath).getFileName();
        if (r.status == "failed") {
            table.add(DeliveryResult::Status::Fail, name, TRANS("failed"), r.error);
            continue;
        }
        const auto status = r.status == "completed"
                                ? (r.findingsAfter > 0 ? DeliveryResult::Status::Warn : DeliveryResult::Status::Pass)
                                : DeliveryResult::Status::Info;
        juce::String value = lufs(r.beforeLufs) + " -> " + lufs(r.afterLufs) + " LUFS, TP " + lufs(r.beforeTruePeak) +
                             " -> " + lufs(r.afterTruePeak);
        if (r.status == "unchanged") value = TRANS("unchanged") + " (" + lufs(r.beforeLufs) + " LUFS)";
        table.add(status, name, value,
                  TRANS("cleared %c of %b").replace("%c", juce::String(r.findingsCleared))
                          .replace("%b", juce::String(r.findingsBefore)) +
                      (r.mastered ? ", " + TRANS("mastered") : juce::String()));
    }
    for (const auto& r : s.results)
        if (r.findingsAfter > 0 && r.remaining.size() > 0)
            table.notes.add(juce::File(r.filePath).getFileName() + ": " + TRANS("still open") + " - " +
                            r.remaining.joinIntoString(", "));
    if (s.outputDir.isNotEmpty()) table.notes.add(TRANS("Written to") + " " + s.outputDir);
    return table;
}

void BatchRenderPanel::runPerFile()
{
    ipc::HttpServiceBridge::SmartBatchRequest req;
    for (const auto& f : folder_.findChildFiles(juce::File::findFiles, false, kAudioWildcards))
        req.filePaths.push_back(f.getFullPathName());
    if (req.filePaths.empty()) return refreshReadiness();
    std::sort(req.filePaths.begin(), req.filePaths.end());
    req.fix = true;
    req.master = masterToggle_.getToggleState();
    req.style = master_macros::styleId(static_cast<master_macros::Style>(juce::jlimit(0, 3, styleBox_.getSelectedId() - 1)));
    // The session's genre only when its targets are confirmed; else neutral.
    if (const auto* s = controller_.getActiveSession();
        req.master && s != nullptr && controller_.analysis().genreRelativeActionAllowed()) {
        req.genreId = juce::String(s->target.primary_genre);
        req.genreActionAllowed = req.genreId.isNotEmpty();
    }

    runButton_.setEnabled(false);
    chooseButton_.setEnabled(false);
    modeBox_.setEnabled(false);
    cancelButton_.setVisible(true);
    resized();
    statusLabel_.setText(TRANS("Starting: each file is analysed, fixed%m on its own...")
                             .replace("%m", req.master ? TRANS(" and mastered") : juce::String()),
                         juce::dontSendNotification);
    smartResults_.setResult({});
    startInFlight_ = true;
    cancelRequestedWhileStarting_ = false;
    juce::Component::SafePointer<BatchRenderPanel> self(this);
    controller_.runLongInBackground([self, req, url = bridgeUrl()] {
        auto started = ipc::HttpServiceBridge(url).startSmartBatch(req);
        juce::MessageManager::callAsync([self, started = std::move(started)] {
            if (self == nullptr) return;
            self->startInFlight_ = false;
            // Not session-scoped: the batch belongs to its folder and job id.
            if (self->cancelRequestedWhileStarting_) {
                self->cancelRequestedWhileStarting_ = false;
                if (started.is_ok()) {
                    const auto jobId = started.value().jobId;
                    self->controller_.runInBackground(
                        [jobId, url = self->bridgeUrl()] { ipc::HttpServiceBridge(url).cancelSmartBatch(jobId); });
                }
                return;
            }
            if (started.is_err()) {
                self->chooseButton_.setEnabled(true);
                self->modeBox_.setEnabled(true);
                self->cancelButton_.setVisible(false);
                self->resized();
                self->statusLabel_.setText(TRANS("Error: ") + serviceErrorLine(started.error()),
                                           juce::dontSendNotification);
                self->runButton_.setEnabled(self->audioFileCount_ > 0);
                return;
            }
            self->smartJobId_ = started.value().jobId;
            self->currentJobId_ = self->smartJobId_;
            self->pollSmartJob(self->smartJobId_);
        });
    });
}

void BatchRenderPanel::pollSmartJob(const juce::String& jobId)
{
    if (pollStop_) pollStop_->store(true);
    auto stop = std::make_shared<std::atomic<bool>>(false);
    pollStop_ = stop;
    juce::Component::SafePointer<BatchRenderPanel> self(this);
    controller_.runLongInBackground([self, jobId, stop, url = bridgeUrl()]() {
        ipc::HttpServiceBridge bridge(url);
        using Poller = JobPoller<ipc::HttpServiceBridge::SmartBatchStatus>;
        Poller::Hooks hooks;
        hooks.poll = [&bridge](const std::string& id) { return bridge.getSmartBatchStatus(juce::String(id)); };
        hooks.cancel = [&bridge](const std::string& id) { bridge.cancelSmartBatch(juce::String(id)); };
        hooks.isTerminal = [](const auto& st) { return !khensu::api::isActiveStatus(st.status); };
        hooks.onProgress = [self, jobId](const auto& st) {
            juce::String t = TRANS("File %i of %n").replace("%i", juce::String(juce::jmin(st.done + 1, st.total)))
                                 .replace("%n", juce::String(st.total));
            if (st.current.isNotEmpty())
                t << " (" << st.current << ")" << (st.stage.isNotEmpty() ? ": " + st.stage : juce::String());
            auto table = smartResultsTable(st);
            juce::MessageManager::callAsync([self, jobId, t, table]() {
                if (self == nullptr || jobId != self->smartJobId_) return;
                self->statusLabel_.setText(t, juce::dontSendNotification);
                self->smartResults_.setResult(table); // rows appear as each file finishes
            });
        };
        hooks.shouldStop = [stop] { return stop->load(); };
        Poller::Options opts;
        opts.pollIntervalMs = kPollIntervalMs;
        opts.maxConsecutiveErrors = kMaxConsecutivePollErrors;
        opts.deadlineSeconds = 8 * 60 * 60; // analysing + fixing + mastering a big folder takes a while
        const auto out = Poller::run(hooks, opts, jobId.toStdString());
        juce::MessageManager::callAsync([self, jobId, out]() {
            if (self == nullptr || jobId != self->smartJobId_) return; // cancelled meanwhile
            self->smartJobId_ = {};
            self->currentJobId_ = {};
            self->chooseButton_.setEnabled(true);
            self->modeBox_.setEnabled(true);
            self->cancelButton_.setVisible(false);
            self->resized();
            if (!out.finished() || !out.status.has_value()) {
                self->statusLabel_.setText(out.describe(), juce::dontSendNotification);
                self->refreshReadiness();
                return;
            }
            self->finishSmartRun(*out.status);
        });
    });
}

void BatchRenderPanel::finishSmartRun(const ipc::HttpServiceBridge::SmartBatchStatus& s)
{
    smartResults_.setResult(smartResultsTable(s));
    runButton_.setEnabled(audioFileCount_ > 0);
    if (s.status == khensu::api::job_status::cancelled) {
        statusLabel_.setText(TRANS("Cancelled."), juce::dontSendNotification);
        return;
    }
    if (s.status != khensu::api::job_status::completed) {
        statusLabel_.setText(TRANS("Batch failed: ") + (s.error.isNotEmpty() ? s.error : s.status),
                             juce::dontSendNotification);
        return;
    }
    int changed = 0;
    for (const auto& r : s.results)
        if (r.status == "completed") ++changed;
    statusLabel_.setText(TRANS("%c of %n file(s) improved, %f failed.")
                             .replace("%c", juce::String(changed))
                             .replace("%n", juce::String(s.total))
                             .replace("%f", juce::String(s.failed)),
                         juce::dontSendNotification);
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
