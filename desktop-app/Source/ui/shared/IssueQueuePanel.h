// desktop-app/Source/ui/shared/IssueQueuePanel.h
// The one issue queue (CON-1): every finding of the session with Fix, Mark
// done, Dismiss and - once a fixed version exists - what the check of that
// version found (FEAT-1). Open issues first, most severe first; settled ones
// (done, dismissed, resolved) below. The header carries the open count, the
// mix milestone the session is on (the Assistant screen's Milestones,
// moved here) and the latest check with "Check again".
//
// UI-1: above the list, the hero spectrum (HeroSpectrumView) draws every
// finding where it lives; clicking a mark selects its row and selecting a
// row highlights its mark. UI-2: one primary action per row (Fix, or Reopen
// once settled), everything else behind "..." and right-click; the selected
// row opens with the fix amount. OVH-6: the selected row predicts its fix
// from finding_evidence.json and can Preview it - an audition rendered
// through renders().renderPreview at the chosen amount and played as B in
// the A/B - before Keep renders the version through the queue.
//
// Reads and acts only through IIssues, so the Dashboard's notes and
// Mix/Master's list show the same state.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "KhensuEmptyState.h"
#include <map>
#include <memory>
#include <vector>
#include "../../services/IIssues.h"
#include "FixDifferenceView.h"
#include "HeroSpectrumView.h"
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
    bool keyPressed(const juce::KeyPress& key) override;

    /// Rebuilds the rows from IIssues (also on every queue change).
    void refresh();
    /// Fixes the top open issue (the header's / F key's "Fix top issue").
    void fixTopIssue();
    /// ENH-2: renders every open finding with a validated fix as one version.
    void fixAllSafe();

    /// Pure helpers (unit-tested).
    static juce::String stateLabel(const IIssues::Issue& issue);
    static juce::String headerSummary(const std::vector<IIssues::Issue>& issues);
    /// The row's headline: the finding's "what", else its engineer note,
    /// else its suggested action, else the issue code made readable.
    static juce::String titleFor(const IIssues::Issue& issue);
    /// "Severe" / "Moderate" / "Minor" (the severity system's words).
    static juce::String severityWord(float severity);
    /// The row's primary action label: "Fix", "Fix 60%", "Keep" (a preview
    /// of that amount is ready), "Reopen" (settled), or "" (none).
    static juce::String primaryLabel(const IIssues::Issue& issue, float amount, bool previewReady);

    /// NEW-10: where the monitoring choice is read on every refresh (the
    /// user's settings.json by default; tests set their own).
    void setMonitoringSource(std::function<std::string()> source) { monitoringSource_ = std::move(source); }
    /// The rows' notes, for tests: one entry per row, empty when it has none.
    juce::StringArray rowNotesForTesting() const;
    /// The selected issue's key (recId, or the code of a check-only finding).
    const std::string& selectedKey() const noexcept { return selectedKey_; }
    /// Selects an issue (as a click on its row or its mark does).
    void select(const std::string& key, bool scrollToRow);
    HeroSpectrumView& heroForTesting() noexcept { return hero_; }
    /// The fix amount (0..1) the issue's row is set to.
    float amountFor(const std::string& key) const;
    void setAmountForTesting(const std::string& key, float amount) { amounts_[key] = amount; }

  private:
    /// Queue changes rebuild the rows on the next message-loop turn: a
    /// change made from a row's own button must not delete that row while
    /// its click handler is still running.
    void handleAsyncUpdate() override
    {
        animateNextRefresh_ = true;
        refresh();
    }

    class Row;
    friend class Row;
    void layoutRows();
    void updateHero(bool animate);
    void updatePreviewCurve();
    void moveSelection(int delta);
    Row* rowFor(const std::string& key) const;
    std::function<std::string()> monitoringSource_;
    void runFix(const std::string& recId, float amount);
    /// UXR-9: "Issue dismissed - Undo" (reopens the issue on Undo).
    void showUndoToast(const juce::String& message, const std::string& recId);
    void runPrimary(const IIssues::Issue& issue);
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

    // ---- OVH-6: Preview before Keep -----------------------------------------
    struct Preview {
        enum class Stage { None, Rendering, Ready };
        Stage stage = Stage::None;
        std::string recId;
        float amount = 1.0f;
        juce::String path;
        juce::String description; ///< the render queue's label (to cancel it)
        uint64_t epoch = 0;
        int token = 0; ///< bumped on cancel / discard: a late result is ignored
    };
    Preview preview_;
    void togglePreview(const std::string& recId);
    void startPreview(const std::string& recId);
    void cancelPreview();
    /// Drops a ready preview: B goes back to the current version and the
    /// preview file is removed.
    void discardPreview();
    bool previewReadyFor(const std::string& recId, float amount) const noexcept;
    void amountChanged(const std::string& recId, float amount);

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
    KhensuEmptyState emptyState_; // UXR-6
    HeroSpectrumView hero_;
    bool referenceRequested_ = false; ///< VIZ-N1: measuring the reference once
    /// VIZ-3a: what the current version changed, measured (shown while a
    /// fixed version is current).
    FixDifferenceView difference_;
    juce::String differenceFor_;
    void updateDifference();
    juce::Viewport viewport_;
    juce::Component content_;
    std::vector<std::unique_ptr<Row>> rows_;
    std::string selectedKey_;
    std::map<std::string, float> amounts_;
    bool animateNextRefresh_ = false;

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
