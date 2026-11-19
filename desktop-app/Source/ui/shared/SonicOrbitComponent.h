// desktop-app/Source/ui/shared/SonicOrbitComponent.h
#pragma once
#include <JuceHeader.h>
#include <khensu/analysis/AnalysisResult.h>
#include "Camera3D.h"
#include "GLFrameTimer.h"
#include <vector>

namespace khensu {

class IAppController;

class SonicOrbitComponent final : public juce::Component, private juce::Timer, public juce::OpenGLRenderer {
  public:
    explicit SonicOrbitComponent(IAppController& controller);
    ~SonicOrbitComponent() override;

    void setAnalysisData(const AnalysisResult& r);
    void setRenderHistoryMode(bool enabled);
    bool isRenderHistoryMode() const noexcept { return renderHistoryMode_; }

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override;

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
    std::vector<Point3D> genrePointCloud3D_; // 129-genre reference catalog
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SonicOrbitComponent)
};

} // namespace khensu
