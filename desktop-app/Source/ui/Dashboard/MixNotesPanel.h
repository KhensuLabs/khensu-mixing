// desktop-app/Source/ui/Dashboard/MixNotesPanel.h
#pragma once
#include "../shared/PanelCard.h"
#include "../shared/ListenerHandle.h"
#include "../shared/RecommendationCard.h"
#include "../shared/SeverityLegendComponent.h"
#include "../shared/MixNotesTimeline.h"
#include "../shared/KhensuEmptyState.h"
#include <khensu/analysis/AnalysisResult.h>
#include <vector>
#include <memory>
#include <map>
#include <string>

namespace khensu {

class MixNotesPanel final : public PanelCard {
  public:
    explicit MixNotesPanel(IAppController& c);
    ~MixNotesPanel() override;
    void setData(const AnalysisResult& r);
    void resized() override;

  private:
    void sortAndRepopulate();
    void layoutCards();

    /// "Apply" on a card: the issue queue renders the fix (CON-1), so its
    /// state, the taste learning and the fix check are the queue's.
    void applyFix(RecommendationCard& card, const Recommendation& rec);

    juce::Viewport viewport_;
    juce::Component content_;
    KhensuEmptyState emptyState_;
    juce::TextButton sortToggle_{TRANS("Sort: Severity")};
    bool sortBySeverity_ = true;
    SeverityLegendComponent legend_; ///< colour-scheme key (spec §4)
    MixNotesTimeline timeline_;      ///< time-anchored notes strip (spec §4)
    juce::Label evidenceLabel_;      ///< causal chains from /analyze/evidence_graph (pipeline #7)

    std::vector<Recommendation> recs_;

    // CE-AI-2 personal fix model: rank and "ranked for you because ..." per
    // recommendation id, from POST /taste/model/rank. Empty on cold start, so
    // the panel keeps its normal order.
    std::map<std::string, int> personalRank_;
    std::map<std::string, juce::String> personalReason_;
    void requestPersonalRanking();
    std::vector<std::unique_ptr<RecommendationCard>> cards_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // ENG-2: hub registrations; destroyed (deregistered) before the members
    // above them that the callbacks touch. A finding fixed, done or
    // dismissed elsewhere (the Issues screen, Mix/Master) re-reads here.
    ListenerHandle issuesListener_;

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixNotesPanel)
};

} // namespace khensu
