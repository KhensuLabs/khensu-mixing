// desktop-app/Source/ui/Settings/MacroMappingPanel.h
// Editor for MIDI/OSC -> vocal-macro mappings: one row per macro with a Learn
// button (arm, then move a hardware control or send an OSC message), the
// bound source, output range (min/max), response curve, and invert. Plus the
// OSC receiver toggle/port. Every edit persists immediately.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include <array>
#include <memory>

namespace khensu {

class IAppController;
class MacroControlRouter;

class MacroMappingPanel final : public juce::Component {
  public:
    explicit MacroMappingPanel(IAppController& c);
    ~MacroMappingPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /// A resizable window hosting the editor. AppController owns the single
    /// instance (see AppController::showMacroMappingWindow) so it can never
    /// outlive the controller it edits.
    static std::unique_ptr<juce::DocumentWindow> createWindow(IAppController& c);

  private:
    static constexpr int kRows = 7;

    struct Row {
        juce::Label name;
        juce::Label source;
        juce::TextButton learn{TRANS("Learn")};
        juce::Slider minSlider{juce::Slider::LinearHorizontal, juce::Slider::TextBoxLeft};
        juce::Slider maxSlider{juce::Slider::LinearHorizontal, juce::Slider::TextBoxLeft};
        juce::ComboBox curve;
        juce::ToggleButton invert{TRANS("Invert")};
        juce::TextButton clear{TRANS("Clear")};
    };

    void refresh();
    void commitRow(int row);
    int mappingIndexFor(int row) const;

    IAppController& controller_;
    MacroControlRouter& router_;
    std::array<Row, kRows> rows_;
    juce::Label header_;
    juce::ToggleButton oscToggle_{TRANS("Receive OSC (UDP)")};
    juce::TextEditor oscPort_;
    juce::ToggleButton lanToggle_{TRANS("Allow other devices (LAN)")};
    juce::Label oscNote_;
    juce::Label status_;
    bool refreshing_ = false;
    bool learnArmed_ = false; ///< a Learn is pending (state, not label text)

    void applyThemeColours();
    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MacroMappingPanel)
};

} // namespace khensu
