// desktop-app/Source/ui/Dashboard/LoudnessCurvePanel.h
#pragma once
#include <JuceHeader.h>
#include <khensu/analysis/AnalysisResult.h>
#include "../shared/PanelCard.h"
#include <vector>

namespace khensu {

/// Continuous short-term (3 s window / 1 s hop) LUFS history, a real
/// mastering-meter-style loudness journey -- unlike LoudnessArcPanel, which
/// only buckets one LUFS value per detected song section. Backed by the new
/// /analyze/loudness_curve endpoint (loudness.py's short_term_loudness_curve,
/// the same measurement loudness_range() already computed internally for
/// LRA, exported here as a curve instead of collapsed to one number).
///
/// THREAD_CONTEXT(UIThread)
class LoudnessCurvePanel final : public PanelCard {
  public:
    explicit LoudnessCurvePanel(IAppController& controller);
    ~LoudnessCurvePanel() override;

    void setData(const AnalysisResult& r);
    void resized() override;
    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;

  private:
    struct MetricTimelineWindow {
        float startSec = 0.0f;
        float endSec = 0.0f;
        float loudnessLufs = -40.0f;
        float crestDb = 0.0f;
        float harshness = 0.0f;
    };

    /// CE-PLT-4: display-rate playhead with dirty-rect repaint.
    void onVBlank();
    void fetchCurveIfNeeded();
    /// The accepted render's path when one exists on disk, else the
    /// session's original file (what fetchCurveIfNeeded measures).
    juce::String currentTarget() const;
    void rebuildLufsHistogram(); ///< probability density of LUFS values (Gemini audit)
    float xFor(float sec) const;
    float yFor(float lufs) const;
    float yForCrest(float crestDb) const;

    std::vector<float> lufsCurve_;
    std::vector<float> crestCurve_;
    std::vector<MetricTimelineWindow> metricTimeline_; ///< three-lane per-window heatmap data
    std::vector<float> lufsHistogram_; ///< kHistBins, normalized 0..1

    // True-peak overshoot distribution (Gemini audit): x = time_sec, y = dBTP.
    std::vector<juce::Point<float>> tpEvents_;
    int tpOverCount_ = 0;
    float tpThresholdDbtp_ = -1.0f;
    float hopSec_ = 1.0f;
    float durationSec_ = 0.0f;
    float integratedLufs_ = -99.0f;
    float playheadSec_ = -1.0f;
    float hoverX_ = -1.0f;
    juce::String lastPath_;
    juce::String renderSeen_;    ///< last currentRenderPath() onVBlank saw
    bool showingRender_ = false; ///< the curves are an accepted render's, not the original's
    bool loading_ = false;
    bool fetchFailed_ = false;
    juce::String fetchError_;
    juce::String hotspotSummary_; ///< "which section" hotspots from /analyze/metric_timeline (#6)

    static constexpr int kAxisH = 16;
    static constexpr int kHistBins = 48;
    static constexpr int kDensityStripW = 56; ///< right-edge marginal-histogram strip
    static constexpr float kLufsMin = -40.0f;
    static constexpr float kLufsMax = 0.0f;
    static constexpr float kCrestMin = 0.0f;
    static constexpr float kCrestMax = 24.0f;

    // Declared last: touches the members above, so it detaches first.
    juce::VBlankAttachment vblank_{this, [this] { onVBlank(); }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoudnessCurvePanel)
};

} // namespace khensu
