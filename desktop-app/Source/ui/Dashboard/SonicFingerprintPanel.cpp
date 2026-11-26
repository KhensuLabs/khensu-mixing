// desktop-app/Source/ui/Dashboard/SonicFingerprintPanel.cpp
#include "SonicFingerprintPanel.h"
#include "../../IAppController.h" // SonicOrbitComponent needs the full controller
#include "../shared/KhensuTheme.h"

namespace khensu {

SonicFingerprintPanel::SonicFingerprintPanel(IAppController& c)
    : PanelCard(c, "Sonic Fingerprint & Timbre Orbit"), orbit_(c)
{
    setExplainer(TRANS("Fingerprint & Orbit"),
                 TRANS("A six-axis snapshot of the master's character (left) alongside the "
                 "hardware-accelerated 3D Timbre Space (right) showing the song's "
                 "chronological section trajectory (brightness, dynamics, width) relative to reference."),
                 "sonic_fingerprint");

    radar_.setTitle(TRANS("Sonic fingerprint"));
    addAndMakeVisible(radar_);
    addAndMakeVisible(orbit_);

    orbitModeToggle_.setClickingTogglesState(true);
    orbitModeToggle_.setTooltip(TRANS("Toggle between this track's chronological section trajectory "
                                "and a trail of every render applied so far this session."));
    orbitModeToggle_.onClick = [this] { orbit_.setRenderHistoryMode(orbitModeToggle_.getToggleState()); };
    addAndMakeVisible(orbitModeToggle_);

    // STU-12: fly-to presets (a spring glide; instant with reduced motion).
    flyGenreButton_.setTooltip(TRANS("Fly the camera to this session's genre in the catalog."));
    flyGenreButton_.onClick = [this] {
        orbit_.flyToMyGenre();
        orbitModeToggle_.setToggleState(orbit_.isRenderHistoryMode(), juce::dontSendNotification);
    };
    flyTrajectoryButton_.setTooltip(TRANS("Show the line from the original through each version and frame it."));
    flyTrajectoryButton_.onClick = [this] {
        orbit_.flyToTrajectory();
        orbitModeToggle_.setToggleState(orbit_.isRenderHistoryMode(), juce::dontSendNotification);
    };
    flyResetButton_.setTooltip(TRANS("Back to the whole space, centred on the reference target."));
    flyResetButton_.onClick = [this] { orbit_.resetView(); };
    for (auto* b : {&flyGenreButton_, &flyTrajectoryButton_, &flyResetButton_})
        addAndMakeVisible(*b);
}

void SonicFingerprintPanel::setData(const AnalysisResult& r)
{
    auto norm = [](float v, float lo, float hi) { return juce::jlimit(0.0f, 1.0f, (v - lo) / (hi - lo)); };
    // VIS-1: full names, and each spoke reads out the measured value with
    // its unit (the spoke's length is that value on the range shown).
    ui::chart::Axis hz;
    hz.format = ui::chart::Format::Hz;
    std::vector<RadarAxis> axes = {
        {TRANS("Loudness"), norm(r.loudness.lufs_integrated, -24.0f, -6.0f), juce::String(r.loudness.lufs_integrated, 1) + " LUFS"},
        {TRANS("Dynamics"), norm(r.dynamics.dynamic_range_db, 0.0f, 20.0f), juce::String(r.dynamics.dynamic_range_db, 1) + " dB DR"},
        {TRANS("Width"), juce::jlimit(0.0f, 1.0f, r.stereo.width), juce::String(juce::roundToInt(r.stereo.width * 100.0f)) + " %"},
        {TRANS("Low end"), juce::jlimit(0.0f, 1.0f, r.spectral.balance_low),
         juce::String(juce::roundToInt(r.spectral.balance_low * 100.0f)) + " %"},
        {TRANS("Brightness"), norm(r.spectral.centroid_hz, 500.0f, 6000.0f), hz.label(r.spectral.centroid_hz, true)},
        {TRANS("Memorability"), juce::jlimit(0.0f, 1.0f, r.python_data.memorability.score),
         juce::String(r.python_data.memorability.score, 2)},
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
    auto row = b.removeFromTop(KhensuTheme::buttonHSmall);
    orbitModeToggle_.setBounds(row.removeFromRight(120));
    row.removeFromRight(4);
    const int presetW = juce::jmax(0, row.getWidth() - 8) / 3;
    flyGenreButton_.setBounds(row.removeFromLeft(presetW));
    row.removeFromLeft(4);
    flyTrajectoryButton_.setBounds(row.removeFromLeft(presetW));
    row.removeFromLeft(4);
    flyResetButton_.setBounds(row.removeFromLeft(presetW));
    b.removeFromTop(4);
    orbit_.setBounds(b);
}

} // namespace khensu
