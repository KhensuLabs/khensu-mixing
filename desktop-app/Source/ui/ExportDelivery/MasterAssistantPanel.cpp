// desktop-app/Source/ui/ExportDelivery/MasterAssistantPanel.cpp
#include "MasterAssistantPanel.h"
#include "../../IAppController.h"
#include "../../analysis/FixVerification.h"
#include "../shared/ButtonRole.h"
#include "../shared/GuardrailText.h"
#include "../shared/JobPoller.h"
#include "../shared/RenderPaths.h"
#include "../shared/ServiceErrorHint.h"
#include "../shared/SessionAudioPaths.h"
#include <khensu/ipc/generated/ApiJobStatus.h>
#include <khensu/ipc/generated/ApiSchema.h>
#include <khensu/session/SessionManager.h>
#include <khensu/targets/GenreDatabase.h>
#include <algorithm>

namespace khensu {

namespace {
enum MacroIndex { kLoudness = 0, kTone, kWidth, kPunch, kCeiling };
constexpr int kAgentIterations = 5;
constexpr int kFirstSetId = 100; // setsBox_: item 1 is the placeholder, sets from here
constexpr const char* kStyleNames[] = {NEEDS_TRANS("Warm"), NEEDS_TRANS("Balanced"), NEEDS_TRANS("Open"),
                                       NEEDS_TRANS("Loud")};
} // namespace

void MasterAssistantPanel::applyThemeColours()
{
    for (auto* l : {&targetLabel_, &statusLabel_, &verifyLabel_, &strengthLabel_, &summaryLabel_})
        l->setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    for (auto* l : {&styleHeading_, &setsHeading_, &summaryHeading_})
        l->setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    styleCaption_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    for (auto& m : macros_) {
        m.label.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
        m.caption.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    }
}

MasterAssistantPanel::MasterAssistantPanel(IAppController& c) : PanelCard(c, TRANS("Master Assistant"))
{
    applyThemeColours();
    setExplainer(TRANS("Master Assistant"),
                 TRANS("Mastering in four steps, from this session's target. Master: the mastering agent works "
                       "on the current version toward the session's genre, and its master goes into the "
                       "loudness-matched A/B. Shape: five macros - loudness, tone, width, punch and the "
                       "true-peak ceiling - re-render the agent's chain. Accept: the master becomes the "
                       "current version and is checked against the original's findings. Export: the "
                       "platform masters."));

    targetLabel_.setFont(KhensuTheme::labelFont(type::caption));
    addAndMakeVisible(targetLabel_);

    // ENH-5: style and strength, saved sets (the right column, UXR-2).
    auto heading = [this](juce::Label& l, const juce::String& text) {
        l.setText(text, juce::dontSendNotification);
        l.setFont(KhensuTheme::sectionHeaderFont());
        addAndMakeVisible(l);
    };
    heading(styleHeading_, TRANS("Style"));
    heading(setsHeading_, TRANS("Saved macro sets"));
    heading(summaryHeading_, TRANS("Before / after"));
    for (int i = 0; i < 4; ++i)
        styleBox_.addItem(juce::translate(kStyleNames[i]), i + 1);
    styleBox_.setSelectedId(static_cast<int>(master_macros::Style::Balanced) + 1, juce::dontSendNotification);
    styleBox_.setTitle(TRANS("Mastering style"));
    styleBox_.setTooltip(TRANS("Where the agent aims: Balanced is the genre's own targets; Warm, Open and Loud move "
                               "them (see the line below)."));
    styleBox_.onChange = [this] { updateStyleCaption(); };
    addAndMakeVisible(styleBox_);
    strengthLabel_.setText(TRANS("Strength"), juce::dontSendNotification);
    strengthLabel_.setFont(KhensuTheme::labelFont(type::caption));
    addAndMakeVisible(strengthLabel_);
    strengthSlider_.setRange(0.0, 100.0, 5.0);
    strengthSlider_.setValue(50.0, juce::dontSendNotification);
    strengthSlider_.setDoubleClickReturnValue(true, 50.0);
    strengthSlider_.setTextValueSuffix("%");
    strengthSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    strengthSlider_.setTitle(TRANS("Style strength"));
    strengthSlider_.setTooltip(TRANS("How far the style moves the targets: 0% is the genre's own, 100% the full move."));
    strengthSlider_.onValueChange = [this] { updateStyleCaption(); };
    addAndMakeVisible(strengthSlider_);
    styleCaption_.setFont(KhensuTheme::labelFont(KhensuTheme::kFontCaption));
    styleCaption_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(styleCaption_);
    updateStyleCaption();

    setsBox_.setTitle(TRANS("Saved macro sets"));
    setsBox_.setTooltip(TRANS("Load a saved style, strength and macro setting."));
    setsBox_.onChange = [this] {
        const int index = setsBox_.getSelectedId() - kFirstSetId;
        if (index >= 0 && index < static_cast<int>(sets_.size())) applySet(sets_[static_cast<size_t>(index)]);
        deleteSetButton_.setEnabled(index >= 0);
    };
    addAndMakeVisible(setsBox_);
    saveSetButton_.setTooltip(TRANS("Save the style, strength and the five macros under a name."));
    saveSetButton_.onClick = [this] { promptSaveSet(); };
    addAndMakeVisible(saveSetButton_);
    deleteSetButton_.setTooltip(TRANS("Delete the selected set."));
    deleteSetButton_.onClick = [this] { deleteSelectedSet(); };
    addAndMakeVisible(deleteSetButton_);
    applyProjectButton_.setTooltip(TRANS("Fix and master every track in this track's project with this style, "
                                         "strength, loudness and ceiling (Export > Album shows the progress). "
                                         "Tone, width and punch shape one master at a time."));
    applyProjectButton_.onClick = [this] {
        if (onApplyToProject) onApplyToProject(currentSet(setsBox_.getText()));
    };
    addAndMakeVisible(applyProjectButton_);
    summaryLabel_.setFont(KhensuTheme::labelFont(type::caption));
    summaryLabel_.setJustificationType(juce::Justification::topLeft);
    summaryLabel_.setText(TRANS("Master the track to see its loudness, dynamics and brightness before and after, "
                                "against the targets the style set."),
                          juce::dontSendNotification);
    addAndMakeVisible(summaryLabel_);
    rebuildSetsBox();

    masterButton_.setTooltip(TRANS("Run the mastering agent on the current version, toward the session's genre."));
    masterButton_.onClick = [this] { master(); };
    button_role::markPrimary(masterButton_);
    addAndMakeVisible(masterButton_);
    cancelButton_.onClick = [this] { cancel(); };
    addChildComponent(cancelButton_);

    statusLabel_.setFont(KhensuTheme::labelFont(type::caption));
    statusLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(statusLabel_);

    struct Spec {
        const char* name;
        const char* tip;
        double lo, hi, step, def;
        const char* suffix;
    };
    const Spec specs[] = {
        {NEEDS_TRANS("Loudness"), NEEDS_TRANS("Gain into the limiter, relative to the agent's master."), -6, 6, 0.5, 0,
         " dB"},
        {NEEDS_TRANS("Tone"), NEEDS_TRANS("How much of the chain's EQ moves to keep (100% = the agent's)."), 0, 150, 5,
         100, "%"},
        {NEEDS_TRANS("Width"), NEEDS_TRANS("Stereo width relative to the master."), -50, 50, 5, 0, "%"},
        {NEEDS_TRANS("Punch"), NEEDS_TRANS("How hard compression and transient shaping work (100% = the agent's)."), 0,
         150, 5, 100, "%"},
        {NEEDS_TRANS("Ceiling"), NEEDS_TRANS("The limiter's true-peak ceiling."), -3, -0.1, 0.1, -1, " dBTP"},
    };
    for (size_t i = 0; i < macros_.size(); ++i) {
        auto& m = macros_[i];
        const auto& s = specs[i];
        m.label.setText(juce::translate(s.name), juce::dontSendNotification);
        m.label.setFont(KhensuTheme::labelFont(type::caption));
        addAndMakeVisible(m.label);
        m.slider.setRange(s.lo, s.hi, s.step);
        m.slider.setValue(s.def, juce::dontSendNotification);
        m.slider.setDoubleClickReturnValue(true, s.def);
        m.slider.setTextValueSuffix(s.suffix);
        m.slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 78, 20);
        m.slider.setTitle(juce::translate(s.name));
        m.slider.setTooltip(juce::translate(s.tip));
        addAndMakeVisible(m.slider);
        m.caption.setText(macroCaption(static_cast<int>(i)), juce::dontSendNotification);
        m.caption.setFont(KhensuTheme::labelFont(KhensuTheme::kFontCaption));
        m.caption.setMinimumHorizontalScale(1.0f);
        addAndMakeVisible(m.caption);
    }
    shapeButton_.setTooltip(TRANS("Re-render the agent's chain with these macros as a new A/B candidate."));
    shapeButton_.onClick = [this] { shape(); };
    addAndMakeVisible(shapeButton_);
    resetMacrosButton_.setTooltip(TRANS("Back to the agent's own settings."));
    resetMacrosButton_.onClick = [this] {
        for (auto& m : macros_)
            m.slider.setValue(m.slider.getDoubleClickReturnValue(), juce::dontSendNotification);
    };
    addAndMakeVisible(resetMacrosButton_);

    acceptButton_.setTooltip(TRANS("Make the candidate in the A/B the current version; it is then checked "
                                   "against the original's findings."));
    acceptButton_.onClick = [this] { accept(); };
    addAndMakeVisible(acceptButton_);

    verifyLabel_.setFont(KhensuTheme::labelFont(type::caption));
    verifyLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(verifyLabel_);

    exportButton_.setTooltip(TRANS("Render the accepted master for each streaming platform (Platform Masters)."));
    exportButton_.onClick = [this] {
        if (onExportPlatforms) onExportPlatforms();
    };
    addAndMakeVisible(exportButton_);

    verificationListener_ = listen(controller_.analysis(), this, &IAnalysisHub::addVerificationListener,
                                   &IAnalysisHub::removeVerificationListener, [this] { showVerification(); });
    // A newly opened session shows up here first (resetForSession runs before it is adopted).
    analysisListener_ =
        listen(controller_.analysis(), this, &IAnalysisHub::addAnalysisListener, &IAnalysisHub::removeAnalysisListener,
               [this](const AnalysisResult&) { refreshTarget(); });
    setStage(0);
    refreshTarget();
    loadSets();
}

MasterAssistantPanel::~MasterAssistantPanel()
{
    if (pollStop_) pollStop_->store(true);
}

master_macros::Macros MasterAssistantPanel::macros() const
{
    master_macros::Macros m;
    m.loudnessDb = static_cast<float>(macros_[kLoudness].slider.getValue());
    m.tone = static_cast<float>(macros_[kTone].slider.getValue() / 100.0);
    m.widthPct = static_cast<float>(macros_[kWidth].slider.getValue());
    m.punch = static_cast<float>(macros_[kPunch].slider.getValue() / 100.0);
    m.ceilingDbtp = static_cast<float>(macros_[kCeiling].slider.getValue());
    return m;
}

void MasterAssistantPanel::visibilityChanged()
{
    if (isShowing()) refreshTarget();
}

void MasterAssistantPanel::refreshTarget()
{
    const auto* session = controller_.getActiveSession();
    if (session == nullptr) {
        targetLabel_.setText(TRANS("Open a track to master it."), juce::dontSendNotification);
        masterButton_.setEnabled(false);
        return;
    }
    juce::String genre = TRANS("no genre confirmed");
    if (auto g = controller_.genreDatabase().findById(session->target.primary_genre)) genre = juce::String(g->name);
    const juce::String intent = session->target.playback_intent.empty() ? juce::String(TRANS("streaming"))
                                                                        : juce::String(session->target.playback_intent);
    const juce::String reference = controller_.analysis().getReferenceTrack();
    targetLabel_.setText(TRANS("Target: %g, for %i playback").replace("%g", genre).replace("%i", intent) +
                             (reference.isNotEmpty()
                                  ? "  |  " + TRANS("reference: ") + juce::File(reference).getFileNameWithoutExtension()
                                  : juce::String()),
                         juce::dontSendNotification);
    masterButton_.setEnabled(stage_ != 1 && controller_.hasAnalysis());
}

void MasterAssistantPanel::setStage(int stage)
{
    stage_ = stage;
    const bool shaped = stage >= 2 && !baseChain_.empty();
    for (auto& m : macros_)
        m.slider.setEnabled(shaped);
    // Tone and Punch scale stages the agent's chain may not have (a master
    // that only needed gain into the limiter has neither): off when absent.
    auto has = [this](auto pred) { return std::any_of(baseChain_.begin(), baseChain_.end(), pred); };
    const bool hasEq = has([](const master_macros::Effect& e) { return master_macros::isEq(e.effect); });
    const bool hasDynamics = has([](const master_macros::Effect& e) {
        return e.effect == "Compressor" || e.effect == "GlueCompressor" || e.effect == "TransientShaper";
    });
    macros_[kTone].slider.setEnabled(shaped && hasEq);
    macros_[kPunch].slider.setEnabled(shaped && hasDynamics);
    if (shaped && !hasEq) macros_[kTone].slider.setTooltip(TRANS("This master has no EQ stage to scale."));
    if (shaped && !hasDynamics)
        macros_[kPunch].slider.setTooltip(TRANS("This master has no compression or transient stage to scale."));
    shapeButton_.setEnabled(shaped);
    resetMacrosButton_.setEnabled(shaped);
    acceptButton_.setEnabled(stage == 2 && candidatePath_.isNotEmpty());
    exportButton_.setEnabled(stage == 3);
    masterButton_.setEnabled(stage != 1 && controller_.getActiveSession() != nullptr && controller_.hasAnalysis());
    cancelButton_.setVisible(stage == 1);
    // The style is read when Master it is pressed: fixed while a run is on.
    styleBox_.setEnabled(stage != 1);
    strengthSlider_.setEnabled(stage != 1);
    resized();
}

void MasterAssistantPanel::resetForSession()
{
    cancel();
    baseChain_.clear();
    candidatePath_.clear();
    acceptedPath_.clear();
    inputPath_.clear();
    statusLabel_.setText({}, juce::dontSendNotification);
    verifyLabel_.setText({}, juce::dontSendNotification);
    setStage(0);
    refreshTarget();
}

bool MasterAssistantPanel::sessionChanged() const
{
    const auto* s = controller_.getActiveSession();
    return s == nullptr || juce::String(s->id) != jobSessionId_;
}

void MasterAssistantPanel::master()
{
    const auto* session = controller_.getActiveSession();
    if (session == nullptr || session->files.empty()) return;
    if (!controller_.hasAnalysis()) {
        statusLabel_.setText(TRANS("Wait for the analysis: the agent masters toward what it measured."),
                             juce::dontSendNotification);
        return;
    }
    if (!controller_.analysis().genreRelativeActionAllowed()) {
        statusLabel_.setText(TRANS("Mastering abstained: ") + controller_.analysis().genreRelativeAbstainedReason(),
                             juce::dontSendNotification);
        return;
    }
    jobSessionId_ = juce::String(session->id);
    inputPath_ = controller_.renders().renderInputPath(); // WF-1: the current version
    candidatePath_.clear();
    baseChain_.clear();
    verifyLabel_.setText({}, juce::dontSendNotification);
    statusLabel_.setText(TRANS("Mastering... (the agent renders and measures up to %n passes)")
                             .replace("%n", juce::String(kAgentIterations)),
                         juce::dontSendNotification);
    setStage(1);

    const juce::String genreId(session->target.primary_genre);
    const auto stems = session_audio::activeStemPaths(controller_);
    const auto input = inputPath_;
    const auto set = currentSet();
    const juce::String style(master_macros::styleId(set.style));
    const double strength = set.strength;
    juce::Component::SafePointer<MasterAssistantPanel> self(this);
    controller_.runLongInBackground([self, input, genreId, stems, style, strength] {
        auto started = ipc::HttpServiceBridge{}.startMasteringAgentJob(input, genreId, {}, kAgentIterations, true, {},
                                                                       stems, style, strength);
        juce::MessageManager::callAsync([self, started = std::move(started)] {
            if (self == nullptr) return;
            if (started.is_err()) {
                self->statusLabel_.setText(TRANS("Could not start: ") + serviceErrorLine(started.error()),
                                           juce::dontSendNotification);
                return self->setStage(0);
            }
            self->jobId_ = started.value();
            if (self->sessionChanged()) return self->cancel();
            self->poll(self->jobId_);
        });
    });
}

void MasterAssistantPanel::cancel()
{
    if (pollStop_) pollStop_->store(true);
    if (jobId_.isNotEmpty()) {
        const auto id = jobId_;
        controller_.runInBackground([id] { ipc::HttpServiceBridge{}.cancelMasteringAgentJob(id); });
    }
    jobId_.clear();
    if (stage_ == 1) {
        statusLabel_.setText(TRANS("Cancelled."), juce::dontSendNotification);
        setStage(0);
    }
}

void MasterAssistantPanel::poll(const juce::String& jobId)
{
    if (pollStop_) pollStop_->store(true);
    auto stop = std::make_shared<std::atomic<bool>>(false);
    pollStop_ = stop;
    juce::Component::SafePointer<MasterAssistantPanel> self(this);
    auto* controller = &controller_;
    controller_.runLongInBackground([self, jobId, stop, controller] {
        ipc::HttpServiceBridge bridge;
        using Poller = JobPoller<ipc::HttpServiceBridge::MasteringJobStatus>;
        Poller::Hooks hooks;
        hooks.poll = [&bridge](const std::string& id) { return bridge.getMasteringAgentJobStatus(juce::String(id)); };
        hooks.cancel = [&bridge](const std::string& id) { bridge.cancelMasteringAgentJob(juce::String(id)); };
        hooks.isTerminal = [](const auto& st) { return !khensu::api::isActiveStatus(st.status); };
        const uint64_t epoch = controller->sessionEpoch();
        hooks.shouldStop = [stop, controller, epoch] { return stop->load() || controller->sessionEpoch() != epoch; };
        Poller::Options opts;
        opts.pollIntervalMs = 1000;
        opts.deadlineSeconds = 30 * 60;
        const auto out = Poller::run(hooks, opts, jobId.toStdString());
        juce::MessageManager::callAsync([self, jobId, out] {
            if (self == nullptr || jobId != self->jobId_) return; // cancelled or superseded
            self->jobId_.clear();
            if (self->sessionChanged()) return self->setStage(0);
            if (!out.finished() || !out.status.has_value()) {
                self->statusLabel_.setText(TRANS("Mastering stopped: ") + out.describe(), juce::dontSendNotification);
                return self->setStage(0);
            }
            self->onFinished(*out.status);
        });
    });
}

void MasterAssistantPanel::onFinished(const ipc::HttpServiceBridge::MasteringJobStatus& status)
{
    if (status.status != khensu::api::job_status::completed || !status.result.has_value()) {
        statusLabel_.setText(TRANS("Mastering failed: ") +
                                 (status.error.isNotEmpty() ? status.error : juce::String(status.status)),
                             juce::dontSendNotification);
        return setStage(0);
    }
    const auto& res = *status.result;
    // The chain of the pass the agent chose (its final output), else its last.
    baseChain_.clear();
    for (const auto& it : res.iterations)
        if (it.outputPath == res.finalOutputPath && !it.chain.empty()) baseChain_ = it.chain;
    if (baseChain_.empty() && !res.iterations.empty()) baseChain_ = res.iterations.back().chain;
    decisionHash_ = res.decisionHash;

    juce::String text = res.converged
                            ? TRANS("Mastered in %n pass(es).").replace("%n", juce::String((int)res.iterations.size()))
                            : TRANS("Mastered; the agent ran out of passes before converging.");
    text << " " << guardrail_text::summary(res.guardrail);
    if (guardrail_text::passed(res.guardrail) && res.finalOutputPath.isNotEmpty()) {
        candidatePath_ = res.finalOutputPath;
        controller_.setAbProcessedPath(candidatePath_); // the loudness-matched A/B
        text << "\n" << TRANS("In the A/B now (B). Listen, shape it with the macros, then Accept.");
    } else {
        text << "\n" << TRANS("The guardrail held this master back. Shape it with the macros, or master again.");
    }
    statusLabel_.setText(text, juce::dontSendNotification);
    showSummary(res);
    setStage(2);
}

void MasterAssistantPanel::shape()
{
    if (baseChain_.empty() || inputPath_.isEmpty()) return;
    IRenders::ChainRender render;
    render.description = TRANS("Master Assistant");
    render.chain = master_macros::apply(baseChain_, macros());
    render.genreRelative = true;
    render.genreActionAllowed = controller_.analysis().genreRelativeActionAllowed();
    render.abstainedReason = controller_.analysis().genreRelativeAbstainedReason();
    render.decisionHash = decisionHash_;
    const auto out = uniqueRenderFile(juce::File(inputPath_), "_master").getFullPathName();
    statusLabel_.setText(TRANS("Rendering the shaped master..."), juce::dontSendNotification);
    shapeButton_.setEnabled(false);
    acceptButton_.setEnabled(false);
    juce::Component::SafePointer<MasterAssistantPanel> self(this);
    controller_.renders().renderPreview(
        render, inputPath_, out,
        [self](IRenders::RenderOutcome outcome, const juce::String& pathOrError, const juce::var&) {
            if (self == nullptr) return;
            if (outcome == IRenders::RenderOutcome::Rendered) {
                self->candidatePath_ = pathOrError;
                self->controller_.setAbProcessedPath(pathOrError);
                self->statusLabel_.setText(
                    TRANS("Shaped master in the A/B (B). Accept it, or adjust and render again."),
                    juce::dontSendNotification);
            } else if (outcome == IRenders::RenderOutcome::Failed) {
                self->statusLabel_.setText(TRANS("Render failed: ") + pathOrError, juce::dontSendNotification);
            }
            self->setStage(outcome == IRenders::RenderOutcome::SessionChanged ? 0 : 2);
        });
}

void MasterAssistantPanel::accept()
{
    if (candidatePath_.isEmpty() || sessionChanged()) return;
    controller_.renders().registerRenderOf(candidatePath_, inputPath_, jobSessionId_);
    controller_.enqueueProvenanceEvent(jobSessionId_, "mastering_candidate_accepted",
                                       "Master Assistant: accepted after matched-level A/B");
    acceptedPath_ = candidatePath_;
    statusLabel_.setText(TRANS("Accepted - now the current version. Checking it against the original's findings..."),
                         juce::dontSendNotification);
    setStage(3);
    showVerification();
}

void MasterAssistantPanel::showVerification()
{
    if (acceptedPath_.isEmpty()) return;
    if (const auto* r = controller_.analysis().currentVerification();
        r != nullptr && juce::File(juce::String(r->renderPath)) == juce::File(acceptedPath_)) {
        verifyLabel_.setText(TRANS("Check: ") + juce::String(fix_verification::summary(*r)) + ".",
                             juce::dontSendNotification);
    } else if (controller_.analysis().verificationRunning()) {
        verifyLabel_.setText(TRANS("Checking the master..."), juce::dontSendNotification);
    }
}

// ---- UXR-2: captions -------------------------------------------------------

juce::String MasterAssistantPanel::macroCaption(int index)
{
    switch (index) {
    case kLoudness:
        return TRANS("Gain into the limiter: +1 dB is about +1 LU louder until the ceiling holds it back.");
    case kTone:
        return TRANS("Scales every EQ move in the agent's chain: 50% halves each boost or cut in dB, 0% is flat.");
    case kWidth:
        return TRANS("Stereo width against the master: +20% is 1.2x as wide, -20% is 0.8x (narrower).");
    case kPunch:
        return TRANS("Scales compression above 1:1 and transient attack: 150% turns a 3:1 into 4:1.");
    case kCeiling:
        return TRANS("The limiter's true-peak ceiling: -1 dBTP leaves room for lossy encoders; nearer 0 is louder.");
    default: break;
    }
    return {};
}

juce::String MasterAssistantPanel::styleCaption(master_macros::Style style)
{
    switch (style) {
    case master_macros::Style::Warm:
        return TRANS("Warm: a darker tone target (up to -22% brightness), a low shelf and gentle tape saturation.");
    case master_macros::Style::Open:
        return TRANS("Open: a brighter target (up to +18%), an air shelf at 12 kHz and up to 2 dB more dynamics.");
    case master_macros::Style::Loud:
        return TRANS("Loud: up to +4 LU louder (never past -6 LUFS) and 2 dB denser, under the same true-peak "
                     "ceiling.");
    case master_macros::Style::Balanced: break;
    }
    return TRANS("Balanced: the genre's own loudness, dynamics and tone targets.");
}

void MasterAssistantPanel::updateStyleCaption()
{
    const auto set = currentSet();
    juce::String text = styleCaption(set.style);
    if (set.style != master_macros::Style::Balanced)
        text << " " << TRANS("At %s% of the full move.").replace("%s", juce::String(juce::roundToInt(set.strength * 100.0f)));
    styleCaption_.setText(text, juce::dontSendNotification);
}

// ---- ENH-5: sets -------------------------------------------------------------

master_macros::MacroSet MasterAssistantPanel::currentSet(const juce::String& name) const
{
    master_macros::MacroSet set;
    set.name = name;
    set.style = static_cast<master_macros::Style>(juce::jlimit(0, 3, styleBox_.getSelectedId() - 1));
    set.strength = static_cast<float>(strengthSlider_.getValue() / 100.0);
    set.macros = macros();
    return set;
}

void MasterAssistantPanel::applySet(const master_macros::MacroSet& set)
{
    styleBox_.setSelectedId(static_cast<int>(set.style) + 1, juce::dontSendNotification);
    strengthSlider_.setValue(set.strength * 100.0, juce::dontSendNotification);
    macros_[kLoudness].slider.setValue(set.macros.loudnessDb, juce::dontSendNotification);
    macros_[kTone].slider.setValue(set.macros.tone * 100.0, juce::dontSendNotification);
    macros_[kWidth].slider.setValue(set.macros.widthPct, juce::dontSendNotification);
    macros_[kPunch].slider.setValue(set.macros.punch * 100.0, juce::dontSendNotification);
    macros_[kCeiling].slider.setValue(set.macros.ceilingDbtp, juce::dontSendNotification);
    updateStyleCaption();
}

void MasterAssistantPanel::loadSets()
{
    auto db = controller_.sessions().sessionManagerPtr();
    if (db == nullptr) return;
    juce::Component::SafePointer<MasterAssistantPanel> self(this);
    controller_.runInBackground([self, db] {
        auto r = db->getUserPreference(master_macros::kSetsPreferenceKey);
        const juce::String json = r.is_ok() ? juce::String(r.value()) : juce::String();
        juce::MessageManager::callAsync([self, json] {
            if (self == nullptr) return;
            self->sets_ = master_macros::parseSets(json); // not session data: no epoch check
            self->rebuildSetsBox();
        });
    });
}

void MasterAssistantPanel::storeSets()
{
    auto db = controller_.sessions().sessionManagerPtr();
    if (db == nullptr) return;
    const std::string json = master_macros::serializeSets(sets_).toStdString();
    if (controller_.isShuttingDown()) {
        db->setUserPreference(master_macros::kSetsPreferenceKey, json);
        return;
    }
    controller_.runInBackground([db, json] { db->setUserPreference(master_macros::kSetsPreferenceKey, json); });
}

void MasterAssistantPanel::rebuildSetsBox()
{
    const juce::String selected = setsBox_.getSelectedId() >= kFirstSetId ? setsBox_.getText() : juce::String();
    setsBox_.clear(juce::dontSendNotification);
    setsBox_.setTextWhenNothingSelected(sets_.empty() ? TRANS("No saved sets yet") : TRANS("Load a set..."));
    for (size_t i = 0; i < sets_.size(); ++i) {
        setsBox_.addItem(sets_[i].name, kFirstSetId + static_cast<int>(i));
        if (sets_[i].name == selected) setsBox_.setSelectedId(kFirstSetId + static_cast<int>(i), juce::dontSendNotification);
    }
    setsBox_.setEnabled(!sets_.empty());
    deleteSetButton_.setEnabled(setsBox_.getSelectedId() >= kFirstSetId);
}

void MasterAssistantPanel::promptSaveSet()
{
    dialog_ = std::make_unique<juce::AlertWindow>(
        TRANS("Save macro set"), TRANS("Name this style, strength and macro setting. A set with the same name is "
                                       "replaced."),
        juce::MessageBoxIconType::NoIcon, this);
    dialog_->addTextEditor("name", setsBox_.getSelectedId() >= kFirstSetId ? setsBox_.getText() : juce::String(),
                           TRANS("Name"));
    dialog_->addButton(TRANS("Save"), 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog_->addButton(TRANS("Cancel"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MasterAssistantPanel> self(this);
    dialog_->enterModalState(true, juce::ModalCallbackFunction::create([self](int r) {
                                 if (self == nullptr || self->dialog_ == nullptr) return;
                                 const auto name = self->dialog_->getTextEditorContents("name").trim();
                                 self->dialog_.reset();
                                 if (r != 1 || name.isEmpty()) return;
                                 self->sets_ = master_macros::withSet(self->sets_, self->currentSet(name));
                                 self->storeSets();
                                 self->rebuildSetsBox();
                                 for (size_t i = 0; i < self->sets_.size(); ++i)
                                     if (self->sets_[i].name == name)
                                         self->setsBox_.setSelectedId(kFirstSetId + static_cast<int>(i),
                                                                      juce::dontSendNotification);
                                 self->deleteSetButton_.setEnabled(true);
                             }));
}

void MasterAssistantPanel::deleteSelectedSet()
{
    if (setsBox_.getSelectedId() < kFirstSetId) return;
    sets_ = master_macros::withoutSet(sets_, setsBox_.getText());
    setsBox_.setSelectedId(0, juce::dontSendNotification);
    storeSets();
    rebuildSetsBox();
}

void MasterAssistantPanel::showSummary(const ipc::HttpServiceBridge::MasteringAgentResult& res)
{
    namespace M = khensu::api::fields::MasteringMixMetrics;
    namespace T = khensu::api::fields::MasteringAgentTargets;
    const juce::var* before = nullptr;
    const juce::var* after = nullptr;
    for (const auto& it : res.iterations) {
        if (it.chain.empty() && before == nullptr) before = &it.metrics; // the untouched source pass
        if (it.outputPath == res.finalOutputPath) after = &it.metrics;
    }
    auto num = [](const juce::var* v, const char* key, int dp) {
        if (v == nullptr) return juce::String("-");
        const auto x = (*v)[key];
        return (x.isDouble() || x.isInt() || x.isInt64()) ? juce::String(static_cast<double>(x), dp) : juce::String("-");
    };
    auto target = [&res](const char* key, int dp) {
        const auto x = res.targets[key];
        return (x.isDouble() || x.isInt() || x.isInt64()) ? juce::String(static_cast<double>(x), dp) : juce::String("-");
    };
    juce::String text;
    text << TRANS("Loudness") << ": " << num(before, M::lufs, 1) << " -> " << num(after, M::lufs, 1) << " LUFS  ("
         << TRANS("target") << " " << target(T::lufs, 1) << ")\n";
    text << TRANS("Crest") << ": " << num(before, M::crest_db, 1) << " -> " << num(after, M::crest_db, 1) << " dB  ("
         << TRANS("target") << " " << target(T::crest_db, 1) << ")\n";
    text << TRANS("Brightness") << ": " << num(before, M::centroid_hz, 0) << " -> " << num(after, M::centroid_hz, 0)
         << " Hz  (" << TRANS("target") << " " << target(T::centroid_hz, 0) << ")\n";
    text << TRANS("Ceiling") << ": " << target(T::ceiling_dbtp, 1) << " dBTP\n";
    if (res.styleSummary.isNotEmpty()) text << res.styleSummary;
    summaryLabel_.setText(text, juce::dontSendNotification);
}

void MasterAssistantPanel::resized()
{
    PanelCard::resized();
    auto body = bodyArea();
    // UXR-2: the steps on the left, style / sets / before-after on the right.
    const bool twoColumns = body.getWidth() >= 760;
    auto b = twoColumns ? body.removeFromLeft(body.getWidth() * 56 / 100) : body;
    if (twoColumns) body.removeFromLeft(KhensuTheme::cardGap);

    targetLabel_.setBounds(b.removeFromTop(20));
    b.removeFromTop(KhensuTheme::sp2);

    auto row = b.removeFromTop(KhensuTheme::buttonH);
    masterButton_.setBounds(row.removeFromLeft(140));
    row.removeFromLeft(KhensuTheme::sp2);
    if (cancelButton_.isVisible()) cancelButton_.setBounds(row.removeFromLeft(90));
    b.removeFromTop(KhensuTheme::sp1);
    statusLabel_.setBounds(b.removeFromTop(48));
    b.removeFromTop(KhensuTheme::sp2);

    for (auto& m : macros_) {
        auto r = b.removeFromTop(26);
        m.label.setBounds(r.removeFromLeft(90));
        m.slider.setBounds(r.withWidth(juce::jmin(r.getWidth(), 520)));
        m.caption.setBounds(b.removeFromTop(16).withTrimmedLeft(90));
        b.removeFromTop(KhensuTheme::sp1);
    }
    b.removeFromTop(KhensuTheme::sp1);
    row = b.removeFromTop(KhensuTheme::buttonHSmall);
    shapeButton_.setBounds(row.removeFromLeft(200));
    row.removeFromLeft(KhensuTheme::sp2);
    resetMacrosButton_.setBounds(row.removeFromLeft(80));
    b.removeFromTop(KhensuTheme::sp3);

    row = b.removeFromTop(KhensuTheme::buttonH);
    acceptButton_.setBounds(row.removeFromLeft(170));
    row.removeFromLeft(KhensuTheme::sp2);
    exportButton_.setBounds(row.removeFromLeft(190));
    b.removeFromTop(KhensuTheme::sp1);
    verifyLabel_.setBounds(b.removeFromTop(40));

    // Right column (below the left one when the card is narrow).
    auto r = twoColumns ? body : b;
    styleHeading_.setBounds(r.removeFromTop(18));
    r.removeFromTop(KhensuTheme::sp1);
    row = r.removeFromTop(KhensuTheme::buttonHSmall);
    styleBox_.setBounds(row.removeFromLeft(juce::jmin(160, row.getWidth() / 2)));
    row.removeFromLeft(KhensuTheme::sp2);
    strengthLabel_.setBounds(row.removeFromLeft(64));
    strengthSlider_.setBounds(row);
    r.removeFromTop(KhensuTheme::sp1);
    styleCaption_.setBounds(r.removeFromTop(32));
    r.removeFromTop(KhensuTheme::sp3);

    setsHeading_.setBounds(r.removeFromTop(18));
    r.removeFromTop(KhensuTheme::sp1);
    row = r.removeFromTop(KhensuTheme::buttonHSmall);
    deleteSetButton_.setBounds(row.removeFromRight(72));
    row.removeFromRight(KhensuTheme::sp1);
    saveSetButton_.setBounds(row.removeFromRight(72));
    row.removeFromRight(KhensuTheme::sp1);
    setsBox_.setBounds(row);
    r.removeFromTop(KhensuTheme::sp2);
    applyProjectButton_.setBounds(r.removeFromTop(KhensuTheme::buttonHSmall).withWidth(juce::jmin(260, r.getWidth())));
    r.removeFromTop(KhensuTheme::sp3);

    summaryHeading_.setBounds(r.removeFromTop(18));
    r.removeFromTop(KhensuTheme::sp1);
    summaryLabel_.setBounds(r.removeFromTop(juce::jmax(0, juce::jmin(110, r.getHeight()))));
}

} // namespace khensu
