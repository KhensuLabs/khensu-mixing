// desktop-app/Source/ui/VocalWorkspace/GainRiderPanel.h
// Smart Gain Rider: phrase-level clip-gain automation for the vocal stem,
// computed BEFORE compression - loud phrases pulled down, quiet phrases
// pushed up, each toward the -18 dBFS RMS sweet spot, so the compressor
// receives a consistent level and can work musically instead of fighting
// the performance's full dynamic range.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include <vector>

namespace khensu {

class IAppController;

class GainRiderPanel final : public PanelCard {
  public:
    explicit GainRiderPanel(IAppController& c);

    /// Intake auto-run (spec §1.2): computes the ride map only - rendering
    /// stays a user action. Skipped while a compute is in flight.
    void autoRun()
    {
        if (computeButton_.isEnabled()) computeRide();
    }
    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    juce::String targetFilePath() const;
    void computeRide();
    void renderRide();

    struct Phrase {
        float start = 0.0f, end = 0.0f;
        float rmsDb = 0.0f, gainDb = 0.0f;
    };

    juce::TextButton computeButton_{TRANS("Compute Gain Ride")};
    juce::TextButton renderButton_{TRANS("Render")};
    juce::Label statusLabel_;

    std::vector<Phrase> phrases_;
    float totalDuration_ = 0.0f;
    float noiseFloorDb_ = 0.0f;
    bool hasRide_ = false;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GainRiderPanel)
};

} // namespace khensu
