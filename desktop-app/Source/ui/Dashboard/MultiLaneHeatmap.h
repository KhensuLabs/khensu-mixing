// desktop-app/Source/ui/Dashboard/MultiLaneHeatmap.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h"
#include <khensu/recommendations/Recommendation.h>
#include <functional>
#include <vector>
#include "../shared/KhensuTheme.h"

namespace khensu {

class IAppController;

struct HeatmapLaneData {
    juce::String label;
    std::vector<Recommendation> recs;
};

/// A continuous time-series drawn as a line plot on its own lane, sharing the
/// heatmap's time axis + playhead (Gemini audit: loudness curve & stereo field
/// merged into the unified timeline instead of living in separate panels).
struct TimelineDataLane {
    juce::String label;
    std::vector<juce::Point<float>> samples; ///< x = seconds, y = raw value
    float minV = 0.0f;                       ///< value-axis bottom
    float maxV = 1.0f;                       ///< value-axis top
    juce::Colour colour = khensu::KhensuTheme::accentTeal;
    juce::String suffix; ///< e.g. " LUFS" for the hover readout
};

/// Replaces IssueHeatmapPanel's 3 stacked TimelineComponents (which each drew
/// their own redundant time axis, "no audio loaded" waveform placeholder, and
/// playhead needle) with one component: a single shared time-axis ruler and
/// playhead above N stacked marker lanes. Deliberately does not touch
/// TimelineComponent itself - that class is also used by StructuralFlowPanel
/// and HookDashboard, and this widget only needs a fraction of its behavior
/// (no waveform, no section stripes, no loop selection).
///
/// THREAD_CONTEXT(UIThread)
class MultiLaneHeatmap final : public juce::Component {
  public:
    explicit MultiLaneHeatmap(IAppController* controller);
    ~MultiLaneHeatmap() override;

    void setSpan(float totalSeconds);
    void setLanes(std::vector<HeatmapLaneData> lanes);

    /// Continuous line-plot lanes (loudness, stereo correlation, ...), stacked
    /// above the recommendation-marker lanes and sharing the time axis.
    void setDataLanes(std::vector<TimelineDataLane> dataLanes);

    /// Companion to the per-lane markers: an issues-per-time-bucket density
    /// histogram row (recomputed from lanes_ whenever setLanes()/setSpan()
    /// run), so hotspots read at a glance instead of scanning individual
    /// triangle markers across lanes.
    void setDensityEnabled(bool enabled);

    /// Optional per-frame STFT magnitude grid ([frame][band] = 0..1),
    /// rendered as a heatmap image row above the recommendation-marker
    /// lanes, sharing this component's time axis. Pass an empty `frames` to
    /// clear it (e.g. while a new file's spectrogram is still loading).
    void setSpectrogram(std::vector<std::vector<float>> frames);

    void onSeekRequest(std::function<void(float)> cb) { seekCallback_ = std::move(cb); }

    void paint(juce::Graphics& g) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;

  private:
    /// CE-PLT-4: display-rate playhead (VBlankAttachment) with dirty-rect
    /// repaint of the old and new playhead columns only - the spectrogram
    /// image and lanes underneath are not redrawn.
    void onVBlank();
    std::unique_ptr<juce::VBlankAttachment> vblank_;

    float xFor(float sec) const;
    float timeForX(float x) const;
    juce::Rectangle<float> axisArea() const;
    juce::Rectangle<float> laneArea(int index) const;
    juce::Rectangle<float> spectrogramArea() const;
    juce::Rectangle<float> densityArea() const;
    juce::Rectangle<float> dataLaneArea(int index) const;
    float dataLanesTop() const;   ///< y where the data lanes begin
    float markerLanesTop() const; ///< y where the marker lanes begin
    int laneCount() const { return (int)lanes_.size(); }
    void rebuildSpectrogramImage(int width, int height);
    void rebuildDensity();

    IAppController* controller_ = nullptr;
    float total_ = 0.0f;
    std::vector<HeatmapLaneData> lanes_;
    std::vector<TimelineDataLane> dataLanes_;
    float playheadSec_ = -1.0f;
    float hoverTimeSec_ = -1.0f;

    std::vector<std::vector<float>> spectrogramFrames_;
    juce::Image spectrogramImage_;
    bool spectrogramImageDirty_ = true;

    static constexpr int kAxisH = 18;
    static constexpr int kLabelW = 64;
    static constexpr int kSpectrogramH = 46;
    static constexpr int kDensityH = 22;
    static constexpr int kDataLaneH = 44;
    static constexpr int kDensityBuckets = 60;

    bool densityEnabled_ = false;
    std::vector<float> densityCounts_; // kDensityBuckets, normalized 0..1

    std::function<void(float)> seekCallback_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MultiLaneHeatmap)
};

} // namespace khensu
