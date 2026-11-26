// desktop-app/Source/ui/shared/SonicOrbitComponent.h
// The 3D timbre space: the track's sections (or, in render-history mode, the
// session's trajectory from the original through each version) against the
// whole genre catalog. STU-12: every catalog genre is a point coloured by its
// family (categorical viz palette, legend bottom-left); hovering names the
// nearest genre; flyToMyGenre() / flyToTrajectory() / resetView() move the
// camera on a critically damped spring (instant with reduced motion).
#pragma once
#include <JuceHeader.h>
#include <khensu/analysis/AnalysisResult.h>
#include "Camera3D.h"
#include "GLFrameTimer.h"
#include "ValueAnimator.h"
#include <string>
#include <vector>

namespace khensu {

class IAppController;

class SonicOrbitComponent final : public juce::Component,
                                  public juce::SettableTooltipClient,
                                  private juce::Timer,
                                  public juce::OpenGLRenderer {
  public:
    explicit SonicOrbitComponent(IAppController& controller);
    ~SonicOrbitComponent() override;

    void setAnalysisData(const AnalysisResult& r);
    void setRenderHistoryMode(bool enabled);
    bool isRenderHistoryMode() const noexcept { return renderHistoryMode_; }

    /// STU-12 fly-to presets. flyToMyGenre() aims at the session's genre
    /// (stated on Intake, else detected); flyToTrajectory() switches to
    /// render-history mode and frames the original -> versions line.
    void flyToMyGenre();
    void flyToTrajectory();
    void resetView();
    int genreCount() const noexcept { return static_cast<int>(genrePointCloud3D_.size()); }

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;

    // OpenGLRenderer overrides
    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;

  private:
    void timerCallback() override;
    void loadRenderHistoryPoints();
    void loadGenrePointCloud();

    struct Point3D {
        float x, y, z; // Normalized -1..1
        juce::Colour colour;
        juce::String label;
        bool isReference = false;
        int family = -1;  ///< genre points: constellation family (categorical slot)
        std::string id;   ///< genre points: the catalog id
    };

    struct ProjectedPoint {
        float x, y;
        float scale;
        float depth;
        juce::Colour colour;
        juce::String label;
        bool isReference = false;
    };

    IAppController& controller_;
    juce::OpenGLContext openGLContext_;

    std::unique_ptr<juce::OpenGLShaderProgram> shaderProgram_;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> projectionUniform_;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> viewUniform_;

    std::vector<Point3D> points3D_; // "This Track" mode data
    std::vector<Point3D> renderHistoryPoints3D_;
    std::vector<Point3D> genrePointCloud3D_; // the whole genre catalog (genre_targets.json, 185 genres)
    std::vector<ProjectedPoint> projected_;

    bool renderHistoryMode_ = false;
    bool renderHistoryLoading_ = false;
    bool showPointCloud_ = true;
    AnalysisResult lastAnalysisResult_; // re-applied when leaving history mode

    Camera3D camera_;
    GLFrameTimer frameTimer_;
    juce::Point<float> lastDragPos_;
    bool dragging_ = false;

    // Cached values to map active playhead to current section
    double totalDuration_ = 0.0;
    std::vector<std::pair<double, double>> sectionTimeRanges_;

    void projectPoints();
    juce::String sessionGenreId() const;
    int genreIndexById(const juce::String& id) const;
    void flyTo(float x, float y, float z, float zoom);
    void applyCameraAnimation();
    void paintFamilyLegend(juce::Graphics& g, juce::Rectangle<int> area) const;

    int hoveredGenre_ = -1;
    bool frameTrajectoryWhenLoaded_ = false;
    juce::String flyNote_; ///< why a fly-to could not go (no genre, ...)

    // STU-11/12: the camera's look-at point and distance glide on springs.
    // Declared last: their callbacks touch the camera and the points.
    ValueAnimator camX_{[this] { applyCameraAnimation(); }, 450, ValueAnimator::Curve::Spring};
    ValueAnimator camY_{[this] { applyCameraAnimation(); }, 450, ValueAnimator::Curve::Spring};
    ValueAnimator camZ_{[this] { applyCameraAnimation(); }, 450, ValueAnimator::Curve::Spring};
    ValueAnimator camZoom_{[this] { applyCameraAnimation(); }, 450, ValueAnimator::Curve::Spring};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SonicOrbitComponent)
};

} // namespace khensu
