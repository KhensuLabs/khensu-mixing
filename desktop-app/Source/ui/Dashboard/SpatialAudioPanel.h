// desktop-app/Source/ui/Dashboard/SpatialAudioPanel.h
#pragma once
#include "../shared/HoverCells.h"
#include "../shared/PanelCard.h"
#include "../shared/Camera3D.h"
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/recommendations/SpatialAudioAssessment.h>

namespace khensu {

/// Assesses (from the existing stereo analysis - no new audio processing)
/// how well this stereo mix would translate into an object-based spatial
/// upmix (Dolby Atmos, Sony 360 Reality Audio, Apple Spatial Audio, etc.)
/// and renders it as a score + criteria breakdown + an illustrative 3D
/// object-placement reference. See SpatialAudioAssessment.h for why this is
/// a translatability estimate, not an Atmos certification - a stereo file
/// carries no real per-object spatial data to detect.
class SpatialAudioPanel final : public PanelCard, public juce::SettableTooltipClient {
  public:
    /// VIZ-U3: the readout under the pointer.
    void mouseMove(const juce::MouseEvent& e) override { hover_.update(*this, e.position); }
    void mouseExit(const juce::MouseEvent&) override { setTooltip({}); }
    explicit SpatialAudioPanel(IAppController& c);

    void setData(const AnalysisResult& r);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override;

  private:
    void drawScoreColumn(juce::Graphics& g, juce::Rectangle<int> area);
    void drawScene3D(juce::Graphics& g, juce::Rectangle<int> area);

    SpatialAudioAssessment assessment_;
    bool hasData_ = false;

    Camera3D camera_;
    bool dragging_ = false;
    juce::Point<float> lastDragPos_;

    static constexpr int kScoreColW = 190;

    HoverCells hover_; ///< VIZ-U3, filled in paint()
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpatialAudioPanel)
};

} // namespace khensu
