// desktop-app/Source/ui/Dashboard/SonicFingerprintPanel.cpp
#include "SonicFingerprintPanel.h"
#include "../../IAppController.h" // SonicOrbitComponent needs the full controller

namespace khensu {

SonicFingerprintPanel::SonicFingerprintPanel(IAppController& c)
    : PanelCard(c, "Sonic Fingerprint & Timbre Orbit"), orbit_(c)
{
    setExplainer(TRANS("Fingerprint & Orbit"),
                 TRANS("A six-axis snapshot of the master's character (left) alongside the "
                 "hardware-accelerated 3D Timbre Space (right) showing the song's "
                 "chronological section trajectory (brightness, dynamics, width) relative to reference."),
                 "sonic_fingerprint");

    addAndMakeVisible(radar_);
    addAndMakeVisible(orbit_);

    orbitModeToggle_.setClickingTogglesState(true);
    orbitModeToggle_.setTooltip(TRANS("Toggle between this track's chronological section trajectory "
                                "and a trail of every render applied so far this session."));
    orbitModeToggle_.onClick = [this] { orbit_.setRenderHistoryMode(orbitModeToggle_.getToggleState()); };
    addAndMakeVisible(orbitModeToggle_);
}

void SonicFingerprintPanel::setData(const AnalysisResult& r)
{
    auto norm = [](float v, float lo, float hi) { return juce::jlimit(0.0f, 1.0f, (v - lo) / (hi - lo)); };
    std::vector<RadarAxis> axes = {
        {"Loud", norm(r.loudness.lufs_integrated, -24.0f, -6.0f)},
        {"Dyn", norm(r.dynamics.dynamic_range_db, 0.0f, 20.0f)},
        {"Width", juce::jlimit(0.0f, 1.0f, r.stereo.width)},
        {"Low", juce::jlimit(0.0f, 1.0f, r.spectral.balance_low)},
        {"Bright", norm(r.spectral.centroid_hz, 500.0f, 6000.0f)},
        {"Memo", juce::jlimit(0.0f, 1.0f, r.python_data.memorability.score)},
    };
    radar_.setAxes(std::move(axes));
    orbit_.setAnalysisData(r);
    repaint();
}

void SonicFingerprintPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto left = b.removeFromLeft(b.getWidth() / 2 - 4);
    b.removeFromLeft(8);
    radar_.setBounds(left);
    orbitModeToggle_.setBounds(b.removeFromTop(20).removeFromRight(120));
    b.removeFromTop(4);
    orbit_.setBounds(b);
}

} // namespace khensu
