#pragma once
#include <JuceHeader.h>
#include "PluginLookAndFeel.h"
#include <cstdint>

namespace khensu {

class KhensuCopilotProcessor;

// "Chain" tab: exposes every built-in DSP-chain parameter (HPF/LPF, 3 EQ
// bands, single and multiband compressor with crossovers, output gain, chain
// mix, limiter) as rotary knobs / toggle buttons, each with a small dot that
// lights up while KhensuCopilotProcessor::isAiSet() is true for that
// parameter. No bypass control here - PluginEditor's header bypassButton_
// covers global bypass (ParamIDs::bypass).
class DspControlGridPanel final : public juce::Component, private juce::Timer {
  public:
    explicit DspControlGridPanel(KhensuCopilotProcessor& proc);
    ~DspControlGridPanel() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;

    int getNumKnobsForTest() const noexcept { return static_cast<int>(knobs_.size()); }
    int getNumTogglesForTest() const noexcept { return static_cast<int>(stageToggles_.size()); }

  private:
    void timerCallback() override; // 10 Hz while shown - repaints when an AI-set dot changes

    // One row: an optional on/off toggle + one continuous rotary slider
    // (or just a toggle, for e.g. limiter - thresh has its own row).
    /// Rotary slider whose editable value box carries a spoken name. JUCE
    /// recreates that box on every look-and-feel change, so the name is
    /// reapplied there rather than set once (audit AUD-UX-01).
    struct NamedSlider final : public juce::Slider {
        NamedSlider() : juce::Slider(RotaryHorizontalVerticalDrag, TextBoxBelow) {}
        void nameValueBox()
        {
            for (auto* child : getChildren())
                if (auto* box = dynamic_cast<juce::Label*>(child)) box->setTitle(getTitle() + " value");
        }
        void lookAndFeelChanged() override
        {
            juce::Slider::lookAndFeelChanged();
            nameValueBox();
        }
    };

    struct KnobRow {
        juce::String paramId;
        std::unique_ptr<NamedSlider> slider;
        std::unique_ptr<juce::SliderParameterAttachment> sliderAttachment;
        std::unique_ptr<juce::Label> label;
    };
    struct ToggleRow {
        juce::String paramId;
        std::unique_ptr<juce::ToggleButton> button;
        std::unique_ptr<juce::ButtonParameterAttachment> attachment;
    };

    void addKnob(const char* paramId, const juce::String& labelText);
    void addToggle(const char* paramId, const juce::String& labelText, std::vector<ToggleRow>& into);

    KhensuCopilotProcessor& processor_;
    PluginLookAndFeel lookAndFeel_;

    std::vector<KnobRow> knobs_;
    std::vector<ToggleRow> stageToggles_; // hpf, lpf, eq 1-3, comp, multiband, limiter
    std::uint64_t lastAiMask_ = 0;

    // No in-grid bypass control: the header's existing bypassButton_
    // (PluginEditor.h, wired to ParamIDs::bypass via ButtonAttachment in
    // commit 380e6ed) already covers global bypass; this panel doesn't
    // duplicate it.

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DspControlGridPanel)
};

} // namespace khensu
