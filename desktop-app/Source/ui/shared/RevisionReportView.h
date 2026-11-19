// desktop-app/Source/ui/shared/RevisionReportView.h
// CE-COL-1 / FEAT-9: "Compare revisions" - what changed between a before (A)
// and an after (B) render: findings fixed, improved, got worse or new, and
// every measurement that moved (POST /revision/report).
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h" // ThemeListener

namespace khensu {

class IBackgroundWork;

class RevisionReportView final : public juce::Component {
  public:
    RevisionReportView(IBackgroundWork& controller, juce::String aPath, juce::String bPath, juce::String aLabel,
                       juce::String bLabel);

    /// Open the report: in a CallOutBox pointing at `anchor` when given, else
    /// in a dialog window.
    static void show(IBackgroundWork& controller, juce::String aPath, juce::String bPath, juce::String aLabel,
                     juce::String bLabel, juce::Component* anchor);

    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    void fetch();
    void render();

    IBackgroundWork& controller_;
    juce::String aPath_, bPath_, aLabel_, bLabel_;
    juce::var report_;
    juce::String error_;
    bool busy_ = true;
    juce::TextEditor text_;

    void applyThemeColours();
    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        render();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RevisionReportView)
};

} // namespace khensu
