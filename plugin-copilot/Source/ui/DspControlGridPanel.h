#pragma once
#include <JuceHeader.h>
#include "PluginLookAndFeel.h"
#include <khensu/ui/KhensuKnob.h>
#include <khensu/ui/ModuleViz.h>
#include <array>
#include <cstdint>

namespace khensu {

class KhensuCopilotProcessor;

// "Chain" tab: exposes every built-in DSP-chain parameter (HPF/LPF, 3 EQ
// bands, single and multiband compressor with crossovers, de-esser, output
// gain, chain mix, limiter) as KhensuKnobs / toggle buttons, each with a
// small dot that lights up while KhensuCopilotProcessor::isAiSet() is true
// for that parameter. UX2-6a: every knob draws its value under the dial as
// plain text with its unit ("1.2 kHz", "-18.0 dB") and its name under that;
// double-click or Enter types a value, Alt/Option-click resets. All knobs
// share one cell size, so dials, values and names line up across the grid.
// No bypass control here - PluginEditor's header bypassButton_ covers global
// bypass (ParamIDs::bypass).
class DspControlGridPanel final : public juce::Component, private juce::Timer {
  public:
    explicit DspControlGridPanel(KhensuCopilotProcessor& proc);
    ~DspControlGridPanel() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;

    int getNumKnobsForTest() const noexcept { return static_cast<int>(knobs_.size()); }
    int getNumTogglesForTest() const noexcept { return static_cast<int>(stageToggles_.size()); }
    int getNumShapeSelectorsForTest() const noexcept { return static_cast<int>(shapes_.size()); }
    /// The knob for `paramId` (tests), or nullptr.
    ui::KhensuKnob* knobForTest(const juce::String& paramId) const;

  private:
    void timerCallback() override; // 10 Hz while shown - repaints when an AI-set dot changes

    struct KnobRow {
        juce::String paramId;
        std::unique_ptr<ui::KhensuKnob> knob;
        std::unique_ptr<juce::SliderParameterAttachment> attachment;
    };
    struct ToggleRow {
        juce::String paramId;
        std::unique_ptr<juce::ToggleButton> button;
        std::unique_ptr<juce::ButtonParameterAttachment> attachment;
    };

    void addKnob(const char* paramId, const juce::String& labelText);
    void addToggle(const char* paramId, const juce::String& labelText, std::vector<ToggleRow>& into);
    /// OVH-8: an EQ band's shape (bell / low shelf / high shelf).
    void addShape(const char* paramId);
    struct ShapeRow {
        std::unique_ptr<juce::ComboBox> box;
        std::unique_ptr<juce::ComboBoxParameterAttachment> attachment;
    };
    std::vector<ShapeRow> shapes_;

    KhensuCopilotProcessor& processor_;
    PluginLookAndFeel lookAndFeel_;

    std::vector<KnobRow> knobs_;
    std::vector<ToggleRow> stageToggles_; // hpf, lpf, eq 1-3, comp, multiband, de-esser, limiter
    std::uint64_t lastAiMask_ = 0;
    /// Row rectangles from the last layout, for the separators.
    std::vector<juce::Rectangle<int>> rowBounds_;
    /// VIZ-6b: each row's module drawing (right of its knobs; empty = no room).
    std::vector<juce::Rectangle<int>> vizBounds_;
    void paintViz(juce::Graphics& g, int row, juce::Rectangle<float> r);
    float param(const char* id) const;
    /// Live reduction histories (newest last), filled by the timer.
    static constexpr size_t kHistory = 90;
    std::vector<float> limiterHistory_, deessHistory_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DspControlGridPanel)
};

} // namespace khensu
