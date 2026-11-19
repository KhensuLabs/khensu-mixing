// desktop-app/Source/ui/VocalWorkspace/VocalAnalysisPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../shared/MeterBarComponent.h"
#include "../shared/BadgeComponent.h"
#include <khensu/analysis/AnalysisResult.h>

namespace khensu {

// THREAD_CONTEXT(UIThread)
class VocalAnalysisPanel final : public PanelCard, private juce::Timer {
  public:
    explicit VocalAnalysisPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void setStemsAvailable(bool available);
    void paint(juce::Graphics& g) override;
    void resized() override;

    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;

  private:
    void refreshVocalTimeline();
    void timerCallback() override;
    void scrubToMouse(int mouseX);

    IAppController& controller_;
    bool stemsAvailable_ = false;
    MeterBarComponent presenceMeter_, sibilanceMeter_, dynamicsMeter_;
    BadgeComponent levelBadge_;

    // Sibilance Sentinel one-click: sets the De-essing vocal macro from the
    // measured sibilance stats (peak-decile sibilance -> macro strength),
    // driving the same render pipeline as manually dragging the macro.
    juce::TextButton applyDeessButton_{TRANS("Set De-esser From Analysis")};
    void applyDeessRecommendation();

    // Time-resolved vocal data from /analyze/vocal (painted directly).
    std::vector<float> sibTimeline_;                     // 0..1 per window
    std::vector<float> siiTimeline_;                     // 0..1 per window
    std::vector<std::pair<float, float>> breathMarkers_; // (start_sec, dur_sec)
    float presenceScore_ = 0.0f, clarityScore_ = 0.0f, warmthScore_ = 0.0f, siiScore_ = 1.0f;
    // De-esser's auto-tuned target frequency (energy-weighted centroid of the
    // 5-10kHz band on the most sibilant frames); 0 = no sibilant frames found.
    // Computed by analyze_vocal() and already used to bias the chat/mixer-chain
    // LLM prompt, but had no UI readout until now.
    float sibilanceHz_ = 0.0f;
    float totalDurationSec_ = 0.0f;
    float playheadSec_ = -1.0f;
    juce::Rectangle<int> heatmapArea_, siiTimelineArea_, tilesArea_;
    juce::String lastAnalyzedTarget_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VocalAnalysisPanel)
};

} // namespace khensu
