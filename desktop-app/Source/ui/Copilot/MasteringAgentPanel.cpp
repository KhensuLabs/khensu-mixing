// desktop-app/Source/ui/Copilot/MasteringAgentPanel.cpp
#include "MasteringAgentPanel.h"
#include <khensu/ipc/generated/ApiJobStatus.h>
#include "../shared/GuardrailText.h"
#include "../shared/JobPoller.h"
#include "../shared/SessionAudioPaths.h"
#include "../shared/KhensuTheme.h"
#include "../shared/ServiceErrorHint.h"
#include <khensu/session/Session.h>
#include <khensu/targets/GenreDatabase.h>
#include <map>
#include <thread>

namespace khensu {

namespace apif = api::fields;

namespace {
constexpr int kCardH = 110;
constexpr int kCardGap = 8;

/// One metric line with a readable name and the right unit (UIX-15): this
/// used to print raw keys and "dB" for everything ("LUFS: -14.2 dB",
/// "CREST_DB: 9.1 dB", "CENTROID_HZ"). Loudness reads in LUFS, its distance
/// from the target in LU.
juce::String formatDelta(const juce::var& deltas, const char* key)
{
    auto* obj = deltas.getDynamicObject();
    if (obj == nullptr) return {};
    auto dim = obj->getProperty(key);
    auto* dimObj = dim.getDynamicObject();
    if (dimObj == nullptr) return {};
    const juce::String k(key);
    juce::String name = k, unit, deltaUnit;
    if (k == apif::MasteringDeltas::lufs) {
        name = TRANS("Loudness");
        unit = " LUFS";
        deltaUnit = " LU";
    } else if (k == apif::MasteringDeltas::crest_db) {
        name = TRANS("Crest factor");
        unit = deltaUnit = " dB";
    } else if (k == apif::MasteringDeltas::centroid_hz) {
        name = TRANS("Brightness (centroid)");
        unit = deltaUnit = " Hz";
    }
    const double measured = static_cast<double>(dimObj->getProperty(apif::MasteringMetricDelta::measured));
    const double target = static_cast<double>(dimObj->getProperty(apif::MasteringMetricDelta::target));
    const double delta = static_cast<double>(dimObj->getProperty(apif::MasteringMetricDelta::delta));
    const int places = unit == " Hz" ? 0 : 1;
    return name + ": " + juce::String(measured, places) + unit + " (" + TRANS("target") + " "
           + juce::String(target, places) + unit + ", " + (delta >= 0 ? "+" : "") + juce::String(delta, places)
           + deltaUnit + ")";
}

} // anonymous namespace

void MasteringAgentPanel::IterationChartComponent::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour(KhensuTheme::bgRaised);
    g.fillRoundedRectangle(b, 6.f);

    if (lufsSeries.size() < 2) return;
    auto inner = b.reduced(8.f);

    auto plot = [&](const std::vector<float>& series, float lo, float hi, juce::Colour col) {
        if (series.size() < 2) return;
        juce::Path path;
        for (size_t i = 0; i < series.size(); ++i) {
            const float t = series.size() > 1 ? (float)i / (float)(series.size() - 1) : 0.0f;
            const float frac = juce::jlimit(0.0f, 1.0f, (series[i] - lo) / (hi - lo));
            const float x = inner.getX() + t * inner.getWidth();
            const float y = inner.getBottom() - frac * inner.getHeight();
            if (i == 0)
                path.startNewSubPath(x, y);
            else
                path.lineTo(x, y);
        }
        g.setColour(col);
        g.strokePath(path, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved));
    };

    // Series colours come from the theme so the two curves stay distinguishable
    // (and legible) in all three themes; the legend below reuses the SAME two
    // tokens, so a palette change can't desync curve and label.
    plot(lufsSeries, -30.0f, 0.0f, KhensuTheme::accentBlue);
    plot(crestSeries, 0.0f, 24.0f, KhensuTheme::accentAmber);

    g.setFont(KhensuTheme::labelFont(8.0f));
    g.setColour(KhensuTheme::accentBlue);
    g.drawText("LUFS", inner.removeFromTop(10.f), juce::Justification::topLeft);
    g.setColour(KhensuTheme::accentAmber);
    g.drawText("Crest", getLocalBounds().toFloat().reduced(8.f).removeFromTop(10.f).withTrimmedLeft(50.f),
               juce::Justification::topLeft);
}

void MasteringAgentPanel::ConvergenceTrajectoryComponent::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour(KhensuTheme::bgRaised);
    g.fillRoundedRectangle(b, 6.f);

    auto inner = b.reduced(10.f);
    const juce::Point<float> centre = inner.getCentre();

    // Axes through the origin (on-target).
    g.setColour(KhensuTheme::borderSubtle);
    g.drawHorizontalLine((int)centre.y, inner.getX(), inner.getRight());
    g.drawVerticalLine((int)centre.x, inner.getY(), inner.getBottom());

    if (deltas.size() < 2) {
        g.setFont(KhensuTheme::labelFont(8.f));
        g.setColour(KhensuTheme::textSecondary);
        g.drawText("Convergence to target", getLocalBounds().toFloat(), juce::Justification::centred);
        return;
    }

    // Normalize deltas against a plausible +/-6 LUFS, +/-8 dB crest range
    // so the trajectory uses the plot area regardless of how far off-target
    // the first iteration started.
    juce::Path path;
    for (size_t i = 0; i < deltas.size(); ++i) {
        const float x = centre.x + juce::jlimit(-1.0f, 1.0f, deltas[i].x / 6.0f) * inner.getWidth() * 0.5f;
        const float y = centre.y - juce::jlimit(-1.0f, 1.0f, deltas[i].y / 8.0f) * inner.getHeight() * 0.5f;
        if (i == 0)
            path.startNewSubPath(x, y);
        else
            path.lineTo(x, y);
        g.setColour(i + 1 == deltas.size() ? KhensuTheme::accentTeal : KhensuTheme::accentAmber.withAlpha(0.6f));
        g.fillEllipse(x - 2.5f, y - 2.5f, 5.0f, 5.0f);
    }
    g.setColour(KhensuTheme::accentAmber.withAlpha(0.7f));
    g.strokePath(path, juce::PathStrokeType(1.3f));

    g.setFont(KhensuTheme::labelFont(7.5f));
    g.setColour(KhensuTheme::textSecondary);
    g.drawText("CONVERGENCE (-> target)", getLocalBounds().toNearestInt().removeFromTop(10),
               juce::Justification::centred);
}

void MasteringAgentPanel::IterationCard::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced(2.f);
    g.setColour(KhensuTheme::bgRaised);
    g.fillRoundedRectangle(b, 8.f);
    g.setColour((converged_ ? KhensuTheme::accentTeal : KhensuTheme::accentAmber).withAlpha(0.4f));
    g.drawRoundedRectangle(b, 8.f, 1.f);
}

void MasteringAgentPanel::IterationCard::resized()
{
    auto b = getLocalBounds().reduced(10);
    headerLabel_.setBounds(b.removeFromTop(22));
    b.removeFromTop(2);
    critiqueLabel_.setBounds(b.removeFromTop(36));
    b.removeFromTop(2);
    metricsLabel_.setBounds(b);
}

// Every cached palette colour on this panel, in one place (UI-01).
void MasteringAgentPanel::applyThemeColours()
{
    for (auto* l : {&genreLabel_, &iterLabel_, &statusLabel_, &guardrailLabel_})
        l->setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    runButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal);
    runButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::bgVoid);
    cancelButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentRed.darker(0.6f));
}

std::string MasteringAgentPanel::selectedGenreIdForTesting() const
{
    const auto& genres = controller_.genreDatabase().listAll();
    const int i = genreBox_.getSelectedItemIndex();
    return i >= 0 && i < (int)genres.size() ? genres[(size_t)i].id : std::string();
}

void MasteringAgentPanel::syncToSessionGenre()
{
    const auto* session = controller_.getActiveSession();
    if (session == nullptr || session->target.primary_genre.empty()) return;
    const auto& genres = controller_.genreDatabase().listAll();
    for (int i = 0; i < (int)genres.size(); ++i)
        if (genres[(size_t)i].id == session->target.primary_genre) {
            genreBox_.setSelectedItemIndex(i, juce::dontSendNotification);
            return;
        }
}

MasteringAgentPanel::MasteringAgentPanel(IAppController& c, std::string baseUrl)
    : controller_(c), baseUrl_(std::move(baseUrl))
{
    applyThemeColours();
    genreLabel_.setFont(KhensuTheme::labelFont(11.f));
    addAndMakeVisible(genreLabel_);

    for (const auto& g : controller_.genreDatabase().listAll())
        genreBox_.addItem(juce::String(g.name), genreBox_.getNumItems() + 1);
    if (genreBox_.getNumItems() > 0) genreBox_.setSelectedItemIndex(0, juce::dontSendNotification);
    addAndMakeVisible(genreBox_);

    iterLabel_.setFont(KhensuTheme::labelFont(11.f));
    addAndMakeVisible(iterLabel_);

    iterSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    iterSlider_.setRange(2.0, 8.0, 1.0);
    iterSlider_.setValue(5.0, juce::dontSendNotification);
    iterSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 20);
    addAndMakeVisible(iterSlider_);

    runButton_.onClick = [this] { run(); };
    addAndMakeVisible(runButton_);

    cancelButton_.onClick = [this] { cancelRun(); };
    addAndMakeVisible(cancelButton_);
    cancelButton_.setVisible(false);

    acceptCandidateButton_.onClick = [this] { acceptCandidate(); };
    acceptCandidateButton_.setVisible(false);
    addChildComponent(acceptCandidateButton_);
    rejectCandidateButton_.onClick = [this] { rejectCandidate(); };
    rejectCandidateButton_.setVisible(false);
    addChildComponent(rejectCandidateButton_);

    statusLabel_.setFont(KhensuTheme::labelFont(12.f));
    addAndMakeVisible(statusLabel_);
    guardrailLabel_.setFont(KhensuTheme::labelFont(10.f));
    guardrailLabel_.setJustificationType(juce::Justification::centredLeft);
    guardrailLabel_.setMinimumHorizontalScale(0.65f);
    addAndMakeVisible(guardrailLabel_);

    addAndMakeVisible(iterationChart_);
    addAndMakeVisible(convergenceChart_);

    cardsViewport_.setViewedComponent(&cardsContent_, false);
    cardsViewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(cardsViewport_);
}

MasteringAgentPanel::~MasteringAgentPanel()
{
    if (pollStop_) pollStop_->store(true); // the poller cancels the job server-side
}

void MasteringAgentPanel::IterationCard::applyColours()
{
    headerLabel_.setColour(juce::Label::textColourId, converged_ ? KhensuTheme::accentTeal : KhensuTheme::accentAmber);
    critiqueLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    metricsLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

void MasteringAgentPanel::paint(juce::Graphics& g)
{
    g.fillAll(KhensuTheme::bgBase);
    auto header = getLocalBounds().removeFromTop(40).toFloat();
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.12f));
    g.fillRect(header);
    g.setColour(KhensuTheme::accentTeal);
    g.setFont(juce::Font(juce::FontOptions{15.f, juce::Font::bold}));
    g.drawText("  AI Mastering Agent - iterative self-correction", header.toNearestInt(),
               juce::Justification::centredLeft);
}

void MasteringAgentPanel::resized()
{
    auto b = getLocalBounds();
    b.removeFromTop(40); // header
    auto controls = b.removeFromTop(158).reduced(10);

    auto row1 = controls.removeFromTop(38);
    auto genreCol = row1.removeFromLeft(row1.getWidth() / 2).reduced(4, 0);
    genreLabel_.setBounds(genreCol.removeFromTop(14));
    genreBox_.setBounds(genreCol.removeFromTop(22));
    auto iterCol = row1.reduced(4, 0);
    iterLabel_.setBounds(iterCol.removeFromTop(14));
    iterSlider_.setBounds(iterCol.removeFromTop(22));

    controls.removeFromTop(6);
    auto row2 = controls.removeFromTop(30);
    if (cancelButton_.isVisible()) {
        cancelButton_.setBounds(row2.removeFromRight(90));
        row2.removeFromRight(6);
    }
    runButton_.setBounds(row2.removeFromLeft(220));
    row2.removeFromLeft(10);
    statusLabel_.setBounds(row2);
    auto decisionRow = controls.removeFromTop(26);
    rejectCandidateButton_.setBounds(decisionRow.removeFromRight(140));
    decisionRow.removeFromRight(6);
    acceptCandidateButton_.setBounds(decisionRow.removeFromRight(170));
    guardrailLabel_.setBounds(controls.removeFromTop(24));

    if (!iterationChart_.lufsSeries.empty()) {
        auto chartRow = b.removeFromTop(70);
        iterationChart_.setBounds(chartRow.removeFromLeft(chartRow.getWidth() * 2 / 3));
        chartRow.removeFromLeft(6);
        convergenceChart_.setBounds(chartRow);
        b.removeFromTop(6);
    } else {
        iterationChart_.setBounds({});
        convergenceChart_.setBounds({});
    }

    cardsViewport_.setBounds(b);
    layoutCards();
}

void MasteringAgentPanel::layoutCards()
{
    const int vw = cardsViewport_.getWidth();
    const int n = static_cast<int>(cards_.size());
    int totalH = n * (kCardH + kCardGap) + kCardGap;
    cardsContent_.setBounds(0, 0, vw, juce::jmax(totalH, cardsViewport_.getHeight()));
    int y = kCardGap;
    for (auto& card : cards_) {
        card->setBounds(kCardGap, y, vw - kCardGap * 2, kCardH);
        y += kCardH + kCardGap;
    }
}

bool MasteringAgentPanel::jobSessionChanged() const
{
    const auto* s = controller_.getActiveSession();
    return s == nullptr || juce::String(s->id) != jobSessionId_;
}

void MasteringAgentPanel::run()
{
    if (!currentJobId_.isEmpty()) return;
    const auto* session = controller_.getActiveSession();
    if (session == nullptr || session->files.empty()) {
        statusLabel_.setText(TRANS("No active session audio."), juce::dontSendNotification);
        return;
    }
    if (!controller_.hasAnalysis()) {
        statusLabel_.setText(TRANS("Analyze the session before starting a genre-relative mastering pass."),
                             juce::dontSendNotification);
        return;
    }
    if (!controller_.analysis().genreRelativeActionAllowed()) {
        statusLabel_.setText(TRANS("Mastering abstained: ") + controller_.analysis().genreRelativeAbstainedReason(),
                             juce::dontSendNotification);
        return;
    }

    juce::String genreId;
    const int idx = genreBox_.getSelectedItemIndex();
    const auto& genres = controller_.genreDatabase().listAll();
    if (idx >= 0 && idx < static_cast<int>(genres.size())) genreId = juce::String(genres[(size_t)idx].id);

    // WF-1: master the current version (fixes stack) unless the user chose
    // to start from the original.
    const juce::String filePath = controller_.renders().renderInputPath();
    const int maxIterations = static_cast<int>(iterSlider_.getValue());

    for (auto& c : cards_)
        cardsContent_.removeChildComponent(c.get());
    cards_.clear();
    iterationChart_.lufsSeries.clear();
    iterationChart_.crestSeries.clear();
    convergenceChart_.deltas.clear();
    layoutCards();

    runButton_.setEnabled(false);
    cancelButton_.setVisible(true);
    guardrailLabel_.setText({}, juce::dontSendNotification);
    resized();
    statusLabel_.setText(TRANS("Starting..."), juce::dontSendNotification);

    juce::Component::SafePointer<MasteringAgentPanel> self(this);
    const juce::String abstainedReason = controller_.analysis().genreRelativeAbstainedReason(); // "" when allowed
    jobSessionId_ = juce::String(session->id);
    jobInputPath_ = filePath;
    pendingCandidatePath_.clear();
    pendingCandidateDecisionHash_ = {};
    acceptCandidateButton_.setVisible(false);
    rejectCandidateButton_.setVisible(false);
    const auto stemPaths = session_audio::activeStemPaths(controller_);
    controller_.runLongInBackground([self, filePath, genreId, maxIterations, abstainedReason, stemPaths, baseUrl = baseUrl_]() mutable {
        ipc::HttpServiceBridge bridge(baseUrl);
        auto result =
            bridge.startMasteringAgentJob(filePath, genreId, {}, maxIterations, true, abstainedReason, stemPaths);
        juce::MessageManager::callAsync([self, result = std::move(result)]() mutable {
            if (!self) return;
            if (result.is_err()) {
                self->runButton_.setEnabled(true);
                self->cancelButton_.setVisible(false);
                self->resized();
                const auto& error = result.error();
                const juce::String detail = serviceErrorDetail(error);
                self->statusLabel_.setText(isAbstainedRenderError(error)
                                               ? TRANS("Mastering abstained: ") + (detail.isNotEmpty() ? detail : juce::String(error.message))
                                               : TRANS("Error: ") + serviceErrorLine(error),
                                           juce::dontSendNotification);
                return;
            }
            self->currentJobId_ = result.value();
            if (self->jobSessionChanged()) { // session switched while /start was in flight
                self->cancelRun();
                self->statusLabel_.setText(TRANS("Session changed - mastering run cancelled."),
                                           juce::dontSendNotification);
                return;
            }
            self->pollJob(self->currentJobId_);
        });
    });
}

void MasteringAgentPanel::pollJob(const juce::String& jobId)
{
    if (pollStop_) pollStop_->store(true);
    auto stop = std::make_shared<std::atomic<bool>>(false);
    pollStop_ = stop;
    juce::Component::SafePointer<MasteringAgentPanel> self(this);
    auto* controller = &controller_;
    const juce::String sessionId = jobSessionId_;
    controller_.runLongInBackground([self, jobId, stop, controller, sessionId, baseUrl = baseUrl_]() {
        ipc::HttpServiceBridge bridge(baseUrl);
        using Poller = JobPoller<ipc::HttpServiceBridge::MasteringJobStatus>;
        Poller::Hooks hooks;
        hooks.poll = [&bridge](const std::string& id) { return bridge.getMasteringAgentJobStatus(juce::String(id)); };
        hooks.cancel = [&bridge](const std::string& id) { bridge.cancelMasteringAgentJob(juce::String(id)); };
        hooks.isTerminal = [](const auto& st) { return !khensu::api::isActiveStatus(st.status); };
        hooks.onProgress = [self, jobId](const auto& st) {
            juce::MessageManager::callAsync([self, jobId, status = st.status]() {
                if (!self || jobId != self->currentJobId_) return;
                self->statusLabel_.setText(TRANS("Running... (%s)").replace("%s", status), juce::dontSendNotification);
            });
        };
        // AUDIT-002: a session switch cancels the run; its candidate must
        // never reach the new session's A/B. The session lookup only reads
        // an id the message thread swaps atomically per session change, so
        // compare epochs instead of touching session state off-thread.
        const uint64_t epoch = controller->sessionEpoch();
        hooks.shouldStop = [stop, controller, epoch] { return stop->load() || controller->sessionEpoch() != epoch; };
        Poller::Options opts;
        opts.pollIntervalMs = 1000;
        opts.deadlineSeconds = 30 * 60;
        const auto out = Poller::run(hooks, opts, jobId.toStdString());

        juce::MessageManager::callAsync([self, jobId, out]() {
            if (!self || jobId != self->currentJobId_) return; // cancelled / superseded
            if (self->jobSessionChanged()) {
                self->cancelRun();
                self->statusLabel_.setText(TRANS("Session changed - mastering run cancelled."),
                                           juce::dontSendNotification);
                return;
            }
            if (!out.finished() || !out.status.has_value()) {
                self->currentJobId_ = {};
                self->runButton_.setEnabled(true);
                self->cancelButton_.setVisible(false);
                self->resized();
                self->statusLabel_.setText(TRANS("Mastering run stopped: ") + out.describe(),
                                           juce::dontSendNotification);
                return;
            }
            self->onJobFinished(*out.status);
        });
    });
}

void MasteringAgentPanel::onJobFinished(const ipc::HttpServiceBridge::MasteringJobStatus& status)
{
    auto* self = this; // the body below was the old poll callback's
    self->currentJobId_ = {};
    self->runButton_.setEnabled(true);
    self->cancelButton_.setVisible(false);
    self->resized();

    if (status.status == khensu::api::job_status::cancelled) {
        self->statusLabel_.setText(TRANS("Cancelled."), juce::dontSendNotification);
        return;
    }
    if (status.status != khensu::api::job_status::completed || !status.result.has_value()) {
        self->statusLabel_.setText(
            TRANS("Error: ") + (status.error.isNotEmpty() ? status.error : TRANS("agent failed")),
            juce::dontSendNotification);
        return;
    }

    const auto& res = *status.result;
    const bool guardrailPassed = guardrail_text::passed(res.guardrail);
    self->statusLabel_.setText(
        (res.converged ? TRANS("Converged in %n iteration(s).").replace("%n", juce::String((int)res.iterations.size()))
                       : TRANS("Iteration budget exhausted (not fully converged).")) +
            " " + guardrail_text::summary(res.guardrail),
        juce::dontSendNotification);
    self->guardrailLabel_.setText(guardrail_text::measurements(res.guardrail), juce::dontSendNotification);

    for (auto& c : self->cards_)
        self->cardsContent_.removeChildComponent(c.get());
    self->cards_.clear();

    self->iterationChart_.lufsSeries.clear();
    self->iterationChart_.crestSeries.clear();
    self->convergenceChart_.deltas.clear();
    for (const auto& it : res.iterations) {
        if (auto* obj = it.deltas.getDynamicObject()) {
            float lufsDelta = 0.0f, crestDelta = 0.0f;
            if (auto* lufsObj = obj->getProperty(apif::MasteringDeltas::lufs).getDynamicObject()) {
                self->iterationChart_.lufsSeries.push_back((float)(double)lufsObj->getProperty(apif::MasteringMetricDelta::measured));
                lufsDelta = (float)(double)lufsObj->getProperty(apif::MasteringMetricDelta::delta);
            }
            if (auto* crestObj = obj->getProperty(apif::MasteringDeltas::crest_db).getDynamicObject()) {
                self->iterationChart_.crestSeries.push_back((float)(double)crestObj->getProperty(apif::MasteringMetricDelta::measured));
                crestDelta = (float)(double)crestObj->getProperty(apif::MasteringMetricDelta::delta);
            }
            self->convergenceChart_.deltas.push_back({lufsDelta, crestDelta});
        }
    }

    for (const auto& it : res.iterations) {
        auto card = std::make_unique<IterationCard>();
        card->converged_ = it.converged;
        card->headerLabel_.setText((it.converged ? TRANS("Iteration %n  + converged") : TRANS("Iteration %n"))
                                       .replace("%n", juce::String(it.iteration)),
                                   juce::dontSendNotification);
        card->headerLabel_.setFont(juce::Font(juce::FontOptions{13.f, juce::Font::bold}));

        card->critiqueLabel_.setText(it.critique, juce::dontSendNotification);
        card->critiqueLabel_.setFont(KhensuTheme::labelFont(11.f));
        card->critiqueLabel_.setMinimumHorizontalScale(0.7f);

        juce::StringArray parts;
        for (const char* k : {apif::MasteringDeltas::lufs, apif::MasteringDeltas::crest_db,
                              apif::MasteringDeltas::centroid_hz}) {
            auto s = formatDelta(it.deltas, k);
            if (s.isNotEmpty()) parts.add(s);
        }
        // Vocal-band/artifact terms folded into the objective score,
        // not just the pass/fail guardrail gate - surfaced here so
        // the user can see why one candidate outranked another even
        // when lufs/crest/centroid were close.
        if (it.vocalPresenceDelta.has_value())
            parts.add(TRANS("Vocal presence") + ": " + juce::String(*it.vocalPresenceDelta >= 0 ? "+" : "") +
                      juce::String(*it.vocalPresenceDelta, 3));
        if (it.artifactScoreDelta.has_value())
            parts.add(TRANS("Artifact score") + ": " + juce::String(*it.artifactScoreDelta >= 0 ? "+" : "") +
                      juce::String(*it.artifactScoreDelta, 3));
        card->metricsLabel_.setText(parts.joinIntoString("   |   "), juce::dontSendNotification);
        card->metricsLabel_.setFont(KhensuTheme::labelFont(10.f));
        card->applyColours(); // live theme colours (UIX-22)

        self->cardsContent_.addAndMakeVisible(*card);
        self->cards_.push_back(std::move(card));
    }
    self->layoutCards();
    self->resized();
    self->iterationChart_.repaint();

    if (guardrailPassed && !res.finalOutputPath.isEmpty()) {
        // Mastering candidates are never silently promoted. Put the
        // candidate into the existing A/B transport for user choice.
        self->controller_.setAbProcessedPath(res.finalOutputPath);
        self->pendingCandidatePath_ = res.finalOutputPath;
        self->pendingCandidateDecisionHash_ = res.decisionHash;
        // The session the run was STARTED for (AUDIT-002), not
        // whichever one happens to be active at completion.
        self->pendingCandidateSessionId_ = self->jobSessionId_;
        self->pendingCandidateInputPath_ = self->jobInputPath_;
        self->acceptCandidateButton_.setVisible(true);
        self->rejectCandidateButton_.setVisible(true);
        if (res.requiresUserChoice)
            self->statusLabel_.setText(self->statusLabel_.getText() +
                                           " " + TRANS("A/B candidate loaded; choose after matched-level listening."),
                                       juce::dontSendNotification);
    }
    self->resized();
}

void MasteringAgentPanel::acceptCandidate()
{
    if (pendingCandidatePath_.isEmpty()) return;
    const auto sessionId = pendingCandidateSessionId_;
    const auto path = pendingCandidatePath_;
    const auto* active = controller_.getActiveSession();
    if (active == nullptr || juce::String(active->id) != sessionId) {
        // The candidate was rendered for another session: never register it here.
        pendingCandidatePath_.clear();
        acceptCandidateButton_.setVisible(false);
        rejectCandidateButton_.setVisible(false);
        statusLabel_.setText(TRANS("That candidate belongs to a different session - run the agent again here."),
                             juce::dontSendNotification);
        resized();
        return;
    }
    controller_.renders().registerRenderOf(path, pendingCandidateInputPath_, sessionId);
    controller_.enqueueProvenanceEvent(sessionId, "mastering_candidate_accepted",
                                       "User accepted matched-level mastering A/B candidate", {},
                                       candidateEventPayload());
    statusLabel_.setText(TRANS("Mastering candidate accepted and added to render history."), juce::dontSendNotification);
    pendingCandidatePath_.clear();
    acceptCandidateButton_.setVisible(false);
    rejectCandidateButton_.setVisible(false);
    resized();
}

juce::var MasteringAgentPanel::candidateEventPayload() const
{
    // An object, not the bare path this used to send: EventRequest.payload is
    // a dict, so a string payload was rejected (422) and the outbox dropped
    // the event.
    auto* d = new juce::DynamicObject();
    d->setProperty("output_path", pendingCandidatePath_);
    if (pendingCandidateDecisionHash_.isNotEmpty()) d->setProperty("decision_hash", pendingCandidateDecisionHash_);
    return juce::var(d);
}

void MasteringAgentPanel::rejectCandidate()
{
    if (pendingCandidatePath_.isEmpty()) return;
    const auto sessionId = pendingCandidateSessionId_;
    controller_.enqueueProvenanceEvent(sessionId, "mastering_candidate_rejected",
                                       "User rejected matched-level mastering A/B candidate", {},
                                       candidateEventPayload());
    // Put B back on the accepted render (or clear it on the original) - the
    // candidate was only ever a preview (AUDIT-001).
    controller_.setAbProcessedPath(controller_.currentRenderPath());
    statusLabel_.setText(TRANS("Mastering candidate rejected; the accepted render remains active."),
                         juce::dontSendNotification);
    pendingCandidatePath_.clear();
    acceptCandidateButton_.setVisible(false);
    rejectCandidateButton_.setVisible(false);
    resized();
}

void MasteringAgentPanel::rebuildIterationCards()
{
    // Reserved for a future mid-run progressive display; the current server
    // response only populates `result` once the whole loop finishes.
}

void MasteringAgentPanel::cancelRun()
{
    if (currentJobId_.isEmpty()) return;
    if (pollStop_) pollStop_->store(true);
    juce::String jobId = currentJobId_;
    currentJobId_ = {};
    runButton_.setEnabled(true);
    cancelButton_.setVisible(false);
    resized();
    statusLabel_.setText(TRANS("Cancelled."), juce::dontSendNotification);

    controller_.runLongInBackground([jobId, baseUrl = baseUrl_]() mutable {
        ipc::HttpServiceBridge bridge(baseUrl);
        bridge.cancelMasteringAgentJob(jobId);
    });
}

} // namespace khensu
