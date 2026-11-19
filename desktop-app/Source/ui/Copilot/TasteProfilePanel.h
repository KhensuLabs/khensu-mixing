// desktop-app/Source/ui/Copilot/TasteProfilePanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include <khensu/feedback/LearnedPreferences.h>
#include <memory>
#include <vector>

namespace khensu {

class IAppController;

/// "What Khensu has learned": every learned taste offset (loudness, dynamics,
/// width, low end), with how far it moved from the genre default, the
/// accept/reject and A/B evidence behind it, and a plain-language sentence
/// ("You rejected 4 'too loud' warnings, so loudness targets now sit 0.8 dB
/// louder than the genre default."). Each offset can be reset on its own, or
/// all at once after a confirmation. Reset forgets only learned taste - UI
/// state, score weights and session data are untouched
/// (learned_preferences::reset).
// THREAD_CONTEXT(UIThread)
class TasteProfilePanel final : public juce::Component {
  public:
    explicit TasteProfilePanel(IAppController& c);
    ~TasteProfilePanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

    /// Re-reads the learned preferences off the UI thread.
    void refresh();

    /// Test seam: what is currently listed.
    const std::vector<LearnedPreference>& shownPreferences() const noexcept { return shown_; }

  private:
    class Row;

    void show(std::vector<LearnedPreference> prefs);
    void resetOne(const std::string& prefKey);
    void confirmResetAll();
    void resetAll();
    void layoutRows();

    IAppController& controller_;
    juce::Label headerLabel_;
    juce::Label introLabel_;
    juce::Label emptyLabel_;
    juce::TextButton resetAllButton_;
    juce::Viewport viewport_;
    juce::Component rowsHolder_;
    std::vector<std::unique_ptr<Row>> rows_;
    std::vector<LearnedPreference> shown_;

    /// Re-applies every cached palette colour on this panel. Called at
    /// construction and on each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TasteProfilePanel)
};

} // namespace khensu
