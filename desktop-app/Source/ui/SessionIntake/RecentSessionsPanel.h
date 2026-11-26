#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include <khensu/session/Session.h>
#include <functional>
#include <vector>

namespace khensu {

// THREAD_CONTEXT(UIThread)
// Displays recent sessions as horizontally scrolling 160px glass cards.
class RecentSessionsPanel final : public juce::Component {
  public:
    RecentSessionsPanel();
    ~RecentSessionsPanel() override = default;

    void setSessions(const std::vector<Session>& sessions);

    /// Set when the last load actually failed (a locked/corrupt database)
    /// rather than genuinely finding no sessions - shows a distinct message
    /// instead of the ambiguous "no sessions" empty state (audit CQ-05).
    void setLoadError(bool failed);

    std::function<void(const std::string&)> onSessionChosen;
    /// WF-12: a card's right-click menu (Rename / Version note / Duplicate).
    std::function<void(const Session&)> onRenameSession;
    std::function<void(const Session&)> onEditSessionNote;
    std::function<void(const Session&)> onDuplicateSession;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /// Tests: the cards shown and their size.
    int cardCountForTesting() const { return cards_.size(); }
    juce::Rectangle<int> cardBoundsForTesting(int i) const { return cards_[i]->getBounds(); }

  private:
    void showCardMenu(const Session& s);
    /// AUTO-5: sizes the cards to the viewport. setSessions() and resized()
    /// both call it - the cards were built 0 px tall when the sessions
    /// arrived after the last resize, so the card showed only its scrollbar.
    void layoutCards();
    juce::Label headerLabel;
    juce::Viewport scrollView_;
    juce::Component cardContainer_;
    std::vector<Session> sessions_;
    juce::OwnedArray<juce::Component> cards_;
    bool loadFailed_ = false;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RecentSessionsPanel)
};

} // namespace khensu
