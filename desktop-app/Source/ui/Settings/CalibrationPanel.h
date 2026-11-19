// desktop-app/Source/ui/Settings/CalibrationPanel.h
// CUT-10: lives in Settings > Monitoring - calibration belongs to the room,
// not to a track, so it is no longer a per-track Dashboard card.
// Monitor Calibration Wizard (K-System style): plays -20 dBFS RMS pink
// noise through the normal playback path, the user reads SPL at the mix
// position (any phone SPL meter, C-weighted, slow), and the wizard
// computes the offset from the reference level and stores the calibration
// profile. Calibrated monitoring is the root of consistent decisions -
// every level judgment is made against a known reference, not today's
// volume-knob mood.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"

namespace khensu {

class IAppController;

class CalibrationPanel final : public PanelCard {
  public:
    explicit CalibrationPanel(IAppController& c);
    void resized() override;

  private:
    void togglePinkNoise();
    void saveCalibration();
    void loadSavedCalibration();
    juce::File generatePinkNoiseFile();

    juce::TextButton noiseButton_{TRANS("Play Pink Noise (-20 dBFS)")};
    juce::ComboBox targetBox_; // reference SPL by room size
    juce::Label splLabel_;
    juce::TextEditor splInput_; // measured SPL at mix position
    juce::TextButton saveButton_{TRANS("Save Calibration")};
    juce::Label statusLabel_;

    bool noisePlaying_ = false;
    juce::String restorePath_; // file loaded before pink noise, to restore
    juce::File pinkFile_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CalibrationPanel)
};

} // namespace khensu
