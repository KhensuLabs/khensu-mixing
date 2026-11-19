// desktop-app/Source/ui/Dashboard/SummaryPanel.cpp
#include "SummaryPanel.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/MusicalContextText.h"
#include <cmath>

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void SummaryPanel::applyThemeColours()
{
    headline_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
}

SummaryPanel::SummaryPanel(IAppController& c) : PanelCard(c, "Summary")
{
    applyThemeColours();
    setExplainer(TRANS("Overview"), TRANS("A quick health check of the master: loudness, dynamics, phase, and artifacts. "
                             "Green means within typical bounds; amber and red flag areas to review. "
                             "BPM and key are available after deep analysis completes."));
    headline_.setFont(khensu::KhensuTheme::uiFont(13.0f, juce::Font::plain));
    addAndMakeVisible(headline_);
    for (auto* b : {&loudnessBadge_, &dynamicsBadge_, &phaseBadge_, &artifactBadge_, &bpmBadge_, &keyBadge_})
        addAndMakeVisible(*b);
}

void SummaryPanel::setData(const AnalysisResult& r)
{
    const bool isBatch = r.batch_results.size() > 1;

    if (isBatch) {
        const bool peakHot = r.loudness.true_peak_dbtp > -1.0f;
        loudnessBadge_.setBadge("Avg LUFS: " + juce::String(r.loudness.lufs_integrated, 1) +
                                    (peakHot ? "  (peak hot)" : ""),
                                peakHot ? BadgeStatus::Fail : BadgeStatus::Pass);
        dynamicsBadge_.setBadge("Avg DR " + juce::String(r.dynamics.dynamic_range_db, 1),
                                r.dynamics.dynamic_range_db < 5.0f ? BadgeStatus::Warn : BadgeStatus::Pass);
        phaseBadge_.setBadge("Batch Phase OK", BadgeStatus::Pass);
        artifactBadge_.setBadge("Batch Artifacts OK", BadgeStatus::Pass);

        // Octave-folded consensus, not a mean (AUDIT-050): flag disagreement.
        const int bpm = (int)std::round(r.python_data.bpm_detected);
        bpmBadge_.setBadge(TRANS("BPM") + " " + juce::String(bpm) + " " + TRANS("(consensus)"),
                           r.python_data.bpm_confidence > 0.7f ? BadgeStatus::Pass : BadgeStatus::Warn);
        keyBadge_.setBadge("Key: Mixed", BadgeStatus::Neutral);

        headline_.setText(TRANS("Batch analysis complete (") + juce::String(r.batch_results.size()) + " tracks).",
                          juce::dontSendNotification);
    } else {
        const bool peakHot = r.loudness.true_peak_dbtp > -1.0f;
        loudnessBadge_.setBadge("Loudness " + juce::String(r.loudness.lufs_integrated, 1) + " LUFS" +
                                    (peakHot ? "  (peak hot)" : ""),
                                peakHot ? BadgeStatus::Fail : BadgeStatus::Pass);
        dynamicsBadge_.setBadge("DR " + juce::String(r.dynamics.dynamic_range_db, 1),
                                r.dynamics.dynamic_range_db < 5.0f ? BadgeStatus::Warn : BadgeStatus::Pass);
        phaseBadge_.setBadge(r.phase.has_polarity_issue ? "Phase issue" : "Phase OK",
                             r.phase.has_polarity_issue ? BadgeStatus::Fail : BadgeStatus::Pass);
        artifactBadge_.setBadge(r.artifacts.has_clipping ? "Clipping" : "No artifacts",
                                r.artifacts.has_clipping ? BadgeStatus::Fail : BadgeStatus::Pass);

        if (r.has_python_analysis()) {
            // Meter / felt tempo / Camelot / confidence (AUDIT-052).
            const int bpm = (int)std::round(r.python_data.bpm_detected);
            const auto tempoDetail = musical_context::tempoDetail(r.python_data);
            bpmBadge_.setBadge("BPM " + juce::String(bpm) + (tempoDetail.isNotEmpty() ? "  " + tempoDetail : ""),
                               r.python_data.bpm_confidence > 0.7f ? BadgeStatus::Pass : BadgeStatus::Warn);
            auto key = juce::String(r.python_data.pitch_key);
            const auto keyDetail = musical_context::keyDetail(r.python_data);
            keyBadge_.setBadge(key.isEmpty() ? "Key ---"
                                             : "Key " + key + (keyDetail.isNotEmpty() ? "  " + keyDetail : ""),
                               r.python_data.pitch_confidence > 0.6f ? BadgeStatus::Pass : BadgeStatus::Warn);
        } else {
            bpmBadge_.setBadge("BPM ---", BadgeStatus::Neutral);
            keyBadge_.setBadge("Key ---", BadgeStatus::Neutral);
        }

        headline_.setText(r.is_partial ? "Fast analysis ready - deep analysis running..." : "Analysis complete.",
                          juce::dontSendNotification);
    }
    repaint();
}

void SummaryPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    headline_.setBounds(b.removeFromTop(20));
    b.removeFromTop(6);
    const int bw = b.getWidth() / 2 - 6, bh = 26, gap = 8;
    // Row 1: Loudness + DR
    loudnessBadge_.setBounds(b.getX(), b.getY(), bw, bh);
    dynamicsBadge_.setBounds(b.getX() + bw + 12, b.getY(), bw, bh);
    // Row 2: Phase + Artifacts
    phaseBadge_.setBounds(b.getX(), b.getY() + bh + gap, bw, bh);
    artifactBadge_.setBounds(b.getX() + bw + 12, b.getY() + bh + gap, bw, bh);
    // Row 3: BPM + Key
    bpmBadge_.setBounds(b.getX(), b.getY() + (bh + gap) * 2, bw, bh);
    keyBadge_.setBounds(b.getX() + bw + 12, b.getY() + (bh + gap) * 2, bw, bh);
}

} // namespace khensu
