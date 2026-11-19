// desktop-app/Source/ui/Dashboard/ArtifactPanel.cpp
#include "ArtifactPanel.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/RecommendationExplainer.h"

namespace khensu {

namespace {
const juce::String kArtifactExplainerBase =
    "Detects digital clipping and the noise floor level, which hurt perceived quality and translation. "
    "The sample-reversal rate is an uncalibrated, informational signal statistic - it follows the "
    "spectral slope and sample rate (white noise reads ~0.67, a typical mix ~0.53) and is NOT an "
    "AI-generation detector. Use the AI Artifacts panel for the calibrated forensic scan.";
}

// Every cached palette colour on this component, in one place (UI-01).
void ArtifactPanel::applyThemeColours()
{
    noiseLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

ArtifactPanel::ArtifactPanel(IAppController& c) : PanelCard(c, "Artifacts")
{
    applyThemeColours();
    setExplainer(TRANS("Artifacts"), kArtifactExplainerBase, "artifact_detection");
    addAndMakeVisible(clippingBadge_);
    addAndMakeVisible(aiBadge_);
    noiseLabel_.setFont(khensu::KhensuTheme::uiFont(12.0f, juce::Font::plain));
    addAndMakeVisible(noiseLabel_);

    // -80..0 dB covers silence to full-scale; -50 dB and quieter reads as a
    // clean noise floor for a mastered track.
    noiseFloorMeter_.setRange(-80.0f, 0.0f);
    noiseFloorMeter_.setTargetZone(-80.0f, -50.0f);
    addAndMakeVisible(noiseFloorMeter_);

    // Raw 0..1 rate with no target zone: it is uncalibrated, so no band
    // of it can be called "good" (AUDIT-010).
    aiConfidenceMeter_.setRange(0.0f, 1.0f);
    aiConfidenceMeter_.setTitle(TRANS("Sample-reversal rate"));
    aiConfidenceMeter_.setHelpText(TRANS("Share of samples where the waveform's slope "
                                        "changes direction. Uncalibrated and informational only - not an "
                                        "AI-generation score."));
    addAndMakeVisible(aiConfidenceMeter_);
}

void ArtifactPanel::setData(const AnalysisResult& r)
{
    clippingBadge_.setBadge(r.artifacts.has_clipping
                                ? "Clipping (" + juce::String(r.artifacts.clipping_frame_count) + " frames)"
                                : "No clipping",
                            r.artifacts.has_clipping ? BadgeStatus::Fail : BadgeStatus::Pass);
    // The fast pass has no calibrated AI detector (AUDIT-010): it never
    // badges "AI artifacts" or "No AI artifacts". The AI Detection screen does.
    aiBadge_.setBadge(TRANS("AI check: not assessed here"), BadgeStatus::Neutral);
    noiseLabel_.setText(TRANS("Noise floor: ") + juce::String(r.artifacts.noise_floor_db, 1) + " dB",
                        juce::dontSendNotification);
    noiseFloorMeter_.setValue(r.artifacts.noise_floor_db,
                              "Noise floor: " + juce::String(r.artifacts.noise_floor_db, 1) + " dB");
    aiConfidenceMeter_.setValue(r.artifacts.sample_reversal_rate,
                                TRANS("Sample-reversal rate (uncalibrated, informational):") + " " +
                                    juce::String(r.artifacts.sample_reversal_rate, 2));

    const auto* rec =
        explainer::findTopRecommendation(r.recommendations, {"clipping_detected", "noise_floor_high", "dc_offset"});
    setExplainer(TRANS("Artifacts"), explainer::appendReasoningTrace(kArtifactExplainerBase, rec), "artifact_detection");

    repaint();
}

void ArtifactPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto row = b.removeFromTop(26);
    clippingBadge_.setBounds(row.removeFromLeft(row.getWidth() / 2 - 6));
    row.removeFromLeft(12);
    aiBadge_.setBounds(row);
    b.removeFromTop(8);
    noiseLabel_.setBounds(b.removeFromTop(18));
    b.removeFromTop(10);
    noiseFloorMeter_.setBounds(b.removeFromTop(34));
    b.removeFromTop(10);
    aiConfidenceMeter_.setBounds(b.removeFromTop(34));
}

} // namespace khensu
