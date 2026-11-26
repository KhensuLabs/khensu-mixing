// desktop-app/Source/ui/Dashboard/MultiLaneHeatmap.h
#pragma once
#include <JuceHeader.h>
#include <khensu/recommendations/Recommendation.h>
#include <functional>
#include <memory>
#include <vector>
#include "../shared/ChartKitDesktop.h"
#include "../shared/KhensuTheme.h"

namespace khensu {

class IPlayback;

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
    juce::Colour colour = khensu::KhensuTheme::vizCategory(0);
    juce::String suffix; ///< e.g. " LUFS" for the hover readout
};

/// The Issue heatmap card's timeline (VIS-1: on the chart toolkit). One
/// ChartKit time axis in seconds along the bottom and one category axis
/// down the left, one category per row, top to bottom:
///   Spectrum   the spectro-temporal image (the one spectrogram map, VIS-2)
///   Density    findings per time bucket (the sequential map - intensity)
///   data rows  continuous curves (loudness, stereo correlation)
///   lanes      one recommendation-marker lane per domain (severity colours)
/// Hover reads the row, the time and the value with its unit; a click plays
/// from that moment (chart_transport::playFrom) and the playhead follows the
/// transport. The screen-reader summary is built from the data.
///
/// THREAD_CONTEXT(UIThread)
class MultiLaneHeatmap final : public ThemedChart {
  public:
    /// `playback` (optional, tests pass nullptr) drives click-to-play and
    /// the playhead.
    explicit MultiLaneHeatmap(IPlayback* playback);
    ~MultiLaneHeatmap() override;

    void setSpan(float totalSeconds);
    void setLanes(std::vector<HeatmapLaneData> lanes);

    /// Continuous line-plot lanes (loudness, stereo correlation, ...), stacked
    /// above the recommendation-marker lanes and sharing the time axis.
    void setDataLanes(std::vector<TimelineDataLane> dataLanes);

    /// Companion to the per-lane markers: an issues-per-time-bucket density
    /// row (recomputed from the lanes whenever setLanes()/setSpan() run), so
    /// hotspots read at a glance instead of scanning individual markers.
    void setDensityEnabled(bool enabled);

    /// Optional per-frame STFT magnitude grid ([frame][band] = 0..1), drawn as
    /// a heatmap row above the others. Empty clears it.
    void setSpectrogram(std::vector<std::vector<float>> frames);

    /// True when there is nothing to plot (the empty state shows).
    bool isEmpty() const noexcept;

    /// The rows top to bottom (the category axis' names).
    int rowCount() const noexcept { return static_cast<int>(rows_.size()); }
    juce::String rowName(int row) const;
    /// The pixel band of `row` inside the plot.
    juce::Rectangle<float> rowArea(int row) const;

    static constexpr int kDensityBuckets = 60;
    double densityBucketSeconds() const noexcept { return total_ > 0.0f ? total_ / kDensityBuckets : 0.0; }

    void paint(juce::Graphics& g) override;

  protected:
    void paintData(juce::Graphics& g, juce::Rectangle<float> plot) override;
    void paintOverlay(juce::Graphics& g, juce::Rectangle<float> plot) override;
    void paintHoverMark(juce::Graphics& g, juce::Rectangle<float> plot, float px, float py) override;
    ui::chart::Readout readoutAt(double xValue, double yValue) override;
    juce::String autoSummary() const override;

  private:
    enum class RowKind { Spectrum, Density, Data, Marker };
    struct Row {
        RowKind kind;
        int index; ///< into dataLanes_ / lanes_
        juce::String name;
    };

    void rebuildRows();
    void rebuildDensity();
    int rowAt(double yValue) const;

    void paintSpectrogramRow(juce::Graphics& g, juce::Rectangle<float> area);
    void paintDensityRow(juce::Graphics& g, juce::Rectangle<float> area);
    void paintDataRow(juce::Graphics& g, juce::Rectangle<float> area, const TimelineDataLane& dl);
    void paintMarkerRow(juce::Graphics& g, juce::Rectangle<float> area, const HeatmapLaneData& lane);

    IPlayback* playback_ = nullptr;
    float total_ = 0.0f;
    std::vector<HeatmapLaneData> lanes_;
    std::vector<TimelineDataLane> dataLanes_;
    std::vector<Row> rows_;

    std::vector<std::vector<float>> spectrogramFrames_;
    juce::Image spectrogramImage_;
    bool spectrogramImageDirty_ = true;

    bool densityEnabled_ = false;
    std::vector<int> densityCounts_; ///< findings per bucket (kDensityBuckets)
    int densityMax_ = 0;

    /// The playhead follows the transport (declared last: it references the chart).
    std::unique_ptr<chart_transport::PlayheadFollower> playheadFollower_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MultiLaneHeatmap)
};

} // namespace khensu
