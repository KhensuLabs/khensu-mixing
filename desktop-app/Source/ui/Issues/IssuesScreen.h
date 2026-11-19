// desktop-app/Source/ui/Issues/IssuesScreen.h
#pragma once
#include <JuceHeader.h>
#include "../shared/ScreenHeader.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/IssueQueuePanel.h"

namespace khensu {

class IAppController;

/// The Issues screen (sidebar "Issues"; CON-1): the
/// one issue queue - every finding with Fix, Mark done, Dismiss and the
/// check of the fixed version (FEAT-1), the mix milestones in its header.
/// It replaced the Guided Assistant's suggestion list and milestone rail;
/// the Dashboard's notes and Mix/Master's list act through the same queue.
/// THREAD_CONTEXT(UIThread)
class IssuesScreen final : public juce::Component {
  public:
    explicit IssuesScreen(IAppController& controller);
    ~IssuesScreen() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

    /// "Fix top issue" (header button, F key, palette): fixes the most
    /// severe open issue that has a one-click fix, here in the queue.
    void fixTopIssue();

  private:
    void updateEmptyState();

    IAppController& controller_;

    ScreenHeader titleLabel_; // UX-3: title = the sidebar label
    juce::TextButton fixTopButton_{TRANS("Fix top issue")}; // the header's primary (UX-3)
    juce::Label emptySessionLabel_;
    juce::TextButton loadTrackButton_; // UX-5: the empty state's one action
    IssueQueuePanel queue_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IssuesScreen)
};

} // namespace khensu
