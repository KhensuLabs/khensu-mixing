// desktop-app/Source/ui/Copilot/SectionIntensityPanel.cpp
#include "SectionIntensityPanel.h"
#include <khensu/ipc/generated/ApiJobStatus.h>
#include "../shared/GuardrailText.h"
#include "../shared/JobPoller.h"
#include "../shared/SessionAudioPaths.h"
#include "../shared/KhensuTheme.h"
#include "../shared/RenderPaths.h"
#include "../shared/ServiceErrorHint.h"
#include <khensu/session/Session.h>
#include <map>
#include <thread>

namespace khensu {

namespace {
constexpr int kRowH = 44;
constexpr int kRowGap = 6;

juce::String formatTime(float seconds)
{
    const int m = (int)(seconds / 60.0f);
    const int s = (int)std::fmod(seconds, 60.0f);
    return juce::String(m).paddedLeft('0', 2) + ":" + juce::String(s).paddedLeft('0', 2);
}


} // anonymous namespace

// Every cached palette colour on this panel, in one place (UI-01).
void SectionIntensityPanel::applyThemeColours()
{
    headerLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentTeal);
    for (auto* l : {&baseIntensityLabel_, &statusLabel_, &guardrailLabel_})
        l->setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    applyButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal);
    applyButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::bgVoid);
    cancelButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentRed.darker(0.6f));
    // Rows are built at runtime: walk them so a theme switch reaches them (UIX-22).
    for (auto& row : rows_)
        row->label.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
}

SectionIntensityPanel::SectionIntensityPanel(IAppController& c, std::string baseUrl)
    : controller_(c), baseUrl_(std::move(baseUrl))
{
    applyThemeColours();
    headerLabel_.setFont(juce::Font(juce::FontOptions{15.f, juce::Font::bold}));
    headerLabel_.setText(TRANS("  Section-Aware Processing - vary intensity per section"), juce::dontSendNotification);
    addAndMakeVisible(headerLabel_);

    baseIntensityLabel_.setFont(juce::Font(juce::FontOptions{11.f}));
    addAndMakeVisible(baseIntensityLabel_);

    baseIntensitySlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    baseIntensitySlider_.setRange(0.0, 1.0, 0.01);
    baseIntensitySlider_.setValue(0.6, juce::dontSendNotification);
    baseIntensitySlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 20);
    addAndMakeVisible(baseIntensitySlider_);

    applyButton_.onClick = [this] { apply(); };
    // A section-aware render is meaningless until analysis supplies at least
    // one section. Start disabled so the precondition is visible even before
    // the first visibility-driven refresh.
    applyButton_.setEnabled(false);
    addAndMakeVisible(applyButton_);

    cancelButton_.onClick = [this] { cancelJob(); };
    addAndMakeVisible(cancelButton_);
    cancelButton_.setVisible(false);

    statusLabel_.setFont(juce::Font(juce::FontOptions{12.f}));
    addAndMakeVisible(statusLabel_);
    guardrailLabel_.setFont(juce::Font(juce::FontOptions{10.f}));
    guardrailLabel_.setJustificationType(juce::Justification::centredLeft);
    guardrailLabel_.setMinimumHorizontalScale(0.65f);
    addAndMakeVisible(guardrailLabel_);

    rowsViewport_.setViewedComponent(&rowsContent_, false);
    rowsViewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(rowsViewport_);
}

SectionIntensityPanel::~SectionIntensityPanel()
{
    if (pollStop_) pollStop_->store(true); // the poller cancels the job server-side
}

void SectionIntensityPanel::visibilityChanged()
{
    if (isVisible()) refreshSections();
}

void SectionIntensityPanel::refreshSections()
{
    const bool jobRunning = currentJobId_.isNotEmpty();
    if (!controller_.hasAnalysis() || !controller_.currentAnalysis().section_analysis.has_value()) {
        rows_.clear();
        rowsContent_.removeAllChildren();
        rowsKey_ = {};
        applyButton_.setEnabled(false);
        if (!jobRunning)
            statusLabel_.setText(TRANS("No section analysis available for the current track."),
                                 juce::dontSendNotification);
        layoutRows();
        return;
    }

    const auto& sections = controller_.currentAnalysis().section_analysis->sections;
    juce::String key = controller_.getActiveSession() != nullptr ? juce::String(controller_.getActiveSession()->id)
                                                                  : juce::String();
    for (const auto& sec : sections)
        key << "|" << juce::String(sec.label) << "@" << juce::String(sec.start_sec, 2) << "-"
            << juce::String(sec.end_sec, 2);
    if (key == rowsKey_ && !rows_.empty()) {
        // Same sections: keep the user's sliders, and never re-enable Apply
        // while a render is still running (UIX-19).
        applyButton_.setEnabled(!jobRunning);
        layoutRows();
        return;
    }
    rowsKey_ = key;
    rows_.clear();
    rowsContent_.removeAllChildren();
    applyButton_.setEnabled(false);

    for (const auto& sec : sections) {
        auto row = std::make_unique<SectionRow>();
        row->label.setText(juce::String(sec.label) + "  (" + formatTime(sec.start_sec) + "-" + formatTime(sec.end_sec) +
                               ")",
                           juce::dontSendNotification);
        row->label.setFont(juce::Font(juce::FontOptions{11.f}));
        row->label.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
        rowsContent_.addAndMakeVisible(row->label);

        row->intensitySlider.setSliderStyle(juce::Slider::LinearHorizontal);
        row->intensitySlider.setRange(0.0, 1.0, 0.01);
        row->intensitySlider.setValue(1.0, juce::dontSendNotification);
        row->intensitySlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 18);
        rowsContent_.addAndMakeVisible(row->intensitySlider);

        rows_.push_back(std::move(row));
    }

    if (rows_.empty()) {
        statusLabel_.setText(TRANS("No sections detected; section-aware processing is unavailable."),
                             juce::dontSendNotification);
    } else {
        applyButton_.setEnabled(!jobRunning);
        if (!jobRunning)
            statusLabel_.setText(TRANS("%n sections detected. Lower a section's slider to reduce processing there "
                                       "(0 = untouched).")
                                     .replace("%n", juce::String((int)rows_.size())),
                                 juce::dontSendNotification);
    }
    layoutRows();
}

void SectionIntensityPanel::layoutRows()
{
    const int vw = rowsViewport_.getWidth();
    const int n = static_cast<int>(rows_.size());
    int totalH = n * (kRowH + kRowGap) + kRowGap;
    rowsContent_.setBounds(0, 0, vw, juce::jmax(totalH, rowsViewport_.getHeight()));

    int y = kRowGap;
    for (auto& row : rows_) {
        auto b = juce::Rectangle<int>(kRowGap, y, vw - kRowGap * 2, kRowH);
        row->label.setBounds(b.removeFromTop(18));
        row->intensitySlider.setBounds(b.removeFromTop(22));
        y += kRowH + kRowGap;
    }
}

void SectionIntensityPanel::apply()
{
    if (rows_.empty()) {
        statusLabel_.setText(TRANS("No sections detected; section-aware processing is unavailable."),
                             juce::dontSendNotification);
        return;
    }
    if (!currentJobId_.isEmpty()) return;
    const auto* session = controller_.getActiveSession();
    if (session == nullptr || session->files.empty()) {
        statusLabel_.setText(TRANS("No active session audio."), juce::dontSendNotification);
        return;
    }
    if (!controller_.hasAnalysis() || !controller_.currentAnalysis().section_analysis.has_value()) return;
    if (!controller_.analysis().genreRelativeActionAllowed()) {
        statusLabel_.setText(TRANS("Section processing abstained: ") +
                                 controller_.analysis().genreRelativeAbstainedReason(),
                             juce::dontSendNotification);
        return;
    }

    const auto& sections = controller_.currentAnalysis().section_analysis->sections;
    const double baseIntensity = baseIntensitySlider_.getValue();

    std::vector<ipc::HttpServiceBridge::SectionChainSpec> specs;
    for (size_t i = 0; i < sections.size() && i < rows_.size(); ++i) {
        ipc::HttpServiceBridge::SectionChainSpec spec;
        spec.label = juce::String(sections[i].label);
        spec.startSec = sections[i].start_sec;
        spec.endSec = sections[i].end_sec;

        const double effective = baseIntensity * rows_[i]->intensitySlider.getValue();
        if (effective >= 0.02) {
            ipc::HttpServiceBridge::EffectSpec comp;
            comp.effect = "Compressor";
            comp.parameters["threshold_db"] = -18.0f;
            comp.parameters["ratio"] = static_cast<float>(1.0 + 5.0 * effective);
            spec.effects.push_back(comp);

            ipc::HttpServiceBridge::EffectSpec gain;
            gain.effect = "Gain";
            gain.parameters["gain_db"] = static_cast<float>(4.0 * effective);
            spec.effects.push_back(gain);
        }
        specs.push_back(std::move(spec));
    }

    // WF-1: process the current version (fixes stack) unless the user chose
    // the original; the output is named after the original either way.
    const juce::File src(controller_.renders().renderInputPath());
    const juce::File out = uniqueRenderFile(juce::File(juce::String(session->files[0].path)), "_sectioned");
    juce::String sessionId(session->id);
    jobSessionId_ = sessionId;
    jobInputPath_ = src.getFullPathName();

    applyButton_.setEnabled(false);
    cancelButton_.setVisible(true);
    guardrailLabel_.setText({}, juce::dontSendNotification);
    resized();
    statusLabel_.setText(TRANS("Rendering section-aware chain..."), juce::dontSendNotification);

    juce::Component::SafePointer<SectionIntensityPanel> self(this);
    const juce::String inPath = src.getFullPathName(), outPath = out.getFullPathName();
    const juce::String abstainedReason = controller_.analysis().genreRelativeAbstainedReason(); // "" when allowed
    const auto stemPaths = session_audio::activeStemPaths(controller_);
    controller_.runLongInBackground([self, inPath, outPath, specs, sessionId, abstainedReason, stemPaths, baseUrl = baseUrl_]() mutable {
        ipc::HttpServiceBridge bridge(baseUrl);
        auto result = bridge.startSectionChainJob(inPath, outPath, specs, 30.0, sessionId, "section_aware", true, true,
                                                  abstainedReason, stemPaths);
        juce::MessageManager::callAsync([self, result = std::move(result)]() mutable {
            if (!self) return;
            if (result.is_err()) {
                self->applyButton_.setEnabled(true);
                self->cancelButton_.setVisible(false);
                self->resized();
                const auto& error = result.error();
                const juce::String detail = serviceErrorDetail(error);
                self->statusLabel_.setText(isAbstainedRenderError(error)
                                               ? TRANS("Section processing abstained: ")
                                                     + (detail.isNotEmpty() ? detail : juce::String(error.message))
                                               : TRANS("Error: ") + serviceErrorLine(error),
                                           juce::dontSendNotification);
                return;
            }
            self->currentJobId_ = result.value();
            if (self->jobSessionChanged()) { // session switched while /start was in flight
                self->cancelJob();
                self->statusLabel_.setText(TRANS("Session changed - section render cancelled."),
                                           juce::dontSendNotification);
                return;
            }
            self->pollJob(self->currentJobId_);
        });
    });
}

bool SectionIntensityPanel::jobSessionChanged() const
{
    const auto* s = controller_.getActiveSession();
    return s == nullptr || juce::String(s->id) != jobSessionId_;
}

void SectionIntensityPanel::pollJob(const juce::String& jobId)
{
    if (pollStop_) pollStop_->store(true);
    auto stop = std::make_shared<std::atomic<bool>>(false);
    pollStop_ = stop;
    juce::Component::SafePointer<SectionIntensityPanel> self(this);
    auto* controller = &controller_;
    const uint64_t epoch = controller_.sessionEpoch();
    controller_.runLongInBackground([self, jobId, stop, controller, epoch, baseUrl = baseUrl_]() {
        ipc::HttpServiceBridge bridge(baseUrl);
        using Poller = JobPoller<ipc::HttpServiceBridge::SectionChainJobStatus>;
        Poller::Hooks hooks;
        hooks.poll = [&bridge](const std::string& id) { return bridge.getSectionChainJobStatus(juce::String(id)); };
        hooks.cancel = [&bridge](const std::string& id) { bridge.cancelSectionChainJob(juce::String(id)); };
        hooks.isTerminal = [](const auto& st) { return !khensu::api::isActiveStatus(st.status); };
        hooks.onProgress = [self, jobId](const auto& st) {
            juce::MessageManager::callAsync([self, jobId, status = st.status]() {
                if (!self || jobId != self->currentJobId_) return;
                self->statusLabel_.setText(TRANS("Rendering... (%s)").replace("%s", status),
                                           juce::dontSendNotification);
            });
        };
        // AUDIT-002: a session switch cancels the render; it must never be
        // registered into another song.
        hooks.shouldStop = [stop, controller, epoch] { return stop->load() || controller->sessionEpoch() != epoch; };
        Poller::Options opts;
        opts.pollIntervalMs = 750;
        opts.deadlineSeconds = 30 * 60;
        const auto out = Poller::run(hooks, opts, jobId.toStdString());

        juce::MessageManager::callAsync([self, jobId, out]() {
            if (!self || jobId != self->currentJobId_) return;
            if (self->jobSessionChanged()) {
                self->cancelJob();
                self->statusLabel_.setText(TRANS("Session changed - section render cancelled."),
                                           juce::dontSendNotification);
                return;
            }
            self->currentJobId_ = {};
            self->applyButton_.setEnabled(!self->rows_.empty());
            self->cancelButton_.setVisible(false);
            self->resized();
            if (!out.finished() || !out.status.has_value()) {
                self->statusLabel_.setText(TRANS("Section render stopped: ") + out.describe(),
                                           juce::dontSendNotification);
                return;
            }
            const auto& status = *out.status;
            if (status.status == khensu::api::job_status::completed) {
                const bool guardrailPassed = guardrail_text::passed(status.guardrail);
                self->statusLabel_.setText(TRANS("Done: %n sections rendered.")
                                                   .replace("%n", juce::String((int)status.sectionReports.size()))
                                               + " " + guardrail_text::summary(status.guardrail),
                                           juce::dontSendNotification);
                self->guardrailLabel_.setText(guardrail_text::measurements(status.guardrail), juce::dontSendNotification);
                if (guardrailPassed && !status.outputPath.isEmpty())
                    self->controller_.renders().registerRenderOf(status.outputPath, self->jobInputPath_,
                                                                 self->jobSessionId_);
            } else if (status.status == khensu::api::job_status::cancelled) {
                self->statusLabel_.setText(TRANS("Cancelled."), juce::dontSendNotification);
            } else {
                self->statusLabel_.setText(
                    TRANS("Error: ") + (status.error.isNotEmpty() ? status.error : TRANS("render failed")),
                    juce::dontSendNotification);
            }
        });
    });
}

void SectionIntensityPanel::cancelJob()
{
    if (currentJobId_.isEmpty()) return;
    if (pollStop_) pollStop_->store(true);
    juce::String jobId = currentJobId_;
    currentJobId_ = {};
    applyButton_.setEnabled(!rows_.empty());
    cancelButton_.setVisible(false);
    resized();
    statusLabel_.setText(TRANS("Cancelled."), juce::dontSendNotification);

    controller_.runLongInBackground([jobId, baseUrl = baseUrl_]() mutable {
        ipc::HttpServiceBridge bridge(baseUrl);
        bridge.cancelSectionChainJob(jobId);
    });
}

void SectionIntensityPanel::paint(juce::Graphics& g) { g.fillAll(KhensuTheme::bgBase); }

void SectionIntensityPanel::resized()
{
    auto b = getLocalBounds();
    headerLabel_.setBounds(b.removeFromTop(40));
    auto controls = b.removeFromTop(100).reduced(10);

    auto row1 = controls.removeFromTop(36);
    baseIntensityLabel_.setBounds(row1.removeFromLeft(200));
    baseIntensitySlider_.setBounds(row1);

    controls.removeFromTop(4);
    auto row2 = controls.removeFromTop(26);
    if (cancelButton_.isVisible()) {
        cancelButton_.setBounds(row2.removeFromRight(80));
        row2.removeFromRight(6);
    }
    applyButton_.setBounds(row2.removeFromLeft(220));
    row2.removeFromLeft(10);
    statusLabel_.setBounds(row2);
    guardrailLabel_.setBounds(controls.removeFromTop(24));

    rowsViewport_.setBounds(b.reduced(10, 0));
    layoutRows();
}

} // namespace khensu
