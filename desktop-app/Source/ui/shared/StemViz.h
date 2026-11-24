// desktop-app/Source/ui/shared/StemViz.h
// VIZ-5: the stems, measured natively from the separated files' timelines
// (TrackTimeline, one per stem - no engine round trip):
//   streamgraph   each stem's energy over time, stacked around a centre line
//                 (the silhouette layout), so the arrangement reads as shapes:
//                 the vocal entering, the drums carrying, the drop;
//   masking map   per stem pair, frequency x time: where both stems put
//                 comparable energy in the same band at the same moment - the
//                 service's own definition (analysis/masking.py: per frame,
//                 min of the normalised band profiles, only in bands within
//                 20 dB of each other and above -80 dB, only where both play).
// Colours: a fixed categorical slot per stem (vocals, drums, bass, other) -
// never a severity colour; the masking map is the sequential palette.
// THREAD_CONTEXT(UIThread) for the charts; the math is pure (tested).
#pragma once
#include <JuceHeader.h>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <khensu/analysis/TrackTimeline.h>
#include "ChartStyle.h"

namespace khensu {

namespace stem_viz {

/// The stems in their fixed order (= categorical slot).
inline const std::vector<std::string>& stemOrder()
{
    static const std::vector<std::string> order{"vocals", "drums", "bass", "other"};
    return order;
}
inline int slotOf(const std::string& stem)
{
    const auto& o = stemOrder();
    const auto it = std::find(o.begin(), o.end(), stem);
    return it == o.end() ? 4 : static_cast<int>(it - o.begin());
}

struct Stream {
    double hopSec = 0.5;
    std::vector<std::string> stems;
    std::vector<std::vector<float>> energy; ///< [stem][frame] linear power (mean square)
    std::vector<double> timeSec;
};

/// Each stem's short-term power, on a common grid of `hopSec`.
Stream stream(const std::map<std::string, std::shared_ptr<const TrackTimeline>>& stems, double hopSec = 0.5);

struct MaskingMap {
    std::string stemA, stemB;
    std::vector<float> bandHz;
    double frameSec = 0.0;
    std::vector<std::vector<float>> grid; ///< [frame][band] 0..1 (normalised to the map's max)
    float worst = 0.0f;                   ///< the un-normalised maximum
    double worstSec = 0.0;
    float worstHz = 0.0f;
};

/// The masking map of two stems (same band grid required).
MaskingMap masking(const std::string& a, const TrackTimeline& ta, const std::string& b, const TrackTimeline& tb);

} // namespace stem_viz

class StemStreamGraph final : public ThemedChart {
  public:
    StemStreamGraph();
    void setStream(stem_viz::Stream s);
    const stem_viz::Stream& stream() const noexcept { return stream_; }

  protected:
    void paintData(juce::Graphics& g, juce::Rectangle<float> plot) override;
    ui::chart::Readout readoutAt(double x, double y) override;

  private:
    stem_viz::Stream stream_;
    float maxTotal_ = 1.0f;
};

class MaskingHeatmapView final : public ThemedChart {
  public:
    MaskingHeatmapView();
    void setMap(stem_viz::MaskingMap m);
    const stem_viz::MaskingMap& map() const noexcept { return map_; }

  protected:
    void paintData(juce::Graphics& g, juce::Rectangle<float> plot) override;
    ui::chart::Readout readoutAt(double x, double y) override;

  private:
    stem_viz::MaskingMap map_;
    juce::Image image_;
};

} // namespace khensu
