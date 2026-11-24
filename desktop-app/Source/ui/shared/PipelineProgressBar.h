#pragma once
#include <JuceHeader.h>
#include <array>
#include <string>

namespace khensu {

// Dismissible strip pinned above DashboardScreen's viewport, showing the
// 6-stage pipeline (fast/deep/sections/drums/chords/reference collapse into
// this slot list - reference is greyed out when no reference track is set;
// stems is the 6th slot) plus a Skip button. Driven entirely by
// AppController::onPipelineProgress callbacks - this component has no
// polling of its own.
class PipelineProgressBar final : public juce::Component {
  public:
    enum class SlotState { Pending, InProgress, Done, Failed, Skipped };

    PipelineProgressBar();

    void paint(juce::Graphics&) override;
    void resized() override;

    // stage matches AnalysisController's coarse names ("fast", "deep_analysis",
    // "sections", "drums", "chords", "reference") or "stems:<status>" from
    // AppController::runStemSeparation. stageIndex/totalStages are the fixed
    // 1-6 numbering both sources already report in.
    void updateStage(const std::string& stage, int stageIndex, int totalStages);

    // Deep analysis's inner fill bar - called separately since the coarse
    // updateStage() call for "deep_analysis" doesn't carry the 1-9 sub-phase
    // number; wired from the same poll loop that already has it.
    void updateDeepAnalysisSubProgress(int subIndex, int subTotal);
    void updateStemProgress(float fraction);

    void setReferenceTrackPresent(bool present)
    {
        hasReferenceTrack_ = present;
        repaint();
    }
    void reset();
    /// For tests: one slot's state (0 = Fast ... 5 = Stems).
    SlotState slotState(int index) const noexcept
    {
        return slotStates_[static_cast<size_t>(juce::jlimit(0, kNumSlots - 1, index))];
    }

    std::function<void()> onSkipClicked;

  private:
    static constexpr int kNumSlots = 6;
    std::array<SlotState, kNumSlots> slotStates_{};
    std::array<juce::String, kNumSlots> slotLabels_{"Fast",  "Deep Analysis",    "Sections",
                                                    "Drums", "Chords/Reference", "Stems"};
    float deepAnalysisFraction_ = 0.0f;
    float stemFraction_ = 0.0f;
    bool hasReferenceTrack_ = true;

    juce::TextButton skipButton_{TRANS("Skip deep analysis - use fast results only")};

    void drawSlot(juce::Graphics& g, juce::Rectangle<int> bounds, int index);
    /// Inner fill bar for the two stages that report sub-progress (deep
    /// analysis, stems). `fraction` is clamped to 0..1.
    static void drawSubProgress(juce::Graphics& g, juce::Rectangle<int> bounds, float fraction);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PipelineProgressBar)
};

} // namespace khensu
