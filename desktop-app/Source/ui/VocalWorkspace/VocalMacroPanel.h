// desktop-app/Source/ui/VocalWorkspace/VocalMacroPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"

namespace khensu {

// THREAD_CONTEXT(UIThread)
class VocalMacroPanel final : public PanelCard {
  public:
    explicit VocalMacroPanel(IAppController& c);
    ~VocalMacroPanel() override;
    void setStrategy(const juce::String& strategyId, bool forcePresetDefault = false);
    void resized() override;

  private:
    static constexpr int kMacroCount = 7;
    static const char* kMacroNames[kMacroCount];

    juce::Slider sliders_[kMacroCount];
    juce::Label labels_[kMacroCount];
    juce::TextButton resetButton_{TRANS("Reset to Suggested")};
    juce::TextButton learnButton_{TRANS("MIDI / OSC Learn...")};

    struct MacroPreset {
        int values[kMacroCount];
    };
    static MacroPreset presetForStrategy(const juce::String& id) noexcept;

    juce::String currentStrategyId_{"lead_pop"};
    bool isInitialized_{false};
    void updateLabelText(int idx);
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VocalMacroPanel)
};

} // namespace khensu
