// desktop-app/Source/ui/Dashboard/StereoFieldPanel.h
#pragma once
#include "../shared/HoverCells.h"
#include "../shared/PanelCard.h"
#include "../shared/ListenerHandle.h"
#include "../shared/ZoneMeterComponent.h"
#include "../shared/StereoFieldVizComponent.h"
#include "../shared/MultiBandGoniometerComponent.h"
#include "../shared/CompareDeltaChip.h"
#include <khensu/analysis/AnalysisResult.h>

namespace khensu {

// Time-windowed companion to the (whole-track) correlation meter and
// goniometer above it - plots correlation and width as two overlaid line
// traces across song progress, from AnalysisResult::StereoFieldTimeline
// (extended_analyzer.analyze_stereo_field_over_time). Hidden (zero rows)
// until that data is present, matching CorrelationStripComponent's pattern
// on PhasePanel.
class StereoFieldTimelineStrip : public juce::Component, public juce::SettableTooltipClient {
  public:
    /// VIZ-U3: the readout under the pointer.
    void mouseMove(const juce::MouseEvent& e) override { hover_.update(*this, e.position); }
    void mouseExit(const juce::MouseEvent&) override { setTooltip({}); }
    StereoFieldTimelineStrip() = default;
    void setTimeline(std::vector<AnalysisResult::StereoFieldPoint> points)
    {
        points_ = std::move(points);
        repaint();
    }
    bool hasData() const noexcept { return !points_.empty(); }
    void paint(juce::Graphics& g) override;

  private:
    std::vector<AnalysisResult::StereoFieldPoint> points_;
    HoverCells hover_; ///< VIZ-U3, filled in paint()
    JUCE_DECLARE_NON_COPYABLE(StereoFieldTimelineStrip)
};

// THREAD_CONTEXT(UIThread)
class StereoFieldPanel final : public PanelCard {
  public:
    explicit StereoFieldPanel(IAppController& c);
    ~StereoFieldPanel() override;
    void setData(const AnalysisResult& r);
    void updateRealTime(float width, float correlation);
    void resized() override;
    void setCompareModeEnabled(bool enabled);

  private:
    StereoFieldVizComponent fieldViz_;
    ZoneMeterComponent correlationMeter_;
    MultiBandGoniometerComponent goniometer_;
    StereoFieldTimelineStrip timelineStrip_;

    CompareDeltaChip compareChip_;
    bool compareEnabled_ = false;
    std::optional<ReferenceDeltas> lastRefDeltas_;
    void updateCompareChip();

    // ENG-2: hub registrations; destroyed (deregistered) before the members
    // above them that the callbacks touch.
    ListenerHandle realTimeMeterListener_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StereoFieldPanel)
};

} // namespace khensu
