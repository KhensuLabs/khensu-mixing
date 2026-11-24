// desktop-app/Source/ui/VocalWorkspace/PitchNoteGridEditor.cpp
#include "../shared/ServiceErrorHint.h"
#include "PitchNoteGridEditor.h"
#include "../shared/PanelRunState.h"
#include "../../IAppController.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "MixTuningReference.h"
#include "../shared/RenderPaths.h"
#include "../shared/SessionAudioPaths.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

namespace {
constexpr int kControlsH = 26;
constexpr int kStatusH = 16;
constexpr float kKeyW = 34.0f;
constexpr int kDragThresholdPx = 4;

juce::String signedCents(double cents)
{
    const int c = juce::roundToInt(cents);
    return (c > 0 ? "+" : "") + juce::String(c) + "c";
}
} // namespace

void PitchNoteGridEditor::applyThemeColours()
{
    for (auto* b : {&analyzeButton_, &renderButton_, &resetButton_, &undoButton_}) {
        b->setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
        b->setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    }
    retuneSlider_.setColour(juce::Slider::trackColourId, KhensuTheme::accentTeal);
    retuneSlider_.setColour(juce::Slider::textBoxTextColourId, KhensuTheme::textSecondary);
    snapKeyToggle_.setColour(juce::ToggleButton::textColourId, KhensuTheme::textSecondary);
    snapKeyToggle_.setColour(juce::ToggleButton::tickColourId, KhensuTheme::accentTeal);
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
}

PitchNoteGridEditor::PitchNoteGridEditor(IAppController& c) : PanelCard(c, "Pitch Editor (Note Grid)")
{
    applyThemeColours();
    setExplainer(TRANS("Pitch Editor"),
                 TRANS("A graphical tuning editor. Detect Notes draws each sung note as a block on a "
                       "piano-roll grid over the raw pitch curve. Drag a block up or down (or select "
                       "it and press Up/Down) to move it to a semitone; double-click resets it. "
                       "Retune pulls every note toward its nearest semitone on the mix's own tuning "
                       "reference (e.g. A=432 for a 432 Hz track, not always 440) - 100% is hard-tuned, "
                       "around 60% keeps natural drift - and leaves notes you moved by hand alone. "
                       "Snap to key targets the nearest note of the detected key instead of the nearest "
                       "semitone. Undo (Ctrl+Z) steps back through moves. Render applies the moves with "
                       "crossfaded pitch shifts and loads the result as the A/B \"B\" side."));
    setWantsKeyboardFocus(true);
    setTitle(TRANS("Pitch editor note grid"));
    setDescription(TRANS("Piano-roll of detected vocal notes. Left/Right select a note, Up/Down move it a semitone "
                         "(a scale step with Snap to key), S snaps it, Delete resets it, Ctrl+Z undoes."));

    analyzeButton_.setTooltip(TRANS("Detect the vocal's notes and pitch curve (uses the vocal stem when available)."));
    analyzeButton_.onClick = [this] { analyze(); };
    addAndMakeVisible(analyzeButton_);

    retuneSlider_.setRange(0.0, 100.0, 1.0);
    retuneSlider_.setValue(0.0, juce::dontSendNotification);
    retuneSlider_.setTextValueSuffix("%");
    retuneSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 18);
    retuneSlider_.setTitle(TRANS("Retune amount"));
    retuneSlider_.setTooltip(TRANS("Pull every note toward its nearest semitone (0% = untouched, 100% = hard-tuned)."));
    // One undo step per slider gesture (drag start), or per discrete change
    // (click on the track, typed value).
    retuneSlider_.onDragStart = [this] {
        sliderDragging_ = true;
        model_.pushUndo();
    };
    retuneSlider_.onDragEnd = [this] { sliderDragging_ = false; };
    retuneSlider_.onValueChange = [this] {
        if (!sliderDragging_) model_.pushUndo();
        model_.retuneAll(retuneSlider_.getValue() / 100.0);
        updateStatus();
        repaint();
    };
    retuneSlider_.setEnabled(false);
    addAndMakeVisible(retuneSlider_);

    snapKeyToggle_.setTitle(TRANS("Snap to key"));
    snapKeyToggle_.setTooltip(TRANS("Retune, S and Up/Down target the notes of the session's detected key "
                                    "instead of every semitone (minor keys include the raised leading tone)."));
    snapKeyToggle_.onClick = [this] {
        model_.setSnapToKey(snapKeyToggle_.getToggleState());
        if (!model_.empty() && retuneSlider_.getValue() > 0.0) {
            model_.pushUndo();
            model_.retuneAll(retuneSlider_.getValue() / 100.0);
        }
        updateStatus();
        repaint();
    };
    snapKeyToggle_.setEnabled(false);
    addAndMakeVisible(snapKeyToggle_);

    undoButton_.setTooltip(TRANS("Undo the last pitch move (Ctrl+Z)."));
    undoButton_.onClick = [this] { performUndo(); };
    undoButton_.setEnabled(false);
    addAndMakeVisible(undoButton_);

    resetButton_.setTooltip(TRANS("Clear every pitch correction."));
    resetButton_.onClick = [this] {
        model_.pushUndo();
        model_.resetAll();
        retuneSlider_.setValue(0.0, juce::dontSendNotification);
        updateStatus();
        repaint();
    };
    resetButton_.setEnabled(false);
    addAndMakeVisible(resetButton_);

    renderButton_.setTooltip(TRANS("Apply the corrections and load the result as the A/B \"B\" side."));
    renderButton_.onClick = [this] { render(); };
    renderButton_.setEnabled(false);
    addAndMakeVisible(renderButton_);

    statusLabel_.setFont(KhensuTheme::labelFont(10.0f));
    statusLabel_.setText(TRANS("Detect notes to start tuning."), juce::dontSendNotification);
    addAndMakeVisible(statusLabel_);
    refreshKey();
}

void PitchNoteGridEditor::refreshKey()
{
    model_.setKey(controller_.hasAnalysis() ? controller_.currentAnalysis().python_data.pitch_key : std::string());
    const auto& key = model_.key();
    if (key.has_value()) {
        const juce::String name = juce::String(theory::tonicName(*key)) + (key->minor ? " minor" : " major");
        snapKeyToggle_.setButtonText(TRANS("Snap to key") + " (" + name + ")");
    } else {
        snapKeyToggle_.setButtonText(TRANS("Snap to key"));
        snapKeyToggle_.setToggleState(false, juce::dontSendNotification);
        model_.setSnapToKey(false);
    }
    snapKeyToggle_.setEnabled(key.has_value() && !model_.empty());
}

void PitchNoteGridEditor::performUndo()
{
    if (!model_.undo()) return;
    updateStatus();
    repaint();
}

juce::String PitchNoteGridEditor::targetFilePath() const
{
    // The current vocal: an earlier edit's render when one stacked (WF-4).
    return session_audio::vocalStemOrSourcePath(controller_);
}

void PitchNoteGridEditor::analyze()
{
    const auto target = targetFilePath();
    if (target.isEmpty()) {
        statusLabel_.setText(TRANS("No active session."), juce::dontSendNotification);
        return;
    }
    analyzeButton_.setEnabled(false);
    statusLabel_.setText(TRANS("Detecting notes..."), juce::dontSendNotification);
    juce::Component::SafePointer<PitchNoteGridEditor> self(this);
    const juce::String mixPath = controller_.activeSourcePath();
    controller_.runInBackground([self, &work = controller_, target, mixPath] {
        ipc::HttpServiceBridge bridge;
        // AUDIT-016: snap grid = the mix's concert A (A=440 only if unmeasurable).
        const auto referenceHz = MixTuningReference::forJob(bridge, mixPath);
        auto result = bridge.analyzePitchCurve(target.toStdString(), referenceHz);
        postIfCurrent(self, work, [result](PitchNoteGridEditor& panel) {
            if (result.is_err()) {
                panel.statusLabel_.setText(TRANS("Detection failed: ") + serviceErrorLine(result.error()),
                                           juce::dontSendNotification);
                return;
            }
            const bool ok = panel.model_.loadFromResponse(result.value());
            panel.selected_ = ok ? 0 : -1;
            panel.refreshKey();
            panel.retuneSlider_.setEnabled(ok);
            panel.resetButton_.setEnabled(ok);
            panel.renderButton_.setEnabled(ok);
            panel.retuneSlider_.setValue(0.0, juce::dontSendNotification);
            if (!ok)
                panel.statusLabel_.setText(TRANS("No sung notes detected."), juce::dontSendNotification);
            else
                panel.updateStatus();
            panel.repaint();
        }, [](PitchNoteGridEditor& panel) { panel.analyzeButton_.setEnabled(true); });
    });
}

void PitchNoteGridEditor::render()
{
    const auto target = targetFilePath();
    const auto corrections = model_.correctionsAsVar();
    if (target.isEmpty()) return;
    if (corrections.size() == 0) {
        statusLabel_.setText(TRANS("Nothing to render - move a note or raise Retune first."),
                             juce::dontSendNotification);
        return;
    }
    renderButton_.setEnabled(false);
    statusLabel_.setText(TRANS("Rendering corrections..."), juce::dontSendNotification);
    // Captured at start: the render must register into THIS session only (AUDIT-002).
    const juce::String sessionId = controller_.getActiveSession() != nullptr
                                       ? juce::String(controller_.getActiveSession()->id)
                                       : juce::String();
    juce::Component::SafePointer<PitchNoteGridEditor> self(this);
    auto* controllerPtr = &controller_;
    // A fresh name per render: the service's fixed <source>_pitchcorrected.wav default
    // overwrote the file earlier render-history nodes point at.
    const juce::String outPath = uniqueRenderFile(juce::File(target), "_pitchcorrected").getFullPathName();
    // The vocal STEM's correction is not a version of the mix (UIX-01): the
    // scope is decided by what was rendered, at start.
    const auto scope = session_audio::scopeForInput(controller_, target);
    controller_.runInBackground([self, target, corrections, outPath, controllerPtr, sessionId, scope] {
        auto* body = new juce::DynamicObject();
        body->setProperty(af::PitchCorrectRequest::file_path, target);
        body->setProperty(af::PitchCorrectRequest::corrections, corrections);
        body->setProperty(af::PitchCorrectRequest::output_path, outPath);
        ipc::HttpServiceBridge bridge;
        auto result = bridge.postAnalyzeJson("/render/pitch_correct", juce::var(body));
        postIfCurrent(self, *controllerPtr, [result, controllerPtr, sessionId, scope, target](PitchNoteGridEditor& panel) {
            if (result.is_err()) {
                panel.statusLabel_.setText(TRANS("Render failed: ") + serviceErrorLine(result.error()),
                                           juce::dontSendNotification);
                return;
            }
            const auto rendered = result.value()[af::PitchCorrectResponse::rendered_path].toString();
            if (rendered.isEmpty() || !juce::File(rendered).existsAsFile()) {
                panel.statusLabel_.setText(TRANS("Render produced no file."), juce::dontSendNotification);
                return;
            }
            // A stem render is auditioned directly, never registered as the
            // session's accepted mix (UIX-01).
            const bool isMix = session_audio::applyRender(*controllerPtr, rendered, sessionId, scope, target);
            const juce::String count(static_cast<int>(result.value()[af::PitchCorrectResponse::applied]));
            panel.statusLabel_.setText(isMix ? TRANS("Rendered") + " " + count + " " +
                                                   TRANS("corrections - use the transport A/B to compare.")
                                             : TRANS("Rendered") + " " + count + " " +
                                                   TRANS("corrections - corrected vocal loaded for preview; the mix is unchanged."),
                                       juce::dontSendNotification);
        }, [](PitchNoteGridEditor& panel) { panel.renderButton_.setEnabled(true); });
    });
}

void PitchNoteGridEditor::updateStatus()
{
    undoButton_.setEnabled(model_.canUndo());
    const auto moved = model_.corrections();
    juce::String s = juce::String(static_cast<int>(model_.notes().size())) + TRANS(" notes, ") +
                     juce::String(static_cast<int>(moved.size())) + TRANS(" corrected") + " - A=" +
                     juce::String(model_.tuning().gridHz, 1) + TRANS(" Hz");
    if (selected_ >= 0 && static_cast<size_t>(selected_) < model_.notes().size()) {
        // Per-note cents readout: detected deviation from its semitone, and
        // where the correction lands (key-aware note names).
        const auto& n = model_.notes()[static_cast<size_t>(selected_)];
        const int from = static_cast<int>(std::lround(n.midi));
        const int to = static_cast<int>(std::lround(n.correctedMidi()));
        s << " - " << TRANS("selected") << " " << model_.noteName(from) << " "
          << signedCents((n.midi - from) * 100.0) << " -> " << model_.noteName(to) << " "
          << signedCents(n.correctedCents()) << " (" << (n.shift >= 0 ? "+" : "") << juce::String(n.shift, 2)
          << " st" << (n.manual ? juce::String(", ") + TRANS("manual") : juce::String()) << ")";
    }
    // Off-grid caveat replaces the selection detail: it changes what Retune means.
    const auto warning = model_.tuning().warningText();
    if (warning.isNotEmpty()) s = warning;
    statusLabel_.setText(s, juce::dontSendNotification);
    statusLabel_.setColour(juce::Label::textColourId,
                           warning.isNotEmpty() ? KhensuTheme::accentAmber : KhensuTheme::textTertiary);
}

juce::Rectangle<float> PitchNoteGridEditor::gridArea() const
{
    auto b = bodyArea();
    b.removeFromTop(kControlsH + 2 + kStatusH + 2);
    return b.toFloat().reduced(2.0f);
}

float PitchNoteGridEditor::xForTime(double t) const
{
    const auto g = gridArea();
    const double d = juce::jmax(0.001, model_.durationSec());
    return g.getX() + kKeyW + static_cast<float>(t / d) * (g.getWidth() - kKeyW);
}

double PitchNoteGridEditor::timeForX(float x) const
{
    const auto g = gridArea();
    const double d = juce::jmax(0.001, model_.durationSec());
    return static_cast<double>((x - g.getX() - kKeyW) / juce::jmax(1.0f, g.getWidth() - kKeyW)) * d;
}

float PitchNoteGridEditor::yForMidi(double midi) const
{
    const auto g = gridArea();
    const auto r = model_.displayRange();
    const double span = juce::jmax(1, r.getLength());
    return g.getBottom() - static_cast<float>((midi - r.getStart()) / span) * g.getHeight();
}

double PitchNoteGridEditor::midiForY(float y) const
{
    const auto g = gridArea();
    const auto r = model_.displayRange();
    return r.getStart() + static_cast<double>((g.getBottom() - y) / juce::jmax(1.0f, g.getHeight())) * r.getLength();
}

void PitchNoteGridEditor::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    const auto area = gridArea();
    if (area.getHeight() < 40.0f) return;
    if (model_.empty()) {
        drawEmptyState(g, "No notes yet", "Detect Notes draws the vocal as editable blocks on a semitone grid.",
                       kControlsH + kStatusH + 4);
        return;
    }

    const auto range = model_.displayRange();
    const float rowH = area.getHeight() / static_cast<float>(juce::jmax(1, range.getLength()));

    // Semitone rows with piano-key gutter; black-key rows are shaded.
    for (int m = range.getStart(); m < range.getEnd(); ++m) {
        const float y = yForMidi(m + 1);
        const int pc = ((m % 12) + 12) % 12;
        const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
        g.setColour(black ? KhensuTheme::bgVoid.withAlpha(0.5f) : KhensuTheme::bgRaised.withAlpha(0.35f));
        g.fillRect(area.getX() + kKeyW, y, area.getWidth() - kKeyW, rowH);
        g.setColour(black ? KhensuTheme::bgVoid : KhensuTheme::textSecondary.withAlpha(0.8f));
        g.fillRect(area.getX(), y + 0.5f, kKeyW - 2.0f, rowH - 1.0f);
        if (pc == 0 && rowH >= 7.0f) {
            g.setColour(KhensuTheme::bgVoid);
            g.setFont(KhensuTheme::labelFont(juce::jmin(9.0f, rowH - 1.0f)));
            g.drawText(PitchNoteGridModel::midiName(m), juce::Rectangle<float>(area.getX(), y, kKeyW - 4.0f, rowH),
                       juce::Justification::centredRight);
        }
        g.setColour(KhensuTheme::borderSubtle.withAlpha(0.4f));
        g.drawHorizontalLine(juce::roundToInt(y), area.getX() + kKeyW, area.getRight());
    }

    // Raw detected pitch curve.
    juce::Path curve;
    bool penDown = false;
    const auto& f0 = model_.curve();
    for (size_t i = 0; i < f0.size(); ++i) {
        if (!f0[i]) {
            penDown = false;
            continue;
        }
        const float x = xForTime(static_cast<double>(i) * model_.hopSec());
        const float y = yForMidi(*f0[i] + 0.5);
        if (!penDown)
            curve.startNewSubPath(x, y);
        else
            curve.lineTo(x, y);
        penDown = true;
    }
    g.setColour(KhensuTheme::textTertiary.withAlpha(0.7f));
    g.strokePath(curve, juce::PathStrokeType(1.0f));

    // Notes: ghost at detected pitch, solid block at corrected pitch.
    for (size_t i = 0; i < model_.notes().size(); ++i) {
        const auto& n = model_.notes()[i];
        const float x0 = xForTime(n.startSec), x1 = xForTime(n.endSec);
        const float w = juce::jmax(2.0f, x1 - x0);
        const juce::Rectangle<float> ghost(x0, yForMidi(n.midi + 1.0), w, rowH);
        const juce::Rectangle<float> block(x0, yForMidi(n.correctedMidi() + 1.0), w, rowH);
        g.setColour(KhensuTheme::textTertiary.withAlpha(0.25f));
        g.drawRoundedRectangle(ghost.reduced(0.5f), 2.0f, 1.0f);
        const bool moved = std::abs(n.shift) >= 0.01;
        const auto fill = moved ? KhensuTheme::accentTeal : KhensuTheme::accentBlue;
        g.setColour(fill.withAlpha(static_cast<int>(i) == selected_ ? 0.95f : 0.65f));
        g.fillRoundedRectangle(block.reduced(0.5f, 1.0f), 2.0f);
        // Per-note cents readout: how far the sung note sits off its semitone.
        if (w >= 30.0f && rowH >= 9.0f) {
            const int from = static_cast<int>(std::lround(n.midi));
            g.setColour(KhensuTheme::textPrimary.withAlpha(0.85f));
            g.setFont(KhensuTheme::labelFont(juce::jmin(9.0f, rowH - 1.0f)));
            g.drawText(signedCents((n.midi - from) * 100.0), block.reduced(2.0f, 0.0f),
                       juce::Justification::centredLeft, false);
        }
        if (static_cast<int>(i) == selected_) {
            g.setColour(KhensuTheme::focusRing);
            g.drawRoundedRectangle(block.expanded(1.0f), 3.0f, 1.5f);
        }
    }

    if (hasKeyboardFocus(false)) {
        g.setColour(KhensuTheme::focusRing.withAlpha(0.6f));
        g.drawRect(area, 1.0f);
    }
}

void PitchNoteGridEditor::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(kControlsH);
    analyzeButton_.setBounds(controls.removeFromLeft(110).reduced(0, 1));
    controls.removeFromLeft(6);
    renderButton_.setBounds(controls.removeFromRight(110).reduced(0, 1));
    controls.removeFromRight(6);
    resetButton_.setBounds(controls.removeFromRight(60).reduced(0, 1));
    controls.removeFromRight(4);
    undoButton_.setBounds(controls.removeFromRight(52).reduced(0, 1));
    controls.removeFromRight(6);
    snapKeyToggle_.setBounds(controls.removeFromRight(juce::jmin(170, juce::jmax(0, controls.getWidth() / 2))));
    controls.removeFromRight(4);
    retuneSlider_.setBounds(controls);
    b.removeFromTop(2);
    statusLabel_.setBounds(b.removeFromTop(kStatusH));
}

void PitchNoteGridEditor::mouseDown(const juce::MouseEvent& e)
{
    const auto area = gridArea();
    if (model_.empty() || !area.contains(e.position)) {
        PanelCard::mouseDown(e);
        return;
    }
    grabKeyboardFocus();
    const int hit = model_.noteAt(timeForX(e.position.x));
    selected_ = hit >= 0 ? hit : selected_;
    dragNote_ = hit;
    dragActive_ = false;
    if (hit >= 0) {
        const auto& n = model_.notes()[static_cast<size_t>(hit)];
        dragStartMidi_ = static_cast<int>(std::lround(n.correctedMidi()));
        dragOrigShift_ = n.shift;
        dragOrigManual_ = n.manual;
    }
    updateStatus();
    repaint();
}

void PitchNoteGridEditor::mouseDrag(const juce::MouseEvent& e)
{
    if (dragNote_ < 0) {
        PanelCard::mouseDrag(e);
        return;
    }
    // UIX-13: no move until the pointer leaves the click's jitter radius -
    // a plain click used to snap the note to a semitone (hard-tune it).
    if (!dragActive_) {
        if (std::abs(e.getDistanceFromDragStartY()) < kDragThresholdPx) return;
        dragActive_ = true;
        model_.pushUndo();
    }
    const auto index = static_cast<size_t>(dragNote_);
    const double downMidi = midiForY(e.mouseDownPosition.y);
    const double nowMidi = midiForY(e.position.y);
    const int delta = static_cast<int>(std::lround(nowMidi - downMidi));
    if (delta == 0) {
        // Back where it started: restore the note's own correction.
        model_.setShift(index, dragOrigShift_, dragOrigManual_);
    } else {
        int target = dragStartMidi_ + delta;
        if (model_.snapToKey()) target = theory::nearestScaleMidi(static_cast<double>(target), *model_.key());
        model_.snapNoteTo(index, target);
    }
    updateStatus();
    repaint();
}

void PitchNoteGridEditor::mouseUp(const juce::MouseEvent&)
{
    dragNote_ = -1;
    dragActive_ = false;
}

void PitchNoteGridEditor::mouseDoubleClick(const juce::MouseEvent& e)
{
    const int hit = model_.noteAt(timeForX(e.position.x));
    if (hit >= 0) model_.pushUndo();
    if (hit >= 0 && model_.resetNote(static_cast<size_t>(hit))) {
        selected_ = hit;
        updateStatus();
        repaint();
    }
}

bool PitchNoteGridEditor::keyPressed(const juce::KeyPress& key)
{
    if (model_.empty()) return false;
    const int n = static_cast<int>(model_.notes().size());
    if (key == juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0)) {
        performUndo();
        return true;
    }
    const bool editKey = selected_ >= 0 && (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey ||
                                            key == juce::KeyPress::deleteKey ||
                                            key == juce::KeyPress::backspaceKey || key.getTextCharacter() == 's');
    if (editKey) model_.pushUndo();
    bool handled = true;
    if (key == juce::KeyPress::leftKey)
        selected_ = juce::jmax(0, selected_ - 1);
    else if (key == juce::KeyPress::rightKey)
        selected_ = juce::jmin(n - 1, selected_ + 1);
    else if (key == juce::KeyPress::upKey && selected_ >= 0)
        model_.nudgeNote(static_cast<size_t>(selected_), 1);
    else if (key == juce::KeyPress::downKey && selected_ >= 0)
        model_.nudgeNote(static_cast<size_t>(selected_), -1);
    else if ((key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) && selected_ >= 0)
        model_.resetNote(static_cast<size_t>(selected_));
    else if (key.getTextCharacter() == 's' && selected_ >= 0)
        model_.snapNoteToTarget(static_cast<size_t>(selected_));
    else
        handled = false;
    if (handled) {
        updateStatus();
        repaint();
    }
    return handled;
}

} // namespace khensu
