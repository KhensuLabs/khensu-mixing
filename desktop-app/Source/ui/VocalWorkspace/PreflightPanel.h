// desktop-app/Source/ui/VocalWorkspace/PreflightPanel.h
// Recording Preflight Advisor: run a short test recording through the
// source-triage checks BEFORE the real take - level, noise floor, room
// reflections, hum, proximity - plus the placement fundamentals as a
// checklist. The vocal is ~80% capture; every problem caught here is an
// hour not spent mixing around it.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../shared/PanelRunState.h"
#include <vector>

namespace khensu {

class IAppController;

class PreflightPanel final : public PanelCard {
  public:
    explicit PreflightPanel(IAppController& c);
    void resized() override;

  private:
    void chooseTestRecording();
    void runPreflight(const juce::String& path);

    struct CheckRow final : public juce::Component {
        void paint(juce::Graphics& g) override;
        juce::String title, detail;
        int state = 0; // 0 = unknown, 1 = pass, 2 = warn, 3 = fail
    };

    juce::TextButton chooseButton_{TRANS("Check Test Recording...")};
    /// Status line, error state and the choose button's busy state (UX-8).
    PanelRunState run_{this, &chooseButton_};
    juce::String lastPath_; ///< the test recording the retry re-scans

    juce::Viewport viewport_;
    juce::Component content_;
    std::vector<std::unique_ptr<CheckRow>> rows_;

    std::unique_ptr<juce::FileChooser> chooser_;

    void buildDefaultChecklist();
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PreflightPanel)
};

} // namespace khensu
