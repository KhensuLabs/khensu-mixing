// desktop-app/Source/ui/MixMasterEngine/StreamingImpactPanel.cpp
#include "StreamingImpactPanel.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../../IAppController.h"
#include <khensu/targets/PlatformTargets.h>

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void StreamingImpactPanel::applyThemeColours()
{
    previewButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    previewButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
}

StreamingImpactPanel::StreamingImpactPanel(IAppController& c) : PanelCard(c, "Streaming Impact")
{
    applyThemeColours();
    setExplainer(TRANS("Streaming Impact"),
                 TRANS("Every platform normalizes loudness to its own target, quietly turning hot mixes "
                 "down. Most never turn a quiet mix up; Spotify does, but only as far as its true-peak "
                 "ceiling allows. Preview auditions your mix as that platform's listeners will hear it, "
                 "and flags true-peak values likely to trigger its own limiter/re-encode."));

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

    penaltyLabel_.setFont(KhensuTheme::labelFont(10.0f));
    penaltyLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(penaltyLabel_);
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
    penaltyLabel_.setText(warnings.isEmpty() ? TRANS("No penalties predicted for this platform.")
                                             : warnings.joinIntoString("\n"),
                          juce::dontSendNotification);
}

void StreamingImpactPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controlsRow = b.removeFromBottom(26);
    platformCombo_.setBounds(controlsRow.removeFromLeft(130));
    controlsRow.removeFromLeft(6);
    previewButton_.setBounds(controlsRow);
    b.removeFromBottom(4);
    penaltyLabel_.setBounds(b.removeFromBottom(34));
}

void StreamingImpactPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    auto b = bodyArea().withTrimmedBottom(64); // reserve space for the preview controls + penalty text

    for (const auto& p : getPlatformLoudnessTargets()) {
        if (b.getHeight() < 20) break;
        auto row = b.removeFromTop(28);
        b.removeFromTop(4);

        const float adj = platformPlaybackGainDb(p, intLufs_, truePeakDbtp_); // negative = turned down
        const bool warn = adj < -1.5f;

        // Platform name
        g.setFont(KhensuTheme::bodyFont(12.0f));
        g.setColour(KhensuTheme::textSecondary);
        g.drawText(juce::String(p.name), row.removeFromLeft(110), juce::Justification::centredLeft, true);

        // Target LUFS
        g.setFont(KhensuTheme::monoFont(10.0f));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(juce::String(juce::roundToInt(p.lufs)) + " LUFS", row.removeFromLeft(72), juce::Justification::centredLeft, false);

        // Adjustment
        juce::String adjStr =
            (adj < 0.0f ? juce::String("-") : juce::String("+")) + juce::String(std::abs(adj), 1) + " dB";
        g.setColour(warn ? KhensuTheme::accentAmber : KhensuTheme::textSecondary);
        g.drawText(adjStr, row, juce::Justification::centredRight, false);
    }

    // True peak warning
    if (truePeakDbtp_ > -1.0f) {
        b.removeFromTop(4);
        g.setFont(KhensuTheme::labelFont(10.0f));
        g.setColour(KhensuTheme::accentAmber);
        g.drawText("(!)  " + TRANS("True peak > -1 dBTP - re-encode risk"), b, juce::Justification::centredLeft,
                   true);
    }
}

} // namespace khensu
