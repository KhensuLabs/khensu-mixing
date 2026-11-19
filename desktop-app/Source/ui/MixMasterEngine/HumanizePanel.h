// desktop-app/Source/ui/MixMasterEngine/HumanizePanel.h
// Analog Feel (spec §5.2, reframed 2026-11-07): a creative effect that adds
// the character of a played, taped and hand-ridden performance - tape drift,
// a fader ride and room tone - to rigid programmed / in-the-box material.
// Every render is non-destructive (loaded as the loudness-matched A/B "B"
// side) and judged by ear; nothing is scanned and no detector score is shown
// or targeted. "Clean Metadata" removes workflow / personal tags only and
// always keeps identity, rights and AI-provenance / disclosure tags.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/PanelCard.h"
#include <vector>

namespace khensu {

class IAppController;

class HumanizePanel final : public PanelCard, public juce::FileDragAndDropTarget {
  public:
    explicit HumanizePanel(IAppController& c);
    void paint(juce::Graphics& g) override;
    void resized() override;

    /// Pulls the current mix's waveform and builds the per-stem lanes.
    /// Fires on screen entry.
    void refreshForSession();

    // FileDragAndDropTarget
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

  private:
    void runHumanize();      ///< custom render through the chain (shaping sliders)
    void runAnalogFeel();    ///< one-click /humanize/analog_feel (amount + characters)
    void runCleanMetadata(); ///< /metadata/clean (keeps provenance / disclosure tags)
    void runDeepHumanize();
    void rebuildStemLanes();

    // One row per available Demucs stem: name + its own intensity slider.
    // Optionally expands (accordion-style - only one lane at a time, to bound
    // how much extra height the fixed-size PanelCard needs) into four more
    // sliders overriding this stem's groove shape (wow rate/depth/level
    // variation/noise floor) instead of sharing the global shaping row.
    struct StemLane {
        juce::String id, label;
        std::unique_ptr<juce::Slider> intensity;
        juce::File externalFile;

        bool customGroove = false;
        std::unique_ptr<juce::TextButton> customToggle;
        std::unique_ptr<juce::Slider> wowRate, depth, levelVar, noiseFloor;
    };
    std::vector<StemLane> stemLanes_;
    bool stemLanesBuilt_ = false;
    void buildCustomGrooveControls(StemLane& lane);
    void setExpandedLane(int laneIndex); ///< collapses every other lane's custom row

    juce::Slider intensity_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::Label intensityCaption_;

    // Groove / Chain Shaping Sliders
    juce::Slider wowRateSlider_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::Label wowRateLabel_;
    juce::Slider depthSlider_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::Label depthLabel_;
    juce::Slider levelVarSlider_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::Label levelVarLabel_;
    juce::Slider noiseFloorSlider_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::Label noiseFloorLabel_;

    juce::TextButton humanizeButton_{TRANS("Custom Render")};
    juce::TextButton analogFeelButton_{TRANS("Analog Feel")};
    juce::TextButton deepButton_{TRANS("Per-Stem Render")};
    juce::TextButton cleanMetaButton_{TRANS("Clean Metadata")};
    juce::ToggleButton tapeDriftToggle_{TRANS("Tape drift")};
    juce::ToggleButton faderRideToggle_{TRANS("Fader ride")};
    juce::ToggleButton roomToneToggle_{TRANS("Room tone")};
    juce::Label statusLabel_;
    juce::Label reportLabel_; ///< Deep Humanize applied-changes report

    std::vector<float> peaks_; ///< mix waveform (from AppController)
    bool renderInFlight_ = false;
    bool dragOver_ = false;

    juce::Rectangle<int> laneArea_;

    /// Re-applies every cached palette colour, including the per-stem lane
    /// controls. Called at construction and on each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HumanizePanel)
};

} // namespace khensu
