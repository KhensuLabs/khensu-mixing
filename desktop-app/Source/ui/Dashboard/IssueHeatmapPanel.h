// desktop-app/Source/ui/Dashboard/IssueHeatmapPanel.h
#pragma once
#include <JuceHeader.h>
#include <khensu/analysis/AnalysisResult.h>
#include "../shared/PanelCard.h"
#include "MultiLaneHeatmap.h"

namespace khensu {

/// Dashboard "Timeline" tab: a cross-domain, time-axis view of detected
/// issues, one lane per domain (Spectral / Dynamics / Stereo), sharing a
/// single time axis and playhead via MultiLaneHeatmap - replacing 3
/// independently-drawn TimelineComponent lanes (each with its own redundant
/// axis and "no audio loaded" placeholder) with one shared-axis widget.
///
/// THREAD_CONTEXT(UIThread)
class IssueHeatmapPanel final : public PanelCard {
  public:
    explicit IssueHeatmapPanel(IAppController& controller);
    void setData(const AnalysisResult& r);
    void resized() override;

  private:
    void fetchSpectrogramIfNeeded();
    void fetchLoudnessCurveIfNeeded();
    void rebuildDataLanes();

    MultiLaneHeatmap heatmap_;
    juce::String lastSpectrogramPath_; // avoids re-fetching for the same file
    juce::String lastLoudnessPath_;    // ditto for the loudness curve fetch

    // Continuous data-lane sources for the unified timeline (Gemini audit).
    std::vector<juce::Point<float>> stereoCorrelation_; // x = sec, y = -1..1
    std::vector<float> lufsCurve_;
    float lufsHopSec_ = 1.0f;
    float span_ = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IssueHeatmapPanel)
};

} // namespace khensu
