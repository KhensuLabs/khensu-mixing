// desktop-app/Source/ui/shared/IssueQueuePanel.h
// The one issue queue (CON-1): every finding of the session with Fix, Mark
// done, Dismiss and - once a fixed version exists - what the check of that
// version found (FEAT-1). Open issues first, most severe first; settled ones
// (done, dismissed, resolved) below. The header carries the open count, the
// mix milestone the session is on (the Assistant screen's Milestones,
// moved here) and the latest check with "Check again".
// Reads and acts only through IIssues, so the Dashboard's notes and
// Mix/Master's list show the same state.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <memory>
#include <vector>
#include "../../services/IIssues.h"
#include "KhensuTheme.h" // ThemeListener
#include "ListenerHandle.h"
#include "PanelCard.h"

namespace khensu {

class IssueQueuePanel final : public PanelCard, private juce::AsyncUpdater {
  public:
    explicit IssueQueuePanel(IAppController& controller);
    ~IssueQueuePanel() override;

    void resized() override;
    void paint(juce::Graphics& g) override;

    /// Rebuilds the rows from IIssues (also on every queue change).
    void refresh();
    /// Fixes the top open issue (the header's / F key's "Fix top issue").
    void fixTopIssue();

    /// Pure helpers (unit-tested).
    static juce::String stateLabel(const IIssues::Issue& issue);
    static juce::String headerSummary(const std::vector<IIssues::Issue>& issues);
    /// The row's headline: the finding's "what", else its engineer note,
    /// else its suggested action, else the issue code made readable.
    static juce::String titleFor(const IIssues::Issue& issue);

    /// NEW-10: where the monitoring choice is read on every refresh (the
    /// user's settings.json by default; tests set their own).
    void setMonitoringSource(std::function<std::string()> source) { monitoringSource_ = std::move(source); }
    /// The rows' notes, for tests: one entry per row, empty when it has none.
    juce::StringArray rowNotesForTesting() const;

  private:
    /// Queue changes rebuild the rows on the next message-loop turn: a
    /// change made from a row's own button must not delete that row while
    /// its click handler is still running.
    void handleAsyncUpdate() override { refresh(); }

    class Row;
    void layoutRows();
    std::function<std::string()> monitoringSource_;
    void runFix(const std::string& recId, juce::Button* button);
    /// NEW-4: Stronger / Gentler on an improved-but-open fix.
    void runRefine(const std::string& recId, bool stronger);
    /// NEW-4: loop the regions that changed between the current version and
    /// the one it was rendered from (each press: the next region).
    void hearWhatChanged();
    void playChangedRegion();
    juce::TextButton hearButton_{TRANS("Hear what changed")};
    std::vector<std::pair<double, double>> changedRegions_;
    juce::String changedFor_;
    int changedIndex_ = 0;
    void setStatus(const juce::String& text, bool error = false);

    juce::Label summaryLabel_;
    juce::Label milestoneLabel_;
    juce::Label checkLabel_;
    juce::TextButton checkAgainButton_{TRANS("Check again")};
    /// WF-2 in the queue: hear the fixed version against the original in
    /// place (B key does the same anywhere).
    juce::TextButton abButton_{TRANS("Hear A/B")};
    // FEAT-8: step through the timed issues, looping each one's region.
    juce::TextButton prevTimedButton_{TRANS("< Previous")};
    juce::TextButton nextTimedButton_{TRANS("Next >")};
    int playlistPos_ = -1;
    void stepPlaylist(bool forward);
    void playIssue(const IIssues::Issue& issue);
    void updateAbButton();
    juce::Label statusLabel_;
    juce::Label emptyLabel_;
    juce::Viewport viewport_;
    juce::Component content_;
    std::vector<std::unique_ptr<Row>> rows_;

    void applyThemeColours();

    // ENG-2: deregistered before the members the callbacks touch.
    ListenerHandle issuesListener_;
    ListenerHandle verificationListener_;

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IssueQueuePanel)
};

} // namespace khensu
