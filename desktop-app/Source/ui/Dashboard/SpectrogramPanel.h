// desktop-app/Source/ui/Dashboard/SpectrogramPanel.h
#pragma once
#include <JuceHeader.h>
#include <khensu/analysis/AnalysisResult.h>
#include "../shared/PanelCard.h"
#include "../shared/Camera3D.h"
#include <vector>

namespace khensu {

class IAppController;

class SpectrogramPanel final : public PanelCard {
  public:
    explicit SpectrogramPanel(IAppController& c);
    ~SpectrogramPanel() override;

    void setData(const AnalysisResult& r);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

  private:
    void fetchSpectrogramIfNeeded();
    juce::Rectangle<float> plotArea() const;
    float xForTime(float sec) const;
    float yForFreq(float hz) const;
    float timeForX(float x) const;
    float freqForY(float y) const;

    void draw3DWaterfall(juce::Graphics& g, const juce::Rectangle<float>& plot);
    void draw3DEqTerrain(juce::Graphics& g, const juce::Rectangle<float>& plot);
    void drawHoverReadout(juce::Graphics& g, const juce::Rectangle<float>& plot);
    void drawRecommendationAnnotations(juce::Graphics& g, const juce::Rectangle<float>& plot);
    std::vector<std::vector<float>> framesForCurrentView() const;
    void resetZoom();
    void updateZoomControl();

    // A frequency-scoped recommendation drawn on the 2D heatmap.
    struct FreqAnnotation {
        float low_hz = 0, high_hz = 0, severity = 0;
        juce::String label;
    };
    std::vector<FreqAnnotation> annotations_;
    int hoveredAnnotation_ = -1;
    juce::Rectangle<float> annotationChipBounds(const FreqAnnotation& a) const;
    int hitTestAnnotation(juce::Point<float> pos) const;

    std::vector<std::vector<float>> frames_; // [frame][band], 0..1
    std::vector<float> bandEdgesHz_;
    float durationSec_ = 0.0f;
    juce::String lastFetchedPath_;

    std::vector<Recommendation> recommendations_;
    std::optional<AnalysisResult::ResonanceData> resonanceData_;

    juce::Image image_;
    bool imageDirty_ = true;

    // View options
    bool is3DMode_ = true;
    bool showEqTerrain_ = false;

    juce::TextButton toggle3DButton_;
    juce::TextButton toggleEqTerrainButton_;
    juce::TextButton resetZoomButton_{TRANS("Reset Zoom")};

    Camera3D camera3D_;
    float hoverX_ = -1.0f, hoverY_ = -1.0f; // local coords; -1 = not hovering
    float viewStartSec_ = 0.0f, viewEndSec_ = 0.0f;
    float viewMinHz_ = 20.0f, viewMaxHz_ = 20000.0f;
    juce::Point<float> dragStart_{-1.0f, -1.0f};
    juce::Rectangle<float> dragSelection_;

    static constexpr int kAxisH = 20;
    static constexpr int kFreqLblW = 46;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrogramPanel)
};

} // namespace khensu
