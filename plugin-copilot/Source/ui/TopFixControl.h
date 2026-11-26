// plugin-copilot/Source/ui/TopFixControl.h
// OVH-8: "Apply top fix" - the open finding with the highest severity whose
// fix the built-in chain can do, one button to apply it live and an Amount
// knob (the automatable fix_amount parameter) that re-applies it as it
// turns. Used on the Suggest tab and in the Compact view.
#pragma once
#include <JuceHeader.h>
#include <khensu/feedback/IssueState.h>
#include <khensu/recommendations/Recommendation.h>
#include <khensu/ui/KhensuKnob.h>

#include <functional>
#include <optional>
#include <vector>

namespace khensu {
class KhensuCopilotProcessor;

namespace copilot {
using IssueStateLookup = std::function<issue_state::State(const std::string& recId)>;
/// Open findings (not done or dismissed, severity > 0), highest severity
/// first, at most `limit`.
std::vector<Recommendation> topFindings(const std::vector<Recommendation>& recs, const IssueStateLookup& stateOf,
                                        size_t limit);
/// The highest-severity open finding whose fix maps onto the built-in chain.
std::optional<Recommendation> selectTopFix(const std::vector<Recommendation>& recs, const IssueStateLookup& stateOf);
/// ENH-10: how many of `open` (most severe first) Apply all would apply.
int countSafeFixes(const std::vector<Recommendation>& open);
} // namespace copilot

// THREAD_CONTEXT(UIThread)
class TopFixControl final : public juce::Component {
  public:
    explicit TopFixControl(KhensuCopilotProcessor& processor);
    ~TopFixControl() override;

    /// The session's findings changed (or their states did).
    void setRecommendations(const std::vector<Recommendation>& recs, copilot::IssueStateLookup stateOf);
    /// Called with the top fix when Apply is pressed; returns true if applied.
    std::function<bool(const Recommendation&)> onApply;
    /// ENH-10: called with every open finding (most severe first) when
    /// "Apply all safe" is pressed; returns true if anything was applied.
    std::function<bool(const std::vector<Recommendation>&)> onApplyAll;
    const std::vector<Recommendation>& openFindings() const noexcept { return openFindings_; }
    int safeFixCount() const noexcept { return safeFixCount_; }

    const std::optional<Recommendation>& topFix() const noexcept { return topFix_; }
    /// Refreshes the "live" marker (the editor's timer).
    void refresh();
    /// Compact layout: everything in one row.
    void setCompact(bool compact);

    juce::Button& applyButton() noexcept { return applyButton_; }
    juce::Button& applyAllButton() noexcept { return applyAllButton_; }
    ui::KhensuKnob& amountKnob() noexcept { return amountKnob_; }
    juce::String findingTextForTesting() const { return findingLabel_.getText(); }
    juce::String reasonTextForTesting() const { return reasonLabel_.getText(); }

    void paint(juce::Graphics&) override;
    void resized() override;

  private:
    KhensuCopilotProcessor& processor_;
    std::optional<Recommendation> topFix_;
    juce::Label titleLabel_, findingLabel_, reasonLabel_;
    juce::TextButton applyButton_;
    juce::TextButton applyAllButton_;
    std::vector<Recommendation> openFindings_;
    int safeFixCount_ = 0;
    ui::KhensuKnob amountKnob_{"Amount"};
    std::unique_ptr<juce::SliderParameterAttachment> amountAttachment_;
    bool compact_ = false;
    void updateTexts();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TopFixControl)
};

/// Compact view: the top three open findings, one line each.
class TopFindingsList final : public juce::Component {
  public:
    TopFindingsList();
    void setFindings(std::vector<Recommendation> findings);
    int numFindings() const noexcept { return static_cast<int>(findings_.size()); }
    void paint(juce::Graphics&) override;

  private:
    std::vector<Recommendation> findings_;
};

} // namespace khensu
