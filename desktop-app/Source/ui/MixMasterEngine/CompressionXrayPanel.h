// desktop-app/Source/ui/MixMasterEngine/CompressionXrayPanel.h
// Compression X-Ray: the effective gain curve of the current processed
// render vs the original, with pumping / over-compression detection -
// what the dynamics chain actually did over time, not what its knobs say.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../shared/PanelRunState.h"
#include <vector>

namespace khensu {

class IAppController;

class CompressionXrayPanel final : public PanelCard {
  public:
    explicit CompressionXrayPanel(IAppController& c);
    void paint(juce::Graphics& g) override;
    void resized() override;

    /// Auto-run when a processed render lands (spec §1.2): same as pressing
    /// the X-Ray button, but skipped while a run is already in flight and
    /// self-guarded when no A/B render exists yet.
    void autoRun()
    {
        if (!run_.isBusy()) runXray();
    }

  private:
    void runXray();

    juce::TextButton xrayButton_{TRANS("X-Ray Current Render")};
    /// Status line, error state and the run button's busy state (UX-8).
    PanelRunState run_{this, &xrayButton_};
    juce::Label eventsLabel_;

    std::vector<float> curveDb_;
    std::vector<float> brightnessDb_; ///< tonal delta over time (processed - original)
    float maxReductionDb_ = 0.0f;
    float staticOffsetDb_ = 0.0f;
    juce::String verdict_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CompressionXrayPanel)
};

} // namespace khensu
