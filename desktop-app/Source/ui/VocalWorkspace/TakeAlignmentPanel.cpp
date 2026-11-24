// desktop-app/Source/ui/VocalWorkspace/TakeAlignmentPanel.cpp
#include "../shared/ServiceErrorHint.h"
#include "TakeAlignmentPanel.h"
#include "../shared/PanelRunState.h"
#include "../../IAppController.h"
#include "../shared/RenderPaths.h"
#include "../shared/SessionAudioPaths.h"
#include "TakesBin.h"
#include <khensu/ipc/HttpServiceBridge.h>

namespace khensu {

namespace {
constexpr int kRowH = 24;
constexpr int kStatusH = 16;
} // namespace

void TakeAlignmentPanel::applyThemeColours()
{
    for (auto* b : {&chooseButton_, &runButton_}) {
        b->setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
        b->setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    }
    strengthSlider_.setColour(juce::Slider::trackColourId, KhensuTheme::accentTeal);
    strengthSlider_.setColour(juce::Slider::textBoxTextColourId, KhensuTheme::textSecondary);
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
}

TakeAlignmentPanel::TakeAlignmentPanel(IAppController& c) : PanelCard(c, "Doubles & Ad-lib Timing")
{
    applyThemeColours();
    setExplainer(TRANS("Doubles & Ad-lib Timing"),
                 TRANS("Tight doubles sound like one thick voice; loose ones sound like two singers. Add the double "
                       "or ad-lib takes, then either Align them to the lead (each phrase slides to the nearest lead "
                       "onset, up to 80 ms) or Quantize them to the tempo grid. Strength below 100% keeps some human "
                       "looseness. Each take is rendered as its own file - pick one in Audition to load it into "
                       "the transport."));

    chooseButton_.setTooltip(TRANS("Choose up to 8 double or ad-lib takes (WAV/AIFF/FLAC)."));
    chooseButton_.onClick = [this] { chooseTakes(); };
    addAndMakeVisible(chooseButton_);

    modeBox_.addItem(TRANS("Align to lead"), 1);
    modeBox_.addItem(TRANS("Quantize to grid"), 2);
    modeBox_.setSelectedId(1, juce::dontSendNotification);
    modeBox_.setTitle(TRANS("Timing mode"));
    modeBox_.setTooltip(TRANS("Align takes to the lead vocal's onsets, or quantize them to the tempo grid."));
    modeBox_.onChange = [this] { gridBox_.setEnabled(modeBox_.getSelectedId() == 2); };
    addAndMakeVisible(modeBox_);

    for (int div : {4, 8, 16, 32})
        gridBox_.addItem("1/" + juce::String(div), div);
    gridBox_.setSelectedId(16, juce::dontSendNotification);
    gridBox_.setTitle(TRANS("Quantize grid"));
    gridBox_.setTooltip(TRANS("Grid resolution for Quantize mode (tempo is estimated from the lead)."));
    gridBox_.setEnabled(false);
    addAndMakeVisible(gridBox_);

    strengthSlider_.setRange(0.0, 100.0, 1.0);
    strengthSlider_.setValue(100.0, juce::dontSendNotification);
    strengthSlider_.setTextValueSuffix("%");
    strengthSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 18);
    strengthSlider_.setTitle(TRANS("Alignment strength"));
    strengthSlider_.setTooltip(TRANS("How far each phrase moves toward its target (100% = fully tight)."));
    addAndMakeVisible(strengthSlider_);

    runButton_.setTooltip(TRANS("Align or quantize the chosen takes and render them."));
    runButton_.onClick = [this] { run(); };
    runButton_.setEnabled(false);
    addAndMakeVisible(runButton_);

    auditionBox_.setTitle(TRANS("Audition rendered take"));
    auditionBox_.setTooltip(TRANS("Load a rendered take into the transport for preview."));
    auditionBox_.setTextWhenNothingSelected(TRANS("Audition..."));
    auditionBox_.onChange = [this] { auditionSelected(); };
    auditionBox_.setEnabled(false);
    addAndMakeVisible(auditionBox_);

    statusLabel_.setFont(KhensuTheme::labelFont(10.0f));
    statusLabel_.setText(TRANS("Add double or ad-lib takes to tighten them against the lead."),
                         juce::dontSendNotification);
    addAndMakeVisible(statusLabel_);

    // The session's Takes bin fills an empty take list (WF-5); a list the
    // user picked here stays until they pick again. Every pick joins the
    // bin, so an emptied bin (a new session) empties the list too.
    auto fromBin = [this] {
        const auto bin = controller_.vocalEdit().sessionTakes();
        if (takePaths_.isEmpty() && !bin.isEmpty()) {
            useTakes(bin);
        } else if (bin.isEmpty() && !takePaths_.isEmpty()) {
            takePaths_.clear();
            runButton_.setEnabled(false);
            statusLabel_.setText(TRANS("Add double or ad-lib takes to tighten them against the lead."),
                                 juce::dontSendNotification);
        }
    };
    fromBin();
    takesListener_ = listen(controller_.vocalEdit(), this, &IVocalEdit::addTakesListener,
                            &IVocalEdit::removeTakesListener, fromBin);
}

juce::String TakeAlignmentPanel::referencePath() const
{
    return session_audio::vocalStemOrSourcePath(controller_);
}

void TakeAlignmentPanel::chooseTakes()
{
    // From the session's Takes bin, or files from disk that then join it (WF-5).
    juce::Component::SafePointer<TakeAlignmentPanel> self(this);
    takes_bin::pick(*this, chooseButton_, controller_.vocalEdit(), chooser_, TRANS("Choose double / ad-lib takes"),
                    takes_bin::Pick::Many, [self](const juce::StringArray& chosen) {
                        if (self == nullptr) return;
                        self->useTakes(chosen);
                    });
}

void TakeAlignmentPanel::useTakes(const juce::StringArray& chosen)
{
    takePaths_.clear();
    for (const auto& path : chosen) {
        if (takePaths_.size() >= 8) break;
        takePaths_.add(path);
    }
    runButton_.setEnabled(!takePaths_.isEmpty());
    statusLabel_.setText(TRANS("%n take(s) ready.").replace("%n", juce::String(takePaths_.size())),
                         juce::dontSendNotification);
}

void TakeAlignmentPanel::run()
{
    TakeAlignmentRequest req;
    req.referencePath = referencePath();
    req.takePaths = takePaths_;
    req.quantize = modeBox_.getSelectedId() == 2;
    req.gridDivision = gridBox_.getSelectedId();
    req.strength = strengthSlider_.getValue() / 100.0;
    if (!req.isValid()) {
        statusLabel_.setText(req.referencePath.isEmpty() ? TRANS("No active session (the lead vocal is the reference).")
                                                         : TRANS("Choose 1-8 takes first."),
                             juce::dontSendNotification);
        return;
    }
    // A fresh output per take: the server default "<take>_aligned.wav" was
    // overwritten by every re-run while the transport still had it (UIX-11).
    for (const auto& take : req.takePaths)
        req.outputPaths.add(uniqueRenderFile(juce::File(take), req.quantize ? "_quantized" : "_aligned")
                                .getFullPathName());
    runButton_.setEnabled(false);
    statusLabel_.setText(req.quantize ? TRANS("Quantizing...") : TRANS("Aligning..."), juce::dontSendNotification);
    const auto body = req.toVar();
    juce::Component::SafePointer<TakeAlignmentPanel> self(this);
    const juce::String sessionId =
        controller_.getActiveSession() != nullptr ? juce::String(controller_.getActiveSession()->id) : juce::String();
    // Decodes, aligns and renders up to 8 takes: the long pool (UIX-14).
    controller_.runLongInBackground([self, &work = controller_, body, sessionId] {
        ipc::HttpServiceBridge bridge(ipc::HttpServiceBridge::kDefaultBaseUrl, 300000);
        auto result = bridge.postAnalyzeJson("/analyze/align_takes", body);
        postIfCurrent(self, work, [result, sessionId](TakeAlignmentPanel& panel) {
            if (result.is_err()) {
                panel.statusLabel_.setText(TRANS("Alignment failed: ") + serviceErrorLine(result.error()),
                                           juce::dontSendNotification);
                return;
            }
            panel.result_ = TakeAlignmentResult::fromVar(result.value());
            panel.resultSessionId_ = sessionId;
            panel.auditionBox_.clear(juce::dontSendNotification);
            int id = 1;
            for (const auto& t : panel.result_.takes)
                if (t.renderedPath.isNotEmpty())
                    panel.auditionBox_.addItem(juce::File(t.renderedPath).getFileName(), id++);
            panel.auditionBox_.setEnabled(id > 1);
            panel.statusLabel_.setText(panel.result_.summary(), juce::dontSendNotification);
            panel.repaint();
        }, [](TakeAlignmentPanel& panel) { panel.runButton_.setEnabled(true); });
    });
}

void TakeAlignmentPanel::auditionSelected()
{
    const int idx = auditionBox_.getSelectedId() - 1;
    int seen = 0;
    for (const auto& t : result_.takes) {
        if (t.renderedPath.isEmpty()) continue;
        if (seen++ == idx && juce::File(t.renderedPath).existsAsFile()) {
            // A single take is not a version of the MIX: audition it
            // directly, never register it as the master (UIX-01).
            const auto* active = controller_.getActiveSession();
            if (active == nullptr || juce::String(active->id) != resultSessionId_) {
                statusLabel_.setText(TRANS("Session changed - the take was not loaded."), juce::dontSendNotification);
                return;
            }
            session_audio::applyRender(controller_, t.renderedPath, resultSessionId_,
                                       session_audio::RenderScope::Stem);
            statusLabel_.setText(TRANS("Loaded %f for preview - the mix is unchanged.")
                                     .replace("%f", juce::File(t.renderedPath).getFileName()),
                                 juce::dontSendNotification);
            return;
        }
    }
}

void TakeAlignmentPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    auto b = bodyArea();
    b.removeFromTop(kRowH * 2 + 4 + kStatusH + 4);
    if (result_.takes.empty() || b.getHeight() < 20) return;

    const double dur = juce::jmax(0.001, result_.durationSec());
    const double maxShift = juce::jmax(5.0, result_.maxAbsShiftMs());
    const int laneH = juce::jmin(36, b.getHeight() / static_cast<int>(result_.takes.size()));
    g.setFont(KhensuTheme::labelFont(9.0f));
    for (const auto& t : result_.takes) {
        auto lane = b.removeFromTop(laneH).reduced(0, 2).toFloat();
        auto label = lane.removeFromLeft(90.0f);
        g.setColour(KhensuTheme::textSecondary);
        g.drawText(juce::File(t.path).getFileNameWithoutExtension(), label, juce::Justification::centredLeft, true);
        g.setColour(KhensuTheme::bgVoid.withAlpha(0.4f));
        g.fillRect(lane);
        const float mid = lane.getCentreY();
        g.setColour(KhensuTheme::borderSubtle);
        g.drawHorizontalLine(juce::roundToInt(mid), lane.getX(), lane.getRight());
        // Bars: later (positive shift = pushed later) up, earlier down.
        for (const auto& s : t.segments) {
            const float x0 = lane.getX() + static_cast<float>(s.startSec / dur) * lane.getWidth();
            const float x1 = lane.getX() + static_cast<float>(s.endSec / dur) * lane.getWidth();
            const float h = static_cast<float>(std::abs(s.shiftMs) / maxShift) * lane.getHeight() * 0.5f;
            g.setColour((s.shiftMs >= 0.0 ? KhensuTheme::accentTeal : KhensuTheme::accentAmber).withAlpha(0.75f));
            g.fillRect(x0, s.shiftMs >= 0.0 ? mid - h : mid, juce::jmax(1.0f, x1 - x0 - 1.0f), h);
        }
    }
}

void TakeAlignmentPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto row1 = b.removeFromTop(kRowH);
    chooseButton_.setBounds(row1.removeFromLeft(100).reduced(0, 1));
    row1.removeFromLeft(6);
    modeBox_.setBounds(row1.removeFromLeft(140).reduced(0, 1));
    row1.removeFromLeft(6);
    gridBox_.setBounds(row1.removeFromLeft(70).reduced(0, 1));
    row1.removeFromLeft(6);
    strengthSlider_.setBounds(row1);
    b.removeFromTop(4);
    auto row2 = b.removeFromTop(kRowH);
    runButton_.setBounds(row2.removeFromLeft(120).reduced(0, 1));
    row2.removeFromLeft(6);
    auditionBox_.setBounds(row2.removeFromLeft(220).reduced(0, 1));
    b.removeFromTop(4);
    statusLabel_.setBounds(b.removeFromTop(kStatusH));
}

} // namespace khensu
