// desktop-app/Source/ui/Copilot/AutoMixPanel.cpp
#include "AutoMixPanel.h"
#include <khensu/ipc/generated/ApiJobStatus.h>
#include <khensu/ipc/generated/ApiSchema.h>
#include <khensu/session/Session.h>
#include "../shared/JobPoller.h"
#include "../shared/PanelRunState.h"
#include "../shared/ServiceErrorHint.h"
#include "../shared/SessionEpoch.h"
#include <cmath>

namespace khensu {

namespace apif = api::fields;

namespace {
/// "-14.2" with a fixed number of places, or "?" for a missing/NaN value.
juce::String num(const juce::var& v, int places)
{
    if (v.isVoid() || v.isUndefined()) return "?";
    const double d = static_cast<double>(v);
    return std::isfinite(d) ? juce::String(d, places) : juce::String("?");
}

juce::String decisionTag(const juce::String& decision)
{
    if (decision == "kept") return "KEPT";
    if (decision == "reverted") return "REVERTED";
    if (decision == "skipped") return "SKIPPED";
    return "RUNNING";
}
} // namespace

juce::String AutoMixPanel::formatStepLog(const juce::var& status)
{
    // A finished job carries the full result; a running one its progress.
    juce::var steps = status[apif::AgentMixStatusResponse::result][apif::AgentMixResult::steps];
    juce::var findings = status[apif::AgentMixStatusResponse::result][apif::AgentMixResult::findings];
    if (!steps.isArray()) {
        steps = status[apif::AgentMixStatusResponse::progress][apif::AgentProgress::steps];
        findings = status[apif::AgentMixStatusResponse::progress][apif::AgentProgress::findings];
    }
    juce::String out;
    const int planned = findings.isArray() ? findings.size() : 0;
    if (planned > 0) out << TRANS("Plan:") << " " << planned << " " << TRANS("step(s) from the findings") << "\n\n";
    if (auto* arr = steps.getArray()) {
        for (const auto& s : *arr) {
            const juce::String decision = s[apif::AgentStep::decision].toString();
            out << s[apif::AgentStep::index].toString() << ". " << s[apif::AgentStep::title].toString() << "  ["
                << decisionTag(decision) << "]\n";
            const juce::String why = s[apif::AgentStep::reason].toString();
            if (why.isNotEmpty()) out << "   " << TRANS("Why:") << " " << why << "\n";
            if (auto* attempts = s[apif::AgentStep::attempts].getArray()) {
                for (const auto& a : *attempts) {
                    juce::StringArray fx;
                    if (auto* effects = a[apif::AgentAttempt::effects].getArray())
                        for (const auto& e : *effects) fx.add(e[apif::AgentEffect::effect].toString());
                    out << "   - " << TRANS("strength") << " " << num(a[apif::AgentAttempt::strength], 2) << " ("
                        << fx.joinIntoString(", ") << "): " << a[apif::AgentAttempt::decision].toString();
                    const juce::String reason = a[apif::AgentAttempt::reason].toString();
                    if (reason.isNotEmpty()) out << " - " << reason;
                    const auto gain = a[apif::AgentAttempt::matched_gain_db];
                    if (!gain.isVoid()) out << "  [" << TRANS("matched by") << " " << num(gain, 1) << " dB]";
                    out << "\n";
                }
            }
            const juce::String verdict = s[apif::AgentStep::decision_reason].toString();
            if (verdict.isNotEmpty() && decision == "skipped") out << "   " << verdict << "\n";
            out << "\n";
        }
    }
    const auto result = status[apif::AgentMixStatusResponse::result];
    if (result.isObject()) {
        out << result[apif::AgentMixResult::summary].toString() << "\n";
        const auto before = result[apif::AgentMixResult::before];
        const auto after = result[apif::AgentMixResult::after];
        out << TRANS("Loudness:") << " " << num(before["lufs"], 1) << " -> " << num(after["lufs"], 1) << " LUFS, "
            << TRANS("true peak") << " " << num(before["true_peak_dbtp"], 1) << " -> "
            << num(after["true_peak_dbtp"], 1) << " dBTP\n";
        if (auto* rem = result[apif::AgentMixResult::remaining_out_of_range].getArray(); rem && !rem->isEmpty()) {
            juce::StringArray names;
            for (const auto& r : *rem) names.add(r.toString());
            out << TRANS("Still out of range:") << " " << names.joinIntoString(", ") << "\n";
        }
    }
    return out;
}

void AutoMixPanel::applyThemeColours()
{
    for (auto* l : {&targetLabel_, &stepsLabel_, &statusLabel_})
        l->setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    runButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal);
    runButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::bgVoid);
    cancelButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentRed.darker(0.6f));
    log_.setColour(juce::TextEditor::backgroundColourId, KhensuTheme::bgRaised);
    log_.setColour(juce::TextEditor::textColourId, KhensuTheme::textPrimary);
    log_.setColour(juce::TextEditor::outlineColourId, KhensuTheme::borderSubtle);
}

AutoMixPanel::AutoMixPanel(IAppController& c, std::string baseUrl) : controller_(c), baseUrl_(std::move(baseUrl))
{
    applyThemeColours();
    for (auto* l : {&targetLabel_, &stepsLabel_})
        l->setFont(KhensuTheme::labelFont(type::caption));

    targetSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    targetSlider_.setRange(-24.0, -6.0, 0.5);
    targetSlider_.setValue(-14.0, juce::dontSendNotification);
    targetSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
    targetSlider_.setTitle(TRANS("Target loudness"));
    stepsSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    stepsSlider_.setRange(1.0, 10.0, 1.0);
    stepsSlider_.setValue(6.0, juce::dontSendNotification);
    stepsSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 20);
    stepsSlider_.setTitle(TRANS("Maximum number of steps"));
    useRecsToggle_.setTooltip(TRANS("On: the agent works through this session's recommendations. Off: it measures "
                                    "the mix itself and plans from what is out of range."));
    loudnessToggle_.setToggleState(true, juce::dontSendNotification);
    loudnessToggle_.setTooltip(TRANS("Let the agent set the final loudness and a -1 dBTP ceiling as its last step."));

    runButton_.setTooltip(TRANS("Render each step, re-measure at matched loudness, keep it only if it helped"));
    runButton_.onClick = [this] { run(); };
    cancelButton_.onClick = [this] { cancelRun(); };
    auditionButton_.setTooltip(TRANS("Load the result as the B side of the loudness-matched A/B"));
    auditionButton_.onClick = [this] { auditionResult(); };
    acceptButton_.setTooltip(TRANS("Add every kept step to the render history, each one derived from the last"));
    acceptButton_.onClick = [this] { acceptResult(); };

    statusLabel_.setFont(KhensuTheme::labelFont(type::secondary));
    statusLabel_.setText(TRANS("Ready. Nothing is changed until you accept the result."), juce::dontSendNotification);
    log_.setMultiLine(true);
    log_.setReadOnly(true);
    log_.setScrollbarsShown(true);
    log_.setFont(KhensuTheme::monoFont(type::caption));
    log_.setTitle(TRANS("Auto-mix step log"));

    for (juce::Component* comp :
         std::initializer_list<juce::Component*>{&targetLabel_, &targetSlider_, &stepsLabel_, &stepsSlider_,
                                                 &useRecsToggle_, &loudnessToggle_, &runButton_, &cancelButton_,
                                                 &auditionButton_, &acceptButton_, &statusLabel_, &log_})
        addAndMakeVisible(comp);
    cancelButton_.setVisible(false);
    auditionButton_.setEnabled(false);
    acceptButton_.setEnabled(false);
}

AutoMixPanel::~AutoMixPanel()
{
    if (pollStop_) pollStop_->store(true); // the poller cancels the job server-side
}

void AutoMixPanel::paint(juce::Graphics& g)
{
    g.fillAll(KhensuTheme::bgBase);
    auto header = getLocalBounds().removeFromTop(40).toFloat();
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.12f));
    g.fillRect(header);
    g.setColour(KhensuTheme::accentTeal);
    g.setFont(KhensuTheme::uiFont(type::subtitle, juce::Font::bold));
    g.drawText("  " + TRANS("Auto-mix (verified) - every step is measured before it is kept"), header.toNearestInt(),
               juce::Justification::centredLeft);
}

void AutoMixPanel::resized()
{
    auto b = getLocalBounds();
    b.removeFromTop(40);
    auto controls = b.removeFromTop(132).reduced(10, 6);
    auto row1 = controls.removeFromTop(38);
    auto left = row1.removeFromLeft(row1.getWidth() / 2).reduced(4, 0);
    targetLabel_.setBounds(left.removeFromTop(14));
    targetSlider_.setBounds(left.removeFromTop(22));
    auto right = row1.reduced(4, 0);
    stepsLabel_.setBounds(right.removeFromTop(14));
    stepsSlider_.setBounds(right.removeFromTop(22));
    controls.removeFromTop(4);
    auto row2 = controls.removeFromTop(24);
    useRecsToggle_.setBounds(row2.removeFromLeft(row2.getWidth() / 2));
    loudnessToggle_.setBounds(row2);
    controls.removeFromTop(6);
    auto row3 = controls.removeFromTop(30);
    runButton_.setBounds(row3.removeFromLeft(200));
    row3.removeFromLeft(6);
    if (cancelButton_.isVisible()) {
        cancelButton_.setBounds(row3.removeFromLeft(90));
        row3.removeFromLeft(6);
    }
    acceptButton_.setBounds(row3.removeFromRight(200));
    row3.removeFromRight(6);
    auditionButton_.setBounds(row3.removeFromRight(170));
    controls.removeFromTop(4);
    statusLabel_.setBounds(controls.removeFromTop(22));
    log_.setBounds(b.reduced(10, 4));
}

juce::var AutoMixPanel::findingsFromAnalysis() const
{
    if (!useRecsToggle_.getToggleState() || !controller_.hasAnalysis()) return {};
    juce::Array<juce::var> out;
    for (const auto& rec : controller_.currentAnalysis().recommendations) {
        if (rec.suggested_parameters.empty()) continue;
        juce::DynamicObject::Ptr f = new juce::DynamicObject();
        f->setProperty(apif::AgentFindingIn::id, juce::String(rec.id));
        f->setProperty(apif::AgentFindingIn::what, juce::String(rec.what));
        f->setProperty(apif::AgentFindingIn::severity, rec.severity);
        juce::DynamicObject::Ptr p = new juce::DynamicObject();
        for (const auto& [k, v] : rec.suggested_parameters)
            p->setProperty(juce::Identifier(juce::String(k)), v);
        f->setProperty(apif::AgentFindingIn::suggested_parameters, juce::var(p.get()));
        out.add(juce::var(f.get()));
        if (out.size() >= 24) break; // the service caps findings at 24
    }
    return out.isEmpty() ? juce::var() : juce::var(out);
}

void AutoMixPanel::setRunning(bool running)
{
    runButton_.setEnabled(!running);
    cancelButton_.setVisible(running);
    resized();
}

void AutoMixPanel::run()
{
    if (currentJobId_.isNotEmpty()) return;
    const auto* session = controller_.getActiveSession();
    if (session == nullptr || session->files.empty()) {
        statusLabel_.setText(TRANS("No active session audio."), juce::dontSendNotification);
        return;
    }
    ipc::HttpServiceBridge::AgentMixOptions opts;
    opts.filePath = controller_.renders().renderInputPath(); // WF-1: stack on the current version
    opts.sessionId = juce::String(session->id);
    opts.targetLufs = targetSlider_.getValue();
    opts.maxSteps = static_cast<int>(stepsSlider_.getValue());
    opts.includeLoudness = loudnessToggle_.getToggleState();
    opts.findings = findingsFromAnalysis();
    if (useRecsToggle_.getToggleState() && opts.findings.isVoid()) {
        statusLabel_.setText(TRANS("No recommendation with renderable parameters - measuring the mix instead."),
                             juce::dontSendNotification);
    }

    lastResult_ = juce::var();
    auditionButton_.setEnabled(false);
    acceptButton_.setEnabled(false);
    log_.setText({}, juce::dontSendNotification);
    jobSessionId_ = opts.sessionId;
    jobInputPath_ = opts.filePath;
    setRunning(true);
    statusLabel_.setText(TRANS("Starting..."), juce::dontSendNotification);

    juce::Component::SafePointer<AutoMixPanel> self(this);
    controller_.runLongInBackground([self, &work = controller_, opts, baseUrl = baseUrl_]() {
        ipc::HttpServiceBridge bridge(baseUrl);
        auto result = bridge.startAgentMixJob(opts);
        const bool started = result.is_ok();
        postIfCurrent(self, work, [started](AutoMixPanel& panel) {
            if (!started) return;
            panel.pollJob(panel.currentJobId_);
        }, [result = std::move(result)](AutoMixPanel& panel) {
            if (result.is_err()) {
                panel.setRunning(false);
                panel.statusLabel_.setText(TRANS("Error: ") + serviceErrorLine(result.error()),
                                           juce::dontSendNotification);
                return;
            }
            panel.currentJobId_ = result.value();
        }, [started](AutoMixPanel& panel) { // session switched mid-start
            if (!started) return;
            panel.cancelRun();
            panel.statusLabel_.setText(TRANS("Session changed - auto-mix cancelled."), juce::dontSendNotification);
        });
    });
}

void AutoMixPanel::pollJob(const juce::String& jobId)
{
    if (pollStop_) pollStop_->store(true);
    auto stop = std::make_shared<std::atomic<bool>>(false);
    pollStop_ = stop;
    juce::Component::SafePointer<AutoMixPanel> self(this);
    auto* controller = &controller_;
    controller_.runLongInBackground([self, jobId, stop, controller, baseUrl = baseUrl_]() {
        ipc::HttpServiceBridge bridge(baseUrl);
        using Poller = JobPoller<juce::var>;
        Poller::Hooks hooks;
        hooks.poll = [&bridge](const std::string& id) { return bridge.getAgentMixJobStatus(juce::String(id)); };
        hooks.cancel = [&bridge](const std::string& id) { bridge.cancelAgentMixJob(juce::String(id)); };
        hooks.isTerminal = [](const juce::var& st) {
            return !api::isActiveStatus(st[apif::AgentMixStatusResponse::status].toString());
        };
        hooks.onProgress = [self, jobId](const juce::var& st) {
            juce::MessageManager::callAsync([self, jobId, st]() {
                if (self == nullptr || jobId != self->currentJobId_) return;
                self->onProgress(st);
            });
        };
        const uint64_t epoch = controller->sessionEpoch();
        hooks.shouldStop = [stop, controller, epoch] { return stop->load() || controller->sessionEpoch() != epoch; };
        Poller::Options opts;
        opts.pollIntervalMs = 750;
        opts.deadlineSeconds = 45 * 60;
        const auto out = Poller::run(hooks, opts, jobId.toStdString());
        juce::MessageManager::callAsync([self, jobId, out, epoch]() {
            if (self == nullptr || jobId != self->currentJobId_) return;
            if (self->controller_.sessionEpoch() != epoch) {
                self->currentJobId_ = {};
                self->setRunning(false);
                self->statusLabel_.setText(TRANS("Session changed - auto-mix cancelled."), juce::dontSendNotification);
                return;
            }
            if (!out.finished() || !out.status.has_value()) {
                self->currentJobId_ = {};
                self->setRunning(false);
                self->statusLabel_.setText(TRANS("Auto-mix stopped: ") + out.describe(), juce::dontSendNotification);
                return;
            }
            self->onFinished(*out.status);
        });
    });
}

void AutoMixPanel::cancelRun()
{
    if (currentJobId_.isEmpty()) return;
    if (pollStop_) pollStop_->store(true);
    const juce::String jobId = currentJobId_;
    currentJobId_ = {};
    setRunning(false);
    statusLabel_.setText(TRANS("Cancelled. Nothing was accepted."), juce::dontSendNotification);
    controller_.runLongInBackground([jobId, baseUrl = baseUrl_]() {
        ipc::HttpServiceBridge bridge(baseUrl);
        bridge.cancelAgentMixJob(jobId);
    });
}

void AutoMixPanel::onProgress(const juce::var& status)
{
    const juce::String stage = status[apif::AgentMixStatusResponse::stage].toString();
    statusLabel_.setText(TRANS("Running") + (stage.isNotEmpty() ? " - " + stage : juce::String("...")),
                         juce::dontSendNotification);
    const juce::String text = formatStepLog(status);
    if (text != log_.getText()) {
        log_.setText(text, juce::dontSendNotification);
        log_.moveCaretToEnd();
    }
}

void AutoMixPanel::onFinished(const juce::var& status)
{
    currentJobId_ = {};
    setRunning(false);
    const juce::String st = status[apif::AgentMixStatusResponse::status].toString();
    if (st == api::job_status::cancelled) {
        statusLabel_.setText(TRANS("Cancelled. Kept renders so far stay on disk; nothing was accepted."),
                             juce::dontSendNotification);
        return;
    }
    if (st != api::job_status::completed) {
        const juce::String err = status[apif::AgentMixStatusResponse::error].toString();
        statusLabel_.setText(TRANS("Error: ") + (err.isNotEmpty() ? err : TRANS("auto-mix failed")),
                             juce::dontSendNotification);
        return;
    }
    log_.setText(formatStepLog(status), juce::dontSendNotification);
    lastResult_ = status[apif::AgentMixStatusResponse::result];
    const bool changed = static_cast<bool>(lastResult_[apif::AgentMixResult::changed]);
    statusLabel_.setText(lastResult_[apif::AgentMixResult::summary].toString() +
                             (changed ? " " + TRANS("Audition it, then accept or ignore it.") : juce::String()),
                         juce::dontSendNotification);
    auditionButton_.setEnabled(changed);
    acceptButton_.setEnabled(changed);
    if (changed) auditionResult();
}

void AutoMixPanel::auditionResult()
{
    const juce::String path = lastResult_[apif::AgentMixResult::final_path].toString();
    if (path.isEmpty() || !juce::File(path).existsAsFile()) return;
    const auto* session = controller_.getActiveSession();
    if (session == nullptr || juce::String(session->id) != jobSessionId_) {
        statusLabel_.setText(TRANS("This result belongs to another session."), juce::dontSendNotification);
        return;
    }
    controller_.setAbProcessedPath(path); // loudness-matched B side
}

void AutoMixPanel::acceptResult()
{
    auto* kept = lastResult_[apif::AgentMixResult::kept_paths].getArray();
    if (kept == nullptr || kept->isEmpty()) return;
    // Each kept step was rendered from the one before it: the first from
    // what the run processed (WF-1), the rest as derived nodes, so the tree
    // shows the real chain.
    bool first = true;
    for (const auto& p : *kept) {
        const juce::String path = p.toString();
        if (first)
            controller_.renders().registerRenderOf(path, jobInputPath_, jobSessionId_);
        else
            controller_.registerDerivedRender(path, jobSessionId_);
        first = false;
    }
    acceptButton_.setEnabled(false);
    statusLabel_.setText(TRANS("Accepted: %n step(s) added to the render history.").replace("%n",
                                                                                            juce::String(kept->size())),
                         juce::dontSendNotification);
}

} // namespace khensu
