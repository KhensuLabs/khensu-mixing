// desktop-app/Source/ui/Dashboard/DrumPatternPanel.h
#pragma once
#include <JuceHeader.h>
#include <khensu/analysis/AnalysisResult.h>
#include "../shared/PanelCard.h"

namespace khensu {

/// Piano-roll-style groove timeline for /analyze/drums' per-hit kick/snare
/// event data (AnalysisResult::DrumData::kick_events / snare_events). The
/// backend has always computed this (router_audio.py's DrumEventResult
/// carries onset/attack/body/fundamental per hit) but the C++ bridge only
/// ever surfaced the aggregate scalars, so no timeline was possible until
/// HttpServiceBridge::analyzeDrums() was extended to parse the event arrays.
///
/// Two lanes (KICK / SNARE) share one time axis; each hit is a marker
/// positioned by onset_sec, sized by body_db (loudness), and coloured by
/// attack_ms (sharper transients = brighter). This is deliberately a
/// self-contained painted component like MultiLaneHeatmap, not a
/// juce::TextEditor list like ChordsPanel - the ask is a graphical groove
/// view, not a scrolling text log.
///
/// THREAD_CONTEXT(UIThread)
class DrumPatternPanel final : public PanelCard {
  public:
    explicit DrumPatternPanel(IAppController& controller);
    void setData(const AnalysisResult& r);

    /// Refresh the timeline from a standalone drum analysis (spec §2.1): the
    /// isolated Demucs drum stem gives far cleaner onsets than the full mix,
    /// so once separation lands the Dashboard re-runs /analyze/drums on the
    /// stem and pushes the result here. `fromStem` tags the source so the
    /// panel can note it's the higher-confidence pass.
    void setDrumData(const AnalysisResult::DrumData& d, bool fromStem);

    void resized() override;
    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;

  private:
    float xFor(float sec) const;
    juce::Rectangle<float> axisArea() const;
    juce::Rectangle<float> laneArea(int index) const;

    void rebuildSpan();

    std::vector<AnalysisResult::DrumHitEvent> kickEvents_;
    std::vector<AnalysisResult::DrumHitEvent> snareEvents_;
    float span_ = 0.0f;
    float kickAvgBpm_ = 0.0f;
    float hoverTimeSec_ = -1.0f;
    bool fromStem_ = false; ///< true = analysed on the isolated drum stem

    static constexpr int kAxisH = 18;
    static constexpr int kLabelW = 52;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DrumPatternPanel)
};

} // namespace khensu
