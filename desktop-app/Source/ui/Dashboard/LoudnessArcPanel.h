// desktop-app/Source/ui/Dashboard/LoudnessArcPanel.h
// Section-level loudness arc bar chart panel.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <khensu/analysis/AnalysisResult.h>
#include "../shared/PanelCard.h"
#include "SectionContrast.h"
#include <vector>

namespace khensu {

class IAppController;

/// Displays per-section integrated LUFS as a horizontal bar chart (loudness arc),
/// revealing chorus lift, verse headroom, and structural energy flow.
/// Populated from AnalysisResult::section_analysis after full analysis completes.
class LoudnessArcPanel : public PanelCard {
  public:
    explicit LoudnessArcPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    struct SectionBar {
        juce::String label;
        float lufs = 0.0f;
        bool valid = false;
    };
    std::vector<SectionBar> bars_;
    float minLufs_ = -30.0f;
    float maxLufs_ = -6.0f;

    // --- Macrodynamics contrast (Katz: macrodynamics is what over-compression
    // destroys). Measured verse->chorus LUFS lift vs. the genre's expected
    // contrast (GenreEntry::chorus_contrast_norm mapped to dB) - drawn as a
    // footer readout under the bars.
    void computeContrast();
    float measuredContrastDb_ = 0.0f;
    float expectedContrastDb_ = 0.0f;
    bool contrastValid_ = false;
    section_contrast::Result contrast_; ///< how the lift was measured (role names, repeats, or approx)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoudnessArcPanel)
};

} // namespace khensu
