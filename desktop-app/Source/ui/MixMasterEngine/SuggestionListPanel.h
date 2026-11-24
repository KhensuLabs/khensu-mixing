// desktop-app/Source/ui/MixMasterEngine/SuggestionListPanel.h
// Mix / Master's recommendations, in the Issues queue's row style (UX2-2): a
// slim severity bar, the finding as the title, the severity word and stage as
// the meta line, and one action. The right rail shows the top three open
// findings; "..." lists them all in signal-chain order (earliest stage first,
// so upstream fixes come before the work that depends on them).
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../shared/ListenerHandle.h"
#include <khensu/analysis/AnalysisResult.h>
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace khensu {

namespace suggestion_order {

/// Indices of the `count` most severe findings not yet settled (ties keep
/// the analysis order). `settled` maps a recommendation id to whether the
/// issue queue counts it as handled.
inline std::vector<size_t> topOpen(const std::vector<Recommendation>& recs, const std::map<std::string, bool>& settled,
                                   size_t count)
{
    std::vector<size_t> open;
    for (size_t i = 0; i < recs.size(); ++i) {
        const auto it = settled.find(recs[i].id);
        if (it == settled.end() || !it->second) open.push_back(i);
    }
    std::stable_sort(open.begin(), open.end(),
                     [&](size_t a, size_t b) { return recs[a].severity > recs[b].severity; });
    if (open.size() > count) open.resize(count);
    return open;
}

/// Every finding in signal-chain order (earliest stage first, severity breaks ties).
inline std::vector<size_t> chainOrder(const std::vector<Recommendation>& recs)
{
    std::vector<size_t> order(recs.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (recs[a].chain_position != recs[b].chain_position) return recs[a].chain_position < recs[b].chain_position;
        return recs[a].severity > recs[b].severity;
    });
    return order;
}

/// The one severity system: red severe, amber moderate, blue minor.
inline juce::String severityWord(float severity)
{
    if (severity >= 66.0f) return TRANS("Severe");
    if (severity >= 33.0f) return TRANS("Moderate");
    return TRANS("Minor");
}

} // namespace suggestion_order

// THREAD_CONTEXT(UIThread)
class SuggestionListPanel final : public PanelCard {
  public:
    explicit SuggestionListPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void resized() override;

    /// Keyboard-driven "next issue": scrolls to and briefly highlights the
    /// next unresolved recommendation in severity order (wraps around).
    /// Returns false if there's nothing unresolved to jump to.
    bool focusNextIssue();

    /// The rail's compact view: the `count` most severe open findings, with
    /// "..." for the rest. 0 = every finding.
    void setTopCount(int count);
    bool showingAll() const noexcept { return showAll_ || topCount_ <= 0; }
    void setShowAll(bool all);
    /// The compact / full view toggled (the screen gives the list more room).
    std::function<void()> onShowAllChanged;
    /// Rows on screen (tests).
    int visibleRowCount() const noexcept { return static_cast<int>(rows_.size()); }
    /// Height that shows the compact view without scrolling.
    int preferredCompactHeight() const;

  private:
    struct GroupHeader final : public juce::Component {
        explicit GroupHeader(const juce::String& text);
        void paint(juce::Graphics& g) override;
        juce::String text_;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GroupHeader)
    };

    struct RecommendationRow final : public juce::Component, public juce::SettableTooltipClient {
        RecommendationRow();
        void setData(const Recommendation& rec, bool isActedOn, bool isFixFirst, bool primary);
        void paint(juce::Graphics& g) override;
        void resized() override;
        void mouseUp(const juce::MouseEvent& e) override
        {
            if (onRowClicked && e.mouseWasClicked()) onRowClicked();
        }
        bool keyPressed(const juce::KeyPress& key) override;
        void focusGained(FocusChangeType) override { repaint(); }
        void focusLost(FocusChangeType) override { repaint(); }
        std::string recId_;
        /// VIZ-U4: the row's height at `width` - the whole title, wrapped
        /// (up to four lines), never cut off.
        int heightFor(int width) const;
        juce::TextLayout titleLayout(float width) const;

        /// Brief highlight pulse used by the keyboard "next issue" command.
        void setHighlighted(bool h)
        {
            highlighted_ = h;
            repaint();
        }
        bool highlighted_ = false;

        juce::TextButton resolveButton{TRANS("Use this")};
        juce::TextButton applyButton{TRANS("Fix")};
        std::function<void()> onResolvePressed;
        std::function<void()> onApplyPressed; ///< the fix, through the issue queue (CON-1)
        std::function<void()> onRowClicked;   ///< click -> why (chain reasoning)
        Recommendation rec_;
        bool isActedOn_ = false;
        bool isFixFirst_ = false;
        bool hasConflict_ = false;

        /// Transient feedback for the apply button - the next setData()
        /// rebuild (the new render changes the analysis) replaces it.
        void setApplyInFlight(bool inFlight)
        {
            applyButton.setEnabled(!inFlight);
            applyButton.setButtonText(inFlight ? TRANS("Fixing...") : TRANS("Fix"));
        }
        void setApplyResult(bool success)
        {
            applyButton.setButtonText(success ? TRANS("Fixed") : TRANS("Failed"));
            applyButton.setEnabled(false);
        }
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RecommendationRow)
    };

    void rebuild();
    void layoutRows();

    juce::Viewport viewport_;
    juce::Component listContent_;
    std::vector<std::unique_ptr<RecommendationRow>> rows_;
    std::vector<std::unique_ptr<GroupHeader>> headers_;
    juce::Label emptyLabel_;
    juce::Label footerLabel_;
    juce::TextButton moreButton_{"..."}; // i18n-exempt(symbol; named by setTitle)

    std::vector<Recommendation> lastRecs_; // cached for focusNextIssue() cycling
    int nextIssueIndex_ = -1;
    int topCount_ = 0;
    bool showAll_ = false;

    static constexpr int kRowH = 58;
    static constexpr int kRowGap = 6;
    static constexpr int kGroupHeaderH = 24;
    static constexpr int kFooterH = 28;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // ENG-2: the issue-queue registration, deregistered before the members it touches.
    ListenerHandle issuesListener_;

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SuggestionListPanel)
};

} // namespace khensu
