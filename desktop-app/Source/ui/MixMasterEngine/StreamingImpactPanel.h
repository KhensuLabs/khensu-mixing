// desktop-app/Source/ui/MixMasterEngine/StreamingImpactPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h"
#include "../shared/PanelCard.h"
#include <khensu/analysis/AnalysisResult.h>

namespace khensu {

class StreamingImpactPanel final : public PanelCard {
  public:
    explicit StreamingImpactPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    float intLufs_ = -23.0f;
    float truePeakDbtp_ = -23.0f;
    /// Own flag: a platform that applies 0 dB (a quiet mix on YouTube) leaves
    /// the controller's offset at 0, which hasStreamingPreview() reads as off.
    bool previewOn_ = false;

    juce::ComboBox platformCombo_;
    juce::TextButton previewButton_{TRANS("Preview at Platform Loudness")};
    juce::Label penaltyLabel_;
    void togglePreview();
    void updatePenaltyText();
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StreamingImpactPanel)
};

} // namespace khensu
