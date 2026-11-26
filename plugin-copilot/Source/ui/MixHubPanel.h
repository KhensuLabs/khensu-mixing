#pragma once
#include <JuceHeader.h>
#include "../mixhub/MixHub.h"
#include "PluginLookAndFeel.h"

#include <array>
#include <vector>

namespace khensu {

class KhensuCopilotProcessor; // forward

/// "Mix Hub" tab: every Khensu Copilot instance in this process, the strongest
/// track-vs-track masking conflicts ("Kick vs Bass at 60 Hz"), and the summed
/// whole-mix balance. Reads MixHub::snapshot() on a 4 Hz timer that only runs
/// while the tab is shown; draws everything in paint() except the role menu
/// (PLG-18: the user can set this instance's role instead of the name guess),
/// and keeps a spoken summary in the component description for screen readers.
/// THREAD_CONTEXT(UIThread)
class MixHubPanel final : public juce::Component, private juce::Timer {
  public:
    explicit MixHubPanel(KhensuCopilotProcessor& processor);
    ~MixHubPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

    /// Re-reads the hub (the timer calls this with MixHub::nowMs()).
    void refresh(double nowMs);

    // Test/accessibility views of what is drawn.
    int getNumInstanceRows() const noexcept { return (int)frames_.size(); }
    std::vector<juce::String> getConflictLines() const;
    juce::String getSummaryText() const { return summaryText_; }

    /// Shown under the list: why a host may show fewer tracks than it has.
    static juce::String scopeNote();

    juce::ComboBox& getRoleBoxForTest() noexcept { return roleBox_; }

    /// ENH-10 (4): the Unmask button of conflict row `row` (shown only for a
    /// pair that includes this track).
    static constexpr int kMaxConflictsShown = 5;
    juce::TextButton& unmaskButtonForTest(int row) { return unmaskButtons_[static_cast<size_t>(row)]; }
    juce::String unmaskNoteForTest() const { return unmaskNote_; }

  private:
    void timerCallback() override { refresh(mixhub::MixHub::nowMs()); }
    void paintInstances(juce::Graphics& g, juce::Rectangle<int> area) const;
    void paintConflicts(juce::Graphics& g, juce::Rectangle<int> area) const;
    void paintBalance(juce::Graphics& g, juce::Rectangle<int> area) const;
    void layoutAreas();
    void updateUnmaskButtons();
    void onUnmaskClicked(int row);
    juce::Rectangle<int> conflictRow(int row) const;

    KhensuCopilotProcessor& processor_;
    std::vector<mixhub::TrackFrame> frames_;
    std::vector<mixhub::MaskingConflict> conflicts_;
    mixhub::MixSummary summary_;
    juce::String summaryText_;
    juce::ComboBox roleBox_;
    std::array<juce::TextButton, kMaxConflictsShown> unmaskButtons_;
    juce::String unmaskNote_;
    juce::Rectangle<int> instancesArea_, conflictsArea_, balanceArea_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixHubPanel)
};

} // namespace khensu
