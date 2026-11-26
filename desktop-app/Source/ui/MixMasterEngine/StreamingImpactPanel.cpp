// desktop-app/Source/ui/MixMasterEngine/StreamingImpactPanel.cpp
#include "StreamingImpactPanel.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../../IAppController.h"
#include "../shared/FeatureHomeLink.h"
#include <khensu/targets/PlatformTargets.h>

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void StreamingImpactPanel::applyThemeColours()
{
    previewButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    previewButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
}

StreamingImpactPanel::StreamingImpactPanel(IAppController& c) : PanelCard(c, "Streaming preview")
{
    applyThemeColours();
    setExplainer(TRANS("Streaming preview"),
                 TRANS("Every platform normalizes loudness to its own target, quietly turning hot mixes "
                 "down. Most never turn a quiet mix up; Spotify does, but only as far as its true-peak "
                 "ceiling allows. Preview auditions your mix as that platform's listeners will hear it. "
                 "The table of every platform lives on the Analysis screen (Loudness & delivery > "
                 "Streaming)."));

    for (const auto& p : getPlatformLoudnessTargets())
        platformCombo_.addItem(juce::String(p.name), platformCombo_.getNumItems() + 1);
    if (platformCombo_.getNumItems() > 0) platformCombo_.setSelectedItemIndex(0, juce::dontSendNotification);
    platformCombo_.onChange = [this] {
        if (previewOn_) {
            // Re-arm the preview at the newly selected platform rather than
            // leaving it applied at the previous one.
            controller_.playback().clearStreamingPreviewGain();
            previewOn_ = false;
            togglePreview();
        }
        updatePenaltyText();
    };
    addAndMakeVisible(platformCombo_);

    previewButton_.onClick = [this] { togglePreview(); };
    addAndMakeVisible(previewButton_);

    penaltyLabel_.setFont(KhensuTheme::bodyFont(KhensuTheme::kFontBody));
    penaltyLabel_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(penaltyLabel_);

    // ENH-8: the per-platform table is the Analysis screen's (Loudness &
    // delivery > Streaming); this card keeps the audition and one line.
    tableLinkButton_.setTooltip(TRANS("Every platform's target and turn-down, on the Analysis screen"));
    tableLinkButton_.onClick = [this] { feature_home::showAnalysisCard(controller_.shell(), "streaming_norm"); };
    addAndMakeVisible(tableLinkButton_);
}

void StreamingImpactPanel::setData(const AnalysisResult& r)
{
    intLufs_ = r.loudness.lufs_integrated;
    truePeakDbtp_ = r.loudness.true_peak_dbtp;
    updatePenaltyText();
    repaint();
}

void StreamingImpactPanel::togglePreview()
{
    if (previewOn_) {
        previewOn_ = false;
        controller_.playback().clearStreamingPreviewGain();
        previewButton_.setButtonText(TRANS("Preview at Platform Loudness"));
        return;
    }
    const int idx = platformCombo_.getSelectedItemIndex();
    const auto& targets = getPlatformLoudnessTargets();
    if (idx < 0 || idx >= (int)targets.size()) return;

    // What the platform really does: down freely, up only where it turns up
    // at all and never past its peak ceiling (UIX-12).
    const float adj = platformPlaybackGainDb(targets[(size_t)idx], intLufs_, truePeakDbtp_);
    controller_.playback().setStreamingPreviewGainDb(adj);
    previewOn_ = true;
    previewButton_.setButtonText(TRANS("Stop Preview (%p)").replace("%p", juce::String(targets[(size_t)idx].name)));
}

void StreamingImpactPanel::updatePenaltyText()
{
    const int idx = platformCombo_.getSelectedItemIndex();
    const auto& targets = getPlatformLoudnessTargets();
    if (idx < 0 || idx >= (int)targets.size()) {
        penaltyLabel_.setText({}, juce::dontSendNotification);
        return;
    }

    const auto& p = targets[(size_t)idx];
    const float adj = platformPlaybackGainDb(p, intLufs_, truePeakDbtp_);
    const juce::String played = TRANS("%p plays it at %d dB.")
                                    .replace("%p", juce::String(p.name))
                                    .replace("%d", (adj < 0.0f ? juce::String("-") : juce::String("+"))
                                                       + juce::String(std::abs(adj), 1));
    const float shortfall = p.lufs - intLufs_ - juce::jmax(0.0f, adj); // quieter than the target after playback gain

    juce::StringArray warnings;
    if (adj < -1.5f)
        warnings.add(TRANS("-%d dB turn-down on %p - your loud master won't sound louder than quieter tracks there.")
                         .replace("%d", juce::String(std::abs(adj), 1))
                         .replace("%p", juce::String(p.name)));
    if (intLufs_ > -70.0f && shortfall > 1.5f)
        warnings.add(TRANS("Plays %d dB below %p's target - it won't be turned up to match louder tracks.")
                         .replace("%d", juce::String(shortfall, 1))
                         .replace("%p", juce::String(p.name)));
    if (truePeakDbtp_ > p.dbtp)
        warnings.add(TRANS("True peak %t dBTP exceeds %p's %c dBTP ceiling - risk of inter-sample clipping after "
                           "their encode.")
                         .replace("%t", juce::String(truePeakDbtp_, 1))
                         .replace("%p", juce::String(p.name))
                         .replace("%c", juce::String(p.dbtp, 1)));

    penaltyLabel_.setColour(juce::Label::textColourId,
                            warnings.isEmpty() ? KhensuTheme::textTertiary : KhensuTheme::accentAmber);
    // One line (ENH-8): the playback gain and the first warning; the
    // tooltip carries every warning.
    penaltyLabel_.setText(played + " "
                              + (warnings.isEmpty() ? TRANS("No penalties predicted for this platform.")
                                                    : warnings[0]),
                          juce::dontSendNotification);
    penaltyLabel_.setTooltip(warnings.isEmpty() ? juce::String() : warnings.joinIntoString("\n"));
}

void StreamingImpactPanel::resized()
{
    PanelCard::resized();
    // One row: platform, audition, the line, the link to the full table.
    auto row = bodyArea().withSizeKeepingCentre(bodyArea().getWidth(), KhensuTheme::buttonH);
    platformCombo_.setBounds(row.removeFromLeft(150));
    row.removeFromLeft(KhensuTheme::sp2);
    previewButton_.setBounds(row.removeFromLeft(230));
    row.removeFromLeft(KhensuTheme::sp3);
    tableLinkButton_.setBounds(row.removeFromRight(juce::jmin(200, juce::jmax(0, row.getWidth() / 3))));
    row.removeFromRight(KhensuTheme::sp3);
    penaltyLabel_.setBounds(row);
}

void StreamingImpactPanel::paint(juce::Graphics& g) { PanelCard::paint(g); }

} // namespace khensu
