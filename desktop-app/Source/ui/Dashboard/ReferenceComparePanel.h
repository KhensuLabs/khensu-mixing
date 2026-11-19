// desktop-app/Source/ui/Dashboard/ReferenceComparePanel.h
#pragma once
#include "../shared/PanelCard.h"
#include "../shared/KhensuErrorState.h"
#include <khensu/ui/EqCurveComponent.h>
#include <khensu/analysis/AnalysisResult.h>
#include <memory>
#include <vector>

namespace khensu {

class ReferenceComparePanel final : public PanelCard {
  public:
    explicit ReferenceComparePanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void setTargetFilePath(const juce::String& path);
    void paint(juce::Graphics& g) override;
    void resized() override;
    void applyMatchEq(); // public so the reference-track ritual can invoke it
    /// DSK-04: the reference, its deltas, the match curve and the blind set
    /// all belong to the session they were loaded in.
    void resetForSession() override;
  private:
    struct DeltaRow {
        juce::String dim, targetStr, refStr, deltaStr;
        float delta = 0.0f;
    };

    juce::String targetFilePath_;
    /// The target the current deltas / match curve were computed against
    /// (WIRE-07): Apply Match EQ refuses to render a curve computed for a
    /// different file onto the current target.
    juce::String matchEqTargetPath_;
    /// Bumped on every new comparison and on session reset; a late result
    /// for an older request is dropped.
    int requestGeneration_ = 0;
    std::vector<DeltaRow> deltas_;
    std::vector<MatchEqBand> matchEq_; // corrective curve from the last compare
    // LUFS and stereo-correlation deltas, kept separately from the display
    // rows so applyMatchEq() can derive a gain trim and width adjustment
    // without re-parsing formatted display strings.
    float lufsDelta_ = 0.0f;
    float stereoCorrelationDelta_ = 0.0f;
    bool loading_ = false;
    juce::String errorMsg_;
    KhensuErrorState errorState_; // shown on analyzer failure (with Retry)
    juce::String refPath_;        // last loaded reference (for blind A/B)
    juce::TextButton loadButton_{TRANS("Load Reference Track")};
    juce::TextButton matchButton_{TRANS("Apply Match EQ")};
    // Blind Reference A/B Ritual: loads the reference as the A/B "B" side
    // (playback gain-matches B to the mix's measured LUFS - countering the
    // "louder perceived better" bias) and opens the blind ABX overlay.
    juce::TextButton blindAbButton_{TRANS("Blind A/B vs Ref")};
    juce::TextButton addBlindReferenceButton_{TRANS("Add Blind Reference")};
    juce::Label blindSetLabel_;
    juce::TextButton toggleCurveButton_{TRANS("Edit Curve")};
    juce::ToggleButton matchLoudnessWidthToggle_{TRANS("+ Loudness/Width")};
    bool showCurve_ = false;
    // The match-EQ curve is drawn on the same interactive component the EQ
    // tab uses for drag-to-edit, so the user can nudge the suggested curve
    // before rendering it rather than only accepting it as-is.
    EqCurveComponent matchEqCurve_;
    std::unique_ptr<juce::FileChooser> fileChooser_;

    void runReferenceAnalysis(const juce::String& refPath);
    void addBlindReference(const juce::String& path);
    void updateBlindReferenceControls();
    juce::StringArray blindReferencePaths_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReferenceComparePanel)
};

} // namespace khensu
