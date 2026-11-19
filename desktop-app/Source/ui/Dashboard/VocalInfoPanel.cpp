// desktop-app/Source/ui/Dashboard/VocalInfoPanel.cpp
#include "VocalInfoPanel.h"
#include "../../IAppController.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include <cmath>
#include "../../session/StemSeparationInfo.h"

namespace khensu {

void VocalFingerprintStrip::paint(juce::Graphics& g)
{
    g.setFont(KhensuTheme::labelFont(9.0f));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText(TRANS("VOCAL FINGERPRINT (3D)"), getLocalBounds().withHeight(12), juce::Justification::centredLeft, false);

    if (!hasData_) {
        g.setFont(KhensuTheme::bodyFont(11.0f));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(emptyText_.isEmpty() ? TRANS("No stems yet - isolate stems to see the vocal fingerprint.")
                                        : emptyText_,
                   getLocalBounds().withTrimmedTop(16),
                   juce::Justification::centredLeft, false);
        return;
    }

    constexpr float kDepth = 5.0f;
    auto b = getLocalBounds().withTrimmedTop(13).toFloat();
    b.removeFromRight(kDepth);
    b.removeFromTop(kDepth);
    if (b.getHeight() < 4.0f) return;

    struct Bar {
        const char* label;
        float value;
        juce::Colour colour;
    };
    const Bar bars[] = {
        {"Loud", loudness_, KhensuTheme::accentTeal},
        {"Bright", brightness_, KhensuTheme::accentAmber},
        {"Dynam", dynamics_, KhensuTheme::accentBlue},
    };
    constexpr int n = 3;
    constexpr float spacing = 8.0f;
    const float barW = (b.getWidth() - spacing * (n - 1)) / (float)n;

    g.setFont(KhensuTheme::labelFont(9.0f));
    for (int i = 0; i < n; ++i) {
        const float t = juce::jlimit(0.0f, 1.0f, bars[i].value);
        const float bx = b.getX() + (float)i * (barW + spacing);
        const float bh = b.getHeight() * (0.15f + 0.75f * t);
        const float by = b.getBottom() - bh - 11.0f;

        KhensuLookAndFeel::draw3DHeatBar(g, {bx, by, barW, bh}, bars[i].colour, kDepth);

        g.setColour(KhensuTheme::textTertiary);
        g.drawText(bars[i].label, juce::Rectangle<float>(bx, b.getBottom() - 10.0f, barW, 10.0f),
                   juce::Justification::centred, false);
    }
}

VocalInfoPanel::VocalInfoPanel(IAppController& c) : PanelCard(c, "Vocal Info")
{
    setExplainer(TRANS("Vocal Info"), TRANS("Hook strength/density come from deep analysis of the full mix. Once stems finish "
                               "separating (now triggered automatically at intake, not just from the Vocal Workspace "
                               "button), the fingerprint below reflects the isolated vocal's own loudness, "
                               "brightness, and dynamics."));

    addAndMakeVisible(stemStatusBadge_);
    addAndMakeVisible(hookBadge_);
    addAndMakeVisible(densityBadge_);
    addAndMakeVisible(fingerprint_);

    refreshStemStatus();
}

void VocalInfoPanel::setData(const AnalysisResult& r)
{
    // Hook numbers come from the deep pass only; without it the badges stay
    // neutral placeholders (demo values used to stand in, even after a real
    // C++-only analysis).
    if (r.has_python_analysis()) {
        const float hookStr = r.python_data.hooks.hook_strength;
        const float hookDens = r.python_data.hooks.hook_density;
        hookBadge_.setBadge(TRANS("Hook Strength") + " " + juce::String(juce::roundToInt(hookStr * 100.0f)) + "%",
                            hookStr > 0.5f ? BadgeStatus::Pass : BadgeStatus::Neutral);
        densityBadge_.setBadge(TRANS("Hook Density") + " " + juce::String(hookDens, 2), BadgeStatus::Neutral);
    } else {
        const auto missing = r.has_fast_analysis() ? TRANS("needs deep analysis") : juce::String("-");
        hookBadge_.setBadge(TRANS("Hook Strength") + " " + missing, BadgeStatus::Neutral);
        densityBadge_.setBadge(TRANS("Hook Density") + " " + missing, BadgeStatus::Neutral);
    }
    refreshStemStatus();
    repaint();
}

void VocalInfoPanel::refreshStemStatus()
{
    // WIRE-06 / AID-07: an honest stems badge - green only for real
    // separated stems; the HPSS fallback is "approximate" (amber), and no
    // stems is "none" (neutral), never a green "Ready (Demo)".
    if (controller_.stems().isSeparatingStems()) {
        stemStatusBadge_.setBadge(TRANS("Stems: Isolating..."), BadgeStatus::Warn);
    } else if (controller_.stems().hasStemsAvailable()) {
        stemStatusBadge_.setBadge(TRANS("Stems: Ready"), BadgeStatus::Pass);
    } else if (controller_.stems().hasApproximateStems()) {
        stemStatusBadge_.setBadge(TRANS("Stems: Approximate (HPSS)"), BadgeStatus::Warn);
        stemStatusBadge_.setDescription(controller_.stems().stemSeparationInfo().describe());
    } else if (!controller_.hasAnalysis()) {
        stemStatusBadge_.setBadge(TRANS("Stems: - (Demo)"), BadgeStatus::Neutral);
    } else {
        stemStatusBadge_.setBadge(TRANS("Stems: None"), BadgeStatus::Neutral);
    }

    if (controller_.stems().hasVocalAnalysis()) {
        const auto& v = controller_.stems().currentVocalAnalysis();
        const float loudNorm = juce::jlimit(0.0f, 1.0f, (v.loudness.lufs_integrated + 30.0f) / 24.0f);
        const float brightNorm = juce::jlimit(0.0f, 1.0f, v.spectral.centroid_hz / 6000.0f);
        const float dynNorm = juce::jlimit(0.0f, 1.0f, v.dynamics.crest_factor_db / 20.0f);
        fingerprint_.setValues(loudNorm, brightNorm, dynNorm);
    } else {
        // No vocal analysis -> honest empty state, never invented bars.
        fingerprint_.setEmptyText(controller_.stems().isSeparatingStems()
                                      ? TRANS("Waiting for stem separation...")
                                      : TRANS("No stems yet - isolate stems to see the vocal fingerprint."));
        fingerprint_.clear();
    }
    resized();
    repaint();
}

void VocalInfoPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();

    stemStatusBadge_.setBounds(b.removeFromTop(26));
    b.removeFromTop(6);

    auto row = b.removeFromTop(26);
    hookBadge_.setBounds(row.removeFromLeft(row.getWidth() / 2 - 4));
    row.removeFromLeft(8);
    densityBadge_.setBounds(row);
    b.removeFromTop(6);

    fingerprint_.setBounds(b);
}

} // namespace khensu
