// desktop-app/Source/ui/VocalWorkspace/PitchNoteGridEditor.h
// Graphical pitch-correction editor: a piano-roll note grid over the vocal's
// detected pitch curve. Each detected note is a block the user drags (mouse)
// or nudges (keyboard Up/Down) to a semitone; a Retune slider pulls every
// note toward its nearest semitone at once (or, with "Snap to key", toward
// the nearest pitch of the session's detected key) without touching notes
// moved by hand. Moves are undoable (Undo / Ctrl+Z). Render applies the moves
// via POST /render/pitch_correct and loads the result as the A/B "B" side.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "PitchNoteGridModel.h"

namespace khensu {

class IAppController;

class PitchNoteGridEditor final : public PanelCard {
  public:
    explicit PitchNoteGridEditor(IAppController& c);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    bool keyPressed(const juce::KeyPress& key) override;

  private:
    juce::String targetFilePath() const;
    void analyze();
    void render();
    void updateStatus();
    /// Pulls the session's detected key into the model (note names, snap).
    void refreshKey();
    void performUndo();

    juce::Rectangle<float> gridArea() const;
    float xForTime(double t) const;
    double timeForX(float x) const;
    float yForMidi(double midi) const;
    double midiForY(float y) const;

    PitchNoteGridModel model_;
    int selected_ = -1;
    int dragNote_ = -1;
    int dragStartMidi_ = 0;
    // UIX-13 drag threshold: a click with a few pixels of jitter must not
    // hard-tune the note. The move starts once the pointer travels
    // kDragThresholdPx vertically; until then the note keeps its correction.
    bool dragActive_ = false;
    double dragOrigShift_ = 0.0;
    bool dragOrigManual_ = false;
    bool sliderDragging_ = false;

    juce::TextButton analyzeButton_{TRANS("Detect Notes")};
    juce::Slider retuneSlider_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::ToggleButton snapKeyToggle_{TRANS("Snap to key")};
    juce::TextButton undoButton_{TRANS("Undo")};
    juce::TextButton resetButton_{TRANS("Reset")};
    juce::TextButton renderButton_{TRANS("Render && A/B")};
    juce::Label statusLabel_;

    void applyThemeColours();
    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchNoteGridEditor)
};

} // namespace khensu
