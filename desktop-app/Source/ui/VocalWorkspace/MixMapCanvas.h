// desktop-app/Source/ui/VocalWorkspace/MixMapCanvas.h
// Unified mix map: every separated stem as a node placed by where it sits in
// the spectrum and sized by its share of the mix energy, joined by masking
// edges (thickness/colour = collision severity, labelled with the clash
// band). A stacked-area energy timeline underneath drives the moment shown;
// it follows playback, or click/drag it to inspect any point, or switch to
// the song average.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../shared/MixMapModel.h"

namespace khensu {

class IAppController;

class MixMapCanvas final : public PanelCard, private juce::Timer {
  public:
    explicit MixMapCanvas(IAppController& c);
    ~MixMapCanvas() override;

    /// Called when stems become available (paths may be empty when absent).
    void setStemPaths(const juce::String& vocals, const juce::String& drums, const juce::String& bass,
                      const juce::String& other);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    bool keyPressed(const juce::KeyPress& key) override;

  private:
    void timerCallback() override;
    void fetch();
    void setTimeFromStrip(float x);
    void updateStatus();
    juce::Rectangle<float> mapArea() const;
    juce::Rectangle<float> stripArea() const;
    float xForHz(double hz, const juce::Rectangle<float>& r) const;
    juce::Point<float> nodeCentre(size_t index, size_t count, double hz, const juce::Rectangle<float>& r) const;
    static juce::Colour stemColour(const juce::String& stem);

    MixMapModel model_;
    juce::String paths_[4];
    double timeSec_ = -1.0; ///< -1 = song average
    bool followPlayback_ = true;
    bool fetching_ = false;
    juce::String hoverText_;

    juce::TextButton refreshButton_{TRANS("Build Mix Map")};
    juce::TextButton averageButton_{TRANS("Song Average")};
    juce::Label statusLabel_;

    void applyThemeColours();
    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixMapCanvas)
};

} // namespace khensu
