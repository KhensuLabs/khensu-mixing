// desktop-app/Source/ui/Dashboard/EmotionalArcPanel.h
#pragma once
#include "../shared/HoverCells.h"
#include <JuceHeader.h>
#include <khensu/analysis/AnalysisResult.h>
#include "../shared/PanelCard.h"

namespace khensu {

class IAppController;

/// Time-windowed energy/mood timeline (see AnalysisResult::EmotionalArcData,
/// extended_analyzer.analyze_emotional_arc) - a continuous energy line
/// across song progress with a mood colour band underneath, and flat
/// sections (energy that doesn't move for 2+ consecutive windows)
/// highlighted with a dashed outline, so a stagnant verse/chorus that never
/// actually lifts is visible at a glance instead of hidden in the single
/// whole-track energy/mood values SongInfoPanel already shows.
class EmotionalArcPanel final : public PanelCard, public juce::SettableTooltipClient {
  public:
    /// VIZ-U3: the readout under the pointer.
    void mouseMove(const juce::MouseEvent& e) override { hover_.update(*this, e.position); }
    void mouseExit(const juce::MouseEvent&) override { setTooltip({}); }
    explicit EmotionalArcPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    static juce::Colour colourForMood(const juce::String& mood);

    AnalysisResult::EmotionalArcData arc_;
    bool hasData_ = false;
    bool analysisExists_ = false; ///< an analysis ran but produced no arc -> "needs deep analysis"

    mutable HoverCells hover_; ///< VIZ-U3, filled in paint()
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EmotionalArcPanel)
};

} // namespace khensu
