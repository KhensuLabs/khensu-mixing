// desktop-app/Source/ui/shared/StereoFieldVizComponent.h
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h"
#include "Camera3D.h"
#include <vector>

namespace khensu {

class IAppController;

class StereoFieldVizComponent final : public juce::Component {
  public:
    explicit StereoFieldVizComponent(IAppController& controller);
    ~StereoFieldVizComponent() override = default;

    void setCorrelation(float c);                  // -1..1
    void setWidthPercent(float w);                 // 0..100
    void setDepthPercent(float d);                 // 0..100
    void setGenreTarget(float width, float depth); // 0..100 each

    void paint(juce::Graphics& g) override;
    void resized() override;

    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override;

    struct StemSphere {
        juce::String name;
        juce::Vector3D<float> pos; // x (-1..1), y (-1..1), z (-1..1)
        juce::Colour colour;
        float radius = 12.0f;
        juce::Point<float> projectedPos;
    };

  private:
    void initStems();
    void updateProjectedPositions();
    void checkAutoPositionStems();
    void analyzeStemsAsync();

    IAppController& controller_;

    float correlation_ = 0.0f;
    float widthPct_ = 0.0f;
    float depthPct_ = 0.0f;
    float genreWidthPct_ = 50.0f;
    float genreDepthPct_ = 50.0f;

    Camera3D camera_;
    std::vector<StemSphere> stems_;
    int selectedStemIdx_ = -1;

    // A drag that doesn't land on a stem sphere orbits the camera around the
    // fixed room centre instead (same interaction as SonicOrbitComponent /
    // ReferenceCloud3D), so the view can be panned/focused rather than only
    // ever showing the initial fixed angle.
    bool orbitingCamera_ = false;
    juce::Point<float> lastCameraDragPos_;

    // Auto-positioning: stems are placed from real per-file pan/loudness
    // analysis the first time they become available, instead of requiring
    // the user to manually drag every sphere into place. A manual drag
    // still persists and takes priority on future loads (see initStems()).
    bool stemsAutoPositioned_ = false;
    bool stemsAnalysisPending_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StereoFieldVizComponent)
};

} // namespace khensu
