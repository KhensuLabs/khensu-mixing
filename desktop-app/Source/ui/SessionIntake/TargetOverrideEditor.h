// desktop-app/Source/ui/SessionIntake/TargetOverrideEditor.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/ModalCardHost.h"
#include <khensu/targets/GenreEntry.h>
#include <khensu/targets/UserPreferenceModel.h>

namespace khensu {

class IAppController;

/// Genre-baseline + custom-delta editor for the four UserPreferenceModel
/// offsets (LUFS/LRA/stereo width/low-end weight). This is the same model
/// UserPreferenceModel already learns from accept/reject feedback - this
/// editor lets the user set/inspect it directly instead of only implicitly.
/// Modal overlay reachable from Session Intake's "Target Overrides" button;
/// doubles as the "Advanced Preferences" entry point referenced in the
/// backlog since the app has no separate Preferences screen yet.
class TargetOverrideEditor final : public ModalCardHost, private juce::Timer {
  public:
    TargetOverrideEditor(IAppController& controller, const GenreEntry& genre);
    ~TargetOverrideEditor() override;

    /// How many times the preferences were written (tests).
    int savesForTesting() const noexcept { return saves_; }
    /// Moves a row's slider as a user drag would (tests): index 0-3.
    void setRowValueForTesting(int row, double value);

    void resized() override;

    std::function<void()> onClose;

  protected:
    juce::Rectangle<int> cardBounds() const override { return card_.getBounds(); }
    void dismiss() override
    {
        if (onClose) onClose();
    }

  private:
    void refreshLabels();
    /// Slider edits save once they settle (500 ms) or on drag end, not on
    /// every value of a drag - each save is a database write.
    void scheduleSave() { startTimer(500); }
    void saveNow();
    void timerCallback() override { saveNow(); }
    int saves_ = 0;

    IAppController& controller_;
    GenreEntry genre_;
    UserPreferenceModel prefs_;

    juce::Component card_;
    juce::Label titleLabel_;
    juce::TextButton closeButton_{TRANS("Close")};
    juce::TextButton resetButton_{TRANS("Reset to Genre Default")};

    struct Row {
        juce::Label label;
        juce::Slider slider;
        juce::Label baselineValue;
    };
    Row lufsRow_, lraRow_, widthRow_, lowEndRow_;

    void setupRow(Row& row, const juce::String& name, double minV, double maxV, float initial,
                  std::function<void(float)> onChange);
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TargetOverrideEditor)
};

} // namespace khensu
