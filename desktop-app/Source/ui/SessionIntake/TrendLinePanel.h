// desktop-app/Source/ui/SessionIntake/TrendLinePanel.h
#pragma once
#include <JuceHeader.h>
#include <khensu/session/SessionManager.h>
#include <functional>
#include <vector>

namespace khensu {

// THREAD_CONTEXT(UIThread)
// Plots small multi-metric sparklines (LUFS, energy, hook strength, true
// peak, dynamic range, mastering readiness) across the user's last N
// sessions so trends (getting louder/more energetic over time, readiness
// improving, etc.) are visible at a glance on the intake screen. Readiness
// reads as 0 for sessions saved before this field existed (sentinel -1 in
// AnalysisSummary), same as any other metric that simply has no data yet.
class TrendLinePanel final : public juce::Component {
  public:
    TrendLinePanel();
    ~TrendLinePanel() override = default;

    void setSummaries(std::vector<SessionManager::AnalysisSummary> summaries);

    /// Sets the currently-selected Session Intake genre, used by the
    /// "This genre" cohort filter option.
    void setCurrentGenre(const juce::String& genre);

    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    enum class CohortFilter { All, ThisGenre, ThisClient, CustomRange };

    std::vector<SessionManager::AnalysisSummary> allSummaries_; // unfiltered, newest first
    std::vector<SessionManager::AnalysisSummary> summaries_;    // filtered view actually drawn
    juce::String currentGenre_;

    juce::ComboBox filterCombo_;
    juce::TextEditor clientQueryEditor_;
    juce::TextEditor rangeStartEditor_, rangeEndEditor_; // YYYY-MM-DD, compared against computed_at

    void applyFilter();

    void drawSparkline(juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& label, juce::Colour colour,
                       float minVal, float maxVal,
                       std::function<float(const SessionManager::AnalysisSummary&)> extract) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrendLinePanel)
};

} // namespace khensu
