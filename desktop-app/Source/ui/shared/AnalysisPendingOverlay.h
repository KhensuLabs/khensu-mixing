// desktop-app/Source/ui/shared/AnalysisPendingOverlay.h
// Covers the result screens (Dashboard, AI Detection, Vocal, Mix/Master,
// System Brain, Export, Copilot, Assistant) while an analysis is running.
// Findings and grades are withheld until the main analysis has finished
// (deep, sections, drums, reference, chords, trust - everything a grade is
// built from): a readiness score or verdict that moves as stages land reads
// as the app changing its mind. The Dashboard's follow-up scans can't change
// a grade, so they fill in afterwards, each panel with its own placeholder
// (PERF-1). Meanwhile the fast pass's loudness, true peak and clipping show
// here at once, marked provisional. Shows the current stage and overall
// progress, with Cancel; after a cancel or failure it offers a re-run.
// Colours are read live in paint(), so theme switches need no listener.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <functional>
#include <string>

namespace khensu {

class AnalysisPendingOverlay final : public juce::Component {
  public:
    enum class Mode { Analyzing, Cancelled, Failed };

    AnalysisPendingOverlay();

    void setMode(Mode mode, const juce::String& detail = {});
    Mode mode() const noexcept { return mode_; }

    /// AnalysisController progress ("fast", "deep_analysis:3:8", "sections",
    /// "drums", "reference", "chords"); idx/total are the coarse 1..6 stages.
    void updateStage(const std::string& stage, int idx, int total);
    /// The fast pass's numbers, shown at once and marked provisional
    /// (PERF-1); cleared when a new analysis starts.
    void setProvisional(float lufsIntegrated, float truePeakDbtp, bool hasClipping);
    bool hasProvisional() const noexcept { return provisionalText_.isNotEmpty(); }
    const juce::String& provisionalText() const noexcept { return provisionalText_; }

    std::function<void()> onCancel;
    std::function<void()> onRerun;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /// Pure helpers (unit-tested): human stage label and overall fraction.
    static juce::String stageLabel(const std::string& stage);
    static float overallFraction(const std::string& stage, int idx, int total);
    static juce::String provisionalSummary(float lufsIntegrated, float truePeakDbtp, bool hasClipping);

  private:
    int cardHeight() const noexcept;

    Mode mode_ = Mode::Analyzing;
    juce::String detail_;
    juce::String stageText_;
    float fraction_ = 0.0f;
    juce::String provisionalText_;
    juce::TextButton cancelButton_;
    juce::TextButton rerunButton_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AnalysisPendingOverlay)
};

} // namespace khensu
