#pragma once
#include <JuceHeader.h>
#include <khensu/feedback/IssueState.h>
#include <khensu/recommendations/Recommendation.h>
#include <functional>
#include <vector>

namespace khensu {

class SuggestionRow final : public juce::Component {
  public:
    SuggestionRow();
    void setRecommendation(const Recommendation& rec);
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseUp(const juce::MouseEvent& e) override;

    /// The finding's issue-queue state (CON-1): open, done or dismissed.
    void setIssueState(issue_state::State state) noexcept
    {
        state_ = state;
        repaint();
    }
    issue_state::State getIssueState() const noexcept { return state_; }
    /// Called with the new state when the user changes it here: the marker
    /// toggles open -> done and done / dismissed -> open; Auto-Apply marks done.
    void setOnIssueStateChanged(std::function<void(issue_state::State)> cb) { on_state_changed_ = std::move(cb); }
    /// Returns true when the fix was applied - only then is the row marked done.
    void setOnAutomateClicked(std::function<bool(const Recommendation&)> cb) { on_automate_clicked_ = std::move(cb); }
    void setOnRowClicked(std::function<void()> cb) { on_row_clicked_ = std::move(cb); }

    static constexpr int kRowHeight = 72;

  private:
    Recommendation rec_;
    juce::String typeLabel_;
    juce::String whatText_;
    juce::String actionText_;
    float severity_ = 0.0f;

    issue_state::State state_ = issue_state::State::Open;
    std::function<void(issue_state::State)> on_state_changed_;
    void changeState(issue_state::State next);
    std::function<bool(const Recommendation&)> on_automate_clicked_;
    std::function<void()> on_row_clicked_;
    juce::TextButton automateButton;
    juce::TextButton copyEnvelopeButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SuggestionRow)
};

class SuggestionFeedPanel final : public juce::Component {
  public:
    SuggestionFeedPanel();
    void resized() override;
    void paint(juce::Graphics& g) override;

    using StateLookup = std::function<issue_state::State(const std::string& rec_id)>;
    void setData(const std::vector<Recommendation>& recs, StateLookup stateOf = nullptr);
    void clearData();
    void setLoading(bool loading);

    void setOnIssueStateChanged(std::function<void(const std::string& rec_id, issue_state::State state)> cb)
    {
        on_state_changed_ = std::move(cb);
    }
    /// Returns true when the fix was applied (the row is then marked done).
    void setOnAutomateSuggestion(std::function<bool(const Recommendation& rec)> cb)
    {
        on_automate_suggestion_ = std::move(cb);
    }
    void setOnRowClicked(std::function<void(const Recommendation& rec)> cb) { on_row_clicked_ = std::move(cb); }

  private:
    juce::Viewport viewport_;
    juce::Component listContent_;
    std::vector<std::unique_ptr<SuggestionRow>> rows_;
    bool loading_ = false;
    juce::String emptyText_ = "No analysis loaded";

    std::function<void(const std::string&, issue_state::State)> on_state_changed_;
    std::function<bool(const Recommendation&)> on_automate_suggestion_;
    std::function<void(const Recommendation&)> on_row_clicked_;

    void rebuildRows(const std::vector<Recommendation>& recs, StateLookup stateOf = nullptr);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SuggestionFeedPanel)
};

} // namespace khensu
