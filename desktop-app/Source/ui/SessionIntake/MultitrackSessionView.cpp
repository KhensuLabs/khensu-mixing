// desktop-app/Source/ui/SessionIntake/MultitrackSessionView.cpp
#include "MultitrackSessionView.h"
#include "../shared/PanelRunState.h"
#include "../../IBackgroundWork.h"
#include "../../PythonServiceManager.h"
#include "../shared/BackgroundStop.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/ServiceErrorHint.h"
#include "../shared/SessionEpoch.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiJobStatus.h>

namespace khensu {

namespace {

enum Column { ColTrack = 1, ColRole, ColLufs, ColPeak, ColWidth, ColIssues };

juce::String fmtDb(const std::optional<double>& v, const char* unit)
{
    return v.has_value() ? juce::String(*v, 1) + " " + unit : juce::String("-");
}

} // namespace

// ---------------------------------------------------------------------------
// MultitrackSessionView
// ---------------------------------------------------------------------------

void MultitrackSessionView::applyThemeColours()
{
    titleLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    mismatchLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentAmber);
    matrixLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    trackTable_.setColour(juce::ListBox::backgroundColourId, KhensuTheme::bgSurface);
    trackTable_.setColour(juce::ListBox::outlineColourId, KhensuTheme::borderSubtle);
    trackTable_.getHeader().setColour(juce::TableHeaderComponent::backgroundColourId, KhensuTheme::bgRaised);
    trackTable_.getHeader().setColour(juce::TableHeaderComponent::textColourId, KhensuTheme::textSecondary);
    adviceEditor_.setColour(juce::TextEditor::backgroundColourId, KhensuTheme::bgSurface);
    adviceEditor_.setColour(juce::TextEditor::textColourId, KhensuTheme::textPrimary);
    adviceEditor_.setColour(juce::TextEditor::outlineColourId, KhensuTheme::borderSubtle);
    for (auto* b : {&analyzeButton_, &createButton_}) {
        b->setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
        b->setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    }
    for (auto* b : {&cancelButton_, &closeButton_}) {
        b->setColour(juce::TextButton::buttonColourId, KhensuTheme::bgOverlay);
        b->setColour(juce::TextButton::textColourOffId, KhensuTheme::textSecondary);
    }
}

MultitrackSessionView::MultitrackSessionView(IBackgroundWork& controller, const juce::File& folder)
    : controller_(controller), folder_(folder)
{
    applyThemeColours();

    titleLabel_.setText(TRANS("Multitrack session") + ": " + folder.getFileName(), juce::dontSendNotification);
    titleLabel_.setFont(KhensuTheme::labelFont(type::title));
    addAndMakeVisible(titleLabel_);

    statusLabel_.setFont(KhensuTheme::bodyFont());
    addAndMakeVisible(statusLabel_);
    mismatchLabel_.setFont(KhensuTheme::bodyFont(KhensuTheme::kFontLabel));
    mismatchLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(mismatchLabel_);
    matrixLabel_.setText(TRANS("Track-vs-track masking"), juce::dontSendNotification);
    matrixLabel_.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
    addAndMakeVisible(matrixLabel_);

    auto& header = trackTable_.getHeader();
    header.addColumn(TRANS("Track"), ColTrack, 150, 60);
    header.addColumn(TRANS("Role"), ColRole, 100, 50);
    header.addColumn(TRANS("Loudness"), ColLufs, 80, 50);
    header.addColumn(TRANS("True peak"), ColPeak, 80, 50);
    header.addColumn(TRANS("Width"), ColWidth, 60, 40);
    header.addColumn(TRANS("Issues"), ColIssues, 140, 60);
    trackTable_.setModel(this);
    trackTable_.setRowHeight(24);
    trackTable_.setTitle(TRANS("Stem tracks"));
    trackTable_.setTooltip(TRANS("One row per stem: its name, role and key measurements. Select a row for advice."));
    addAndMakeVisible(trackTable_);

    adviceEditor_.setMultiLine(true);
    adviceEditor_.setReadOnly(true);
    adviceEditor_.setScrollbarsShown(true);
    adviceEditor_.setFont(KhensuTheme::bodyFont());
    adviceEditor_.setTitle(TRANS("Track advice"));
    adviceEditor_.setTooltip(TRANS("Advice for the selected track."));
    addAndMakeVisible(adviceEditor_);

    matrix_.onTrackClicked = [this](int row) { trackTable_.selectRow(row); };
    addAndMakeVisible(matrix_);

    analyzeButton_.setTooltip(TRANS("Measure every stem, score masking between tracks and build the mixdown."));
    analyzeButton_.onClick = [this] { startAnalysis(); };
    cancelButton_.setTooltip(TRANS("Stop the running analysis."));
    cancelButton_.onClick = [this] { cancelAnalysis(); };
    createButton_.setTooltip(TRANS("Create a session from the mixdown and the named stems."));
    createButton_.onClick = [this] {
        if (!analysis_ || !onCreateSession) return;
        onCreateSession(folder_.getFileName() + " " + TRANS("Multitrack"), multitrack::buildSessionFiles(*analysis_));
    };
    closeButton_.onClick = [this] {
        if (onClose) onClose();
    };
    for (auto* b : {&analyzeButton_, &cancelButton_, &createButton_, &closeButton_})
        addAndMakeVisible(b);

    updateButtons();
    startScan();
}

MultitrackSessionView::~MultitrackSessionView()
{
    alive_->store(false);
    if (running_ && jobId_.isNotEmpty()) {
        const auto jobId = jobId_;
        controller_.runInBackground([jobId] { ipc::HttpServiceBridge(PythonServiceManager::kBaseUrl).cancelMultitrackJob(jobId); });
    }
    trackTable_.setModel(nullptr);
}

void MultitrackSessionView::setStatus(const juce::String& text, bool isError)
{
    statusLabel_.setText(text, juce::dontSendNotification);
    // Transient state tint: re-resolved on the next update, not on theme change.
    statusLabel_.setColour(juce::Label::textColourId, isError ? KhensuTheme::accentRed : KhensuTheme::textSecondary);
}

void MultitrackSessionView::updateButtons()
{
    analyzeButton_.setEnabled(scan_.has_value() && !running_);
    cancelButton_.setEnabled(running_);
    createButton_.setEnabled(analysis_.has_value() && !running_ && !analysis_->tracks.empty());
}

void MultitrackSessionView::startScan()
{
    setStatus(TRANS("Reading the stem folder..."));
    juce::Component::SafePointer<MultitrackSessionView> self(this);
    const auto folder = folder_.getFullPathName();
    controller_.runInBackground([self, &work = controller_, folder] {
        auto response = ipc::HttpServiceBridge(PythonServiceManager::kBaseUrl, 60000).scanMultitrackFolder(folder);
        postIfCurrent(self, work, [response](MultitrackSessionView& panel) {
            if (response.is_err()) {
                panel.setStatus(TRANS("Could not read the stem folder") + ": " + serviceErrorLine(response.error()), true);
                panel.updateButtons();
                return;
            }
            auto parsed = multitrack::parseScan(response.value());
            if (parsed.is_err()) {
                panel.setStatus(TRANS("Unexpected scan response") + ": " + juce::String(parsed.error()), true);
                panel.updateButtons();
                return;
            }
            panel.showScan(parsed.value());
        });
    });
}

void MultitrackSessionView::showScan(const multitrack::Scan& scan)
{
    scan_ = scan;
    analysis_.reset();
    matrix_.setAnalysis(nullptr);
    setStatus(juce::String((int)scan.files.size()) + " " + TRANS("stems found at") + " " +
              juce::String(scan.sessionSampleRate) + " Hz. " + TRANS("Analyze them to see masking and advice."));
    juce::StringArray lines;
    for (const auto& m : scan.mismatches)
        lines.add(m.track + ": " + m.detail);
    mismatchLabel_.setText(lines.joinIntoString("\n"), juce::dontSendNotification);
    trackTable_.updateContent();
    trackTable_.repaint();
    updateButtons();
    resized();
}

void MultitrackSessionView::startAnalysis()
{
    if (!scan_ || running_) return;
    juce::StringArray names;
    for (const auto& f : scan_->files)
        names.add(f.trackName);
    running_ = true;
    updateButtons();
    setStatus(TRANS("Starting the multitrack analysis..."));
    juce::Component::SafePointer<MultitrackSessionView> self(this);
    const auto folder = folder_.getFullPathName();
    controller_.runInBackground([self, &work = controller_, folder, names] {
        auto started = ipc::HttpServiceBridge(PythonServiceManager::kBaseUrl, 60000).startMultitrackJob(folder, names);
        postIfCurrent(self, work, [started](MultitrackSessionView& panel) {
            if (started.is_err()) return;
            panel.jobId_ = started.value();
            panel.pollJob(panel.jobId_);
        }, [started](MultitrackSessionView& panel) {
            if (started.is_err()) {
                panel.running_ = false;
                panel.updateButtons();
                panel.setStatus(TRANS("Could not start the analysis") + ": " + serviceErrorLine(started.error()), true);
            }
        }, [ok = started.is_ok()](MultitrackSessionView& panel) {
            if (!ok) return;
            panel.running_ = false;
            panel.updateButtons();
        });
    });
}

void MultitrackSessionView::pollJob(const juce::String& jobId)
{
    juce::Component::SafePointer<MultitrackSessionView> self(this);
    auto alive = alive_;
    controller_.runLongInBackground([self, alive, jobId, &controller = controller_] {
        ipc::HttpServiceBridge bridge(PythonServiceManager::kBaseUrl, 10000);
        int failures = 0;
        while (alive->load() && !controller.isShuttingDown() && !backgroundShouldStop()) {
            auto response = bridge.getMultitrackJobStatus(jobId);
            if (response.is_err()) {
                if (++failures >= 5 || response.error().kind == ipc::BridgeError::Kind::HttpError) {
                    juce::MessageManager::callAsync([self, err = response.error()] {
                        if (self == nullptr) return;
                        self->running_ = false;
                        self->updateButtons();
                        self->setStatus(TRANS("Lost track of the analysis") + ": " + serviceErrorLine(err), true);
                    });
                    return;
                }
            } else {
                failures = 0;
                auto parsed = multitrack::parseJobStatus(response.value());
                const bool terminal = parsed.is_ok() && parsed.value().isTerminal();
                postIfCurrent(self, controller, [parsed](MultitrackSessionView& panel) {
                    // Progress and failures were shown by the settle step;
                    // only a finished job's result is session-scoped.
                    if (parsed.is_err() || !parsed.value().isTerminal()) return;
                    const auto& s = parsed.value();
                    if (s.isCompleted() && s.result.has_value())
                        panel.showAnalysis(*s.result);
                    else
                        panel.setStatus(TRANS("Analysis ended") + " (" + s.status + ")" +
                                            (s.error.isNotEmpty() ? ": " + s.error : juce::String()),
                                        s.status != khensu::api::job_status::cancelled); // WIRE-10: the generated constant
                }, [parsed](MultitrackSessionView& panel) {
                    if (parsed.is_err()) {
                        panel.running_ = false;
                        panel.updateButtons();
                        panel.setStatus(TRANS("Unexpected status response") + ": " + juce::String(parsed.error()), true);
                        return;
                    }
                    const auto& s = parsed.value();
                    if (!s.isTerminal()) {
                        panel.setStatus(TRANS("Analyzing") + " " + juce::String(s.done) + "/" + juce::String(s.total) +
                                        (s.current.isNotEmpty() ? " - " + s.current : juce::String()));
                        return;
                    }
                    panel.running_ = false;
                    panel.updateButtons();
                });
                if (parsed.is_err() || terminal) return;
            }
            if (!backgroundSleep(500)) return;
        }
    });
}

void MultitrackSessionView::cancelAnalysis()
{
    if (!running_ || jobId_.isEmpty()) return;
    const auto jobId = jobId_;
    setStatus(TRANS("Cancelling..."));
    controller_.runInBackground([jobId] { ipc::HttpServiceBridge(PythonServiceManager::kBaseUrl).cancelMultitrackJob(jobId); });
}

void MultitrackSessionView::showAnalysis(multitrack::Analysis analysis)
{
    analysis_ = std::move(analysis);
    matrix_.setAnalysis(&*analysis_);
    int severe = 0, moderate = 0;
    for (const auto& t : analysis_->tracks) {
        severe += t.adviceCount("severe");
        moderate += t.adviceCount("moderate");
    }
    juce::String text = juce::String((int)analysis_->tracks.size()) + " " + TRANS("tracks analyzed") + ". ";
    if (analysis_->mix.integratedLufs) text += TRANS("Mix") + " " + fmtDb(analysis_->mix.integratedLufs, "LUFS") + ". ";
    if (!analysis_->mix.headroomOk) text += TRANS("The summed stems clip - lower the faders before mixing.") + " ";
    text += juce::String(severe) + " " + TRANS("severe") + ", " + juce::String(moderate) + " " + TRANS("moderate");
    if (!analysis_->pairs.empty() && analysis_->pairs.front().riskLevel != "low")
        text += ". " + TRANS("Worst masking") + ": " + analysis_->pairs.front().summary;
    setStatus(text);

    juce::StringArray lines;
    for (const auto& m : analysis_->mismatches)
        lines.add(m.track + ": " + m.detail);
    mismatchLabel_.setText(lines.joinIntoString("\n"), juce::dontSendNotification);
    trackTable_.updateContent();
    trackTable_.repaint();
    if (!analysis_->tracks.empty()) trackTable_.selectRow(0);
    updateButtons();
    resized();
}

void MultitrackSessionView::showAdviceFor(int row)
{
    matrix_.setHighlightedTrack(row);
    if (!analysis_ || row < 0 || row >= (int)analysis_->tracks.size()) {
        adviceEditor_.setText({}, false);
        return;
    }
    const auto& t = analysis_->tracks[(size_t)row];
    juce::String text = t.name + " (" + t.roleLabel + ", " + TRANS("role from") + " " + t.roleSource + ")\n";
    text += TRANS("Loudness") + " " + fmtDb(t.integratedLufs, "LUFS") + ", " + TRANS("true peak") + " " +
            fmtDb(t.truePeakDbtp, "dBTP") + ", " + TRANS("width") + " " + juce::String(t.stereoWidth, 2) + "\n\n";
    if (t.advice.empty()) text += TRANS("No issues found for this track.");
    for (const auto& a : t.advice) {
        text += "[" + a.severity + "] " + a.what + "\n";
        if (a.suggestedAction.isNotEmpty()) text += "  " + TRANS("Try") + ": " + a.suggestedAction + "\n";
        if (a.whyHelps.isNotEmpty()) text += "  " + TRANS("Why") + ": " + a.whyHelps + "\n";
        text += "\n";
    }
    adviceEditor_.setText(text, false);
}

int MultitrackSessionView::getNumRows()
{
    if (analysis_) return (int)analysis_->tracks.size();
    return scan_ ? (int)scan_->files.size() : 0;
}

void MultitrackSessionView::paintRowBackground(juce::Graphics& g, int row, int, int, bool selected)
{
    if (selected)
        g.fillAll(KhensuTheme::accentTealDim);
    else if (row % 2 == 1)
        g.fillAll(KhensuTheme::bgRaised.withAlpha(0.4f));
}

void MultitrackSessionView::paintCell(juce::Graphics& g, int row, int columnId, int w, int h, bool)
{
    juce::String text;
    juce::Colour colour = KhensuTheme::textPrimary;
    if (analysis_ && row < (int)analysis_->tracks.size()) {
        const auto& t = analysis_->tracks[(size_t)row];
        switch (columnId) {
        case ColTrack: text = t.name; break;
        case ColRole:
            text = t.roleLabel;
            if (t.roleSource != "name") colour = KhensuTheme::textTertiary; // guessed from the spectrum
            break;
        case ColLufs: text = t.silent ? TRANS("silent") : fmtDb(t.integratedLufs, "LUFS"); break;
        case ColPeak:
            text = fmtDb(t.truePeakDbtp, "dBTP");
            if (t.truePeakDbtp && *t.truePeakDbtp >= 0.0) colour = KhensuTheme::accentRed;
            break;
        case ColWidth: text = t.channels == 1 ? TRANS("mono") : juce::String(t.stereoWidth, 2); break;
        case ColIssues: {
            const int severe = t.adviceCount("severe"), moderate = t.adviceCount("moderate");
            if (severe + moderate == 0) {
                text = TRANS("none");
                colour = KhensuTheme::accentGreen;
            } else {
                text = juce::String(severe) + " " + TRANS("severe") + ", " + juce::String(moderate) + " " + TRANS("moderate");
                colour = severe > 0 ? KhensuTheme::accentRed : KhensuTheme::accentAmber;
            }
            break;
        }
        default: break;
        }
    } else if (scan_ && row < (int)scan_->files.size()) {
        const auto& f = scan_->files[(size_t)row];
        switch (columnId) {
        case ColTrack: text = f.trackName; break;
        case ColRole: text = f.roleSource == "name" ? f.roleLabel : TRANS("(after analysis)"); break;
        case ColLufs: text = juce::String(f.sampleRate) + " Hz"; break;
        case ColPeak: text = juce::String(f.durationSec, 1) + " s"; break;
        case ColWidth: text = f.channels == 1 ? TRANS("mono") : TRANS("stereo"); break;
        default: break;
        }
        colour = KhensuTheme::textSecondary;
    }
    g.setColour(colour);
    g.setFont(KhensuTheme::bodyFont());
    g.drawText(text, 6, 0, w - 8, h, juce::Justification::centredLeft, true);
}

void MultitrackSessionView::selectedRowsChanged(int lastRowSelected) { showAdviceFor(lastRowSelected); }

void MultitrackSessionView::paint(juce::Graphics& g)
{
    g.fillAll(KhensuTheme::scrim);
    auto b = getLocalBounds().reduced(juce::jmax(16, getWidth() / 14), juce::jmax(16, getHeight() / 14));
    KhensuLookAndFeel::drawGlassPanel(g, b, GlassPanelVariant::Raised);
}

void MultitrackSessionView::resized()
{
    auto b = getLocalBounds().reduced(juce::jmax(16, getWidth() / 14), juce::jmax(16, getHeight() / 14)).reduced(16);
    if (b.getHeight() <= 0 || b.getWidth() <= 0) return;
    titleLabel_.setBounds(b.removeFromTop(26));
    statusLabel_.setBounds(b.removeFromTop(22));
    const int mismatchH = mismatchLabel_.getText().isEmpty()
                              ? 0
                              : juce::jmin(80, 16 * (mismatchLabel_.getText().retainCharacters("\n").length() + 1));
    mismatchLabel_.setBounds(b.removeFromTop(mismatchH));

    auto footer = b.removeFromBottom(40).reduced(0, 6);
    closeButton_.setBounds(footer.removeFromRight(90));
    footer.removeFromRight(8);
    createButton_.setBounds(footer.removeFromRight(200));
    footer.removeFromRight(8);
    cancelButton_.setBounds(footer.removeFromRight(90));
    footer.removeFromRight(8);
    analyzeButton_.setBounds(footer.removeFromRight(130));

    b.removeFromTop(6);
    auto left = b.removeFromLeft(juce::jmax(0, b.getWidth() * 3 / 5));
    b.removeFromLeft(12);
    adviceEditor_.setBounds(left.removeFromBottom(juce::jmax(0, left.getHeight() / 3)));
    left.removeFromBottom(8);
    trackTable_.setBounds(left);
    matrixLabel_.setBounds(b.removeFromTop(20));
    matrix_.setBounds(b);
}

} // namespace khensu
