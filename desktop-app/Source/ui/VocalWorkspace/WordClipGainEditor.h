// desktop-app/Source/ui/VocalWorkspace/WordClipGainEditor.h
// Per-word clip-gain drawing: the vocal's waveform with each transcribed word
// boxed on top. Drag across the waveform to "draw" a gain line - every word
// the stroke passes takes the stroke's level - or select a word with
// Left/Right and nudge it with Up/Down. Render applies the moves via
// POST /render/word_clip_gain and loads the result as the A/B "B" side.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "WordClipGainModel.h"

namespace khensu {

class IAppController;

class WordClipGainEditor final : public PanelCard {
  public:
    explicit WordClipGainEditor(IAppController& c);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    bool keyPressed(const juce::KeyPress& key) override;

  private:
    juce::String targetFilePath() const;
    void transcribe();
    void render();
    void updateStatus();

    juce::Rectangle<float> laneArea() const;
    float xForTime(double t) const;
    double timeForX(float x) const;
    float yForDb(double db) const;
    double dbForY(float y) const;

    WordClipGainModel model_;
    std::vector<float> peaks_; ///< downsampled |peak| envelope, 0..1
    double durationSec_ = 0.0;
    int selected_ = -1;
    bool drawing_ = false;
    juce::Point<float> lastDrawPos_;

    juce::TextButton transcribeButton_{TRANS("Find Words")};
    juce::TextButton resetButton_{TRANS("Reset")};
    juce::TextButton renderButton_{TRANS("Render")};
    juce::Label statusLabel_;

    void applyThemeColours();
    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WordClipGainEditor)
};

} // namespace khensu
