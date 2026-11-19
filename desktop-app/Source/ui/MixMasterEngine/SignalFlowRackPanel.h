// desktop-app/Source/ui/MixMasterEngine/SignalFlowRackPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/ListenerHandle.h"
#include "../shared/PanelCard.h"
#include "../shared/PluginTargetChooser.h"
#include <memory>
#include <map>

namespace khensu {

class IAppController;

class SignalFlowRackPanel final : public PanelCard, private juce::Timer {
  public:
    explicit SignalFlowRackPanel(IAppController& c);
    ~SignalFlowRackPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

  private:
    void timerCallback() override; // Polls parameters from local service
    void loadParamsFromServer();
    void pushParamsToServer();

    struct DspSlider {
        juce::String paramId;
        std::unique_ptr<juce::Slider> slider;
        std::unique_ptr<juce::Label> label;
    };

    struct DspToggle {
        juce::String paramId;
        std::unique_ptr<juce::ToggleButton> button;
    };

    void addSlider(const char* paramId, const juce::String& name, float min, float max, float defVal,
                   bool isLog = false, const juce::String& suffix = "");
    void addToggle(const char* paramId, const juce::String& name);

    juce::Slider* getSlider(const char* paramId) const;
    juce::ToggleButton* getToggle(const char* paramId) const;

    bool isUserInteracting() const;

    juce::Viewport viewport_;
    juce::Component container_;

    std::vector<DspSlider> sliders_;
    std::vector<DspToggle> toggles_;

    std::map<juce::String, float> currentParams_;
    bool isUpdatingFromPoll_ = false;
    /// Which plugin instance the pushes address (PLG-04); default All.
    std::unique_ptr<PluginTargetChooser> targetChooser_;
    /// WF-9: a chain another surface staged (Mixer Chain, onboarding), shown
    /// here - the plugin chain's one editor - to apply or discard.
    juce::Label stagedLabel_;
    juce::TextButton applyStagedButton_{TRANS("Apply to plugin")};
    juce::TextButton discardStagedButton_{TRANS("Discard")};
    void showStaged();
    ListenerHandle chainListener_;
    /// Re-applies every cached palette colour, walking the runtime-built
    /// slider/toggle vectors. Called when a control is added and on each
    /// theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SignalFlowRackPanel)
};

} // namespace khensu
