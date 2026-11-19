// desktop-app/Source/ui/MixMasterEngine/GainStagingPanel.cpp
#include "GainStagingPanel.h"
#include "../shared/KhensuLookAndFeel.h"
#include <khensu/targets/PlatformTargets.h>

namespace khensu {

GainStagingPanel::GainStagingPanel(IAppController& c) : PanelCard(c, "Gain Staging")
{
    setExplainer(TRANS("Gain Staging"), TRANS("Key loudness and dynamics metrics from the analysis. "
                                 "LUFS: integrated loudness. LRA: loudness range. "
                                 "Crest Factor: peak-to-RMS ratio (dynamics headroom). "
                                 "DR: dynamic range. True Peak: max instantaneous peak."));

    lufsMeter_.setRange(-36.0f, 0.0f);
    lufsMeter_.setTargetZone(-18.0f, -9.0f);

    lraMeter_.setRange(0.0f, 20.0f);
    lraMeter_.setTargetZone(4.0f, 9.0f);

    crestMeter_.setRange(0.0f, 30.0f);
    crestMeter_.setTargetZone(6.0f, 18.0f);

    drMeter_.setRange(0.0f, 20.0f);
    drMeter_.setTargetZone(5.0f, 14.0f);

    addAndMakeVisible(lufsMeter_);
    addAndMakeVisible(lraMeter_);
    addAndMakeVisible(crestMeter_);
    addAndMakeVisible(drMeter_);
    addAndMakeVisible(peakBadge_);
    addAndMakeVisible(hyperCompBadge_);

    gainCalloutLabel_.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::plain));
    gainCalloutLabel_.setText("", juce::dontSendNotification);
    gainCalloutLabel_.setMinimumHorizontalScale(0.5f);
    addAndMakeVisible(gainCalloutLabel_);
}

void GainStagingPanel::setData(const AnalysisResult& r)
{
    const float lufs = r.loudness.lufs_integrated;
    lufsMeter_.setValue(lufs, "LUFS: " + juce::String(lufs, 1));

    const float lra = r.loudness.lra;
    lraMeter_.setValue(lra, "LRA: " + juce::String(lra, 1) + " LU");

    const float crest = r.dynamics.crest_factor_db;
    crestMeter_.setValue(crest, "Crest: " + juce::String(crest, 1) + " dB");

    const float dr = r.dynamics.dynamic_range_db;
    drMeter_.setValue(dr, "DR: " + juce::String(dr, 1) + " dB");

    const float peak = r.loudness.true_peak_dbtp;
    const BadgeStatus peakStatus = peak > 0.0f    ? BadgeStatus::Fail
                                   : peak > -1.0f ? BadgeStatus::Warn
                                                  : BadgeStatus::Pass;
    peakBadge_.setBadge("TP " + juce::String(peak, 1) + " dBTP", peakStatus);

    // Hypercompression / over-processing gauge from crest + PLR.
    // crest < 6 dB or PLR < 8 dB indicates a squashed master; the more
    // extreme, the redder the badge.
    const float plr = r.loudness.plr;
    const bool squashed = crest < 6.0f || plr < 8.0f;
    const bool severe = crest < 4.0f || plr < 6.0f;
    if (severe)
        hyperCompBadge_.setBadge("OVER-COMPRESSED", BadgeStatus::Fail);
    else if (squashed)
        hyperCompBadge_.setBadge("HOT / TIGHT", BadgeStatus::Warn);
    else
        hyperCompBadge_.setBadge("DYNAMICS OK", BadgeStatus::Pass);

    // Compute gain adjustment toward streaming target (-14 LUFS from shared targets)
    float targetDbtp = -1.0f;
    const auto& platforms = getPlatformLoudnessTargets();
    gainAdjustDb_ = 0.0f;
    if (!platforms.empty()) {
        targetDbtp = platforms[0].dbtp;
        // The platform's real playback gain: never past its peak ceiling (UIX-12).
        gainAdjustDb_ = platformPlaybackGainDb(platforms[0], r.loudness.lufs_integrated, r.loudness.true_peak_dbtp);
    }

    // Predict peak after adjustment (target ceiling for streaming safety)
    float peakAfter = r.loudness.true_peak_dbtp + gainAdjustDb_;
    bool willClip = peakAfter > targetDbtp;

    // Update callout label
    if (willClip) {
        gainCalloutLabel_.setText("(!)  " + TRANS("Peak after adjustment: %p dBTP - WILL CLIP")
                                                .replace("%p", juce::String(peakAfter, 1)),
                                  juce::dontSendNotification);
        gainCalloutLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentAmber);
    } else {
        gainCalloutLabel_.setText("+  " + TRANS("Peak after adjustment: %p dBTP - Safe")
                                              .replace("%p", juce::String(peakAfter, 1)),
                                  juce::dontSendNotification);
        gainCalloutLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentTeal);
    }

    balanceLow_ = r.spectral.balance_low;
    balanceMid_ = r.spectral.balance_mid;
    balanceHigh_ = r.spectral.balance_high;
    stereoWidth_ = r.stereo.width;
    stereoCorr_ = r.stereo.correlation;

    repaint();
}

void GainStagingPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    lufsMeter_.setBounds(b.removeFromTop(34));
    b.removeFromTop(6);
    lraMeter_.setBounds(b.removeFromTop(34));
    b.removeFromTop(6);
    crestMeter_.setBounds(b.removeFromTop(34));
    b.removeFromTop(6);
    drMeter_.setBounds(b.removeFromTop(34));
    b.removeFromTop(8);
    peakBadge_.setBounds(b.removeFromTop(26));
    b.removeFromTop(4);
    hyperCompBadge_.setBounds(b.removeFromTop(26));
    b.removeFromTop(6);
    gainCalloutLabel_.setBounds(b.removeFromTop(18));
    b.removeFromTop(8);
    specBalanceArea_ = b.removeFromTop(44);
    b.removeFromTop(6);
    stereoBarArea_ = b.removeFromTop(24);
}

void GainStagingPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (specBalanceArea_.isEmpty()) return;

    // ── Spectral Balance ─────────────────────────────────────────────
    auto sa = specBalanceArea_.toFloat();
    // label row
    g.setFont(KhensuTheme::labelFont(10.0f));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText("SPECTRAL BALANCE", sa.removeFromTop(14.0f), juce::Justification::centredLeft, false);
    // 3 coloured segments proportional to balance_low/mid/high
    float total = juce::jmax(0.001f, balanceLow_ + balanceMid_ + balanceHigh_);
    float wLow = sa.getWidth() * (balanceLow_ / total);
    float wMid = sa.getWidth() * (balanceMid_ / total);
    float wHigh = sa.getWidth() * (balanceHigh_ / total);
    auto bar = sa.reduced(0.0f, 2.0f);
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.7f));
    g.fillRoundedRectangle({bar.getX(), bar.getY(), wLow, bar.getHeight()}, 3.0f);
    g.setColour(KhensuTheme::accentBlue.withAlpha(0.7f));
    g.fillRoundedRectangle({bar.getX() + wLow, bar.getY(), wMid, bar.getHeight()}, 3.0f);
    g.setColour(KhensuTheme::accentAmber.withAlpha(0.7f));
    g.fillRoundedRectangle({bar.getX() + wLow + wMid, bar.getY(), wHigh, bar.getHeight()}, 3.0f);

    // ── Stereo Width ─────────────────────────────────────────────────
    auto st = stereoBarArea_.toFloat();
    // background track
    g.setColour(KhensuTheme::bgRaised);
    g.fillRoundedRectangle(st, 3.0f);
    // fill: teal if wide, amber if narrow
    float fillW = st.getWidth() * juce::jlimit(0.0f, 1.0f, stereoWidth_);
    juce::Colour fillCol = (stereoWidth_ < 0.2f)   ? KhensuTheme::accentAmber
                           : (stereoCorr_ < -0.2f) ? KhensuTheme::accentRed
                                                   : KhensuTheme::accentTeal;
    g.setColour(fillCol.withAlpha(0.75f));
    g.fillRoundedRectangle({st.getX(), st.getY(), fillW, st.getHeight()}, 3.0f);
    // border
    g.setColour(KhensuTheme::borderSubtle);
    g.drawRoundedRectangle(st, 3.0f, 1.0f);
    // label
    g.setFont(KhensuTheme::labelFont(10.0f));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText("WIDTH  " + juce::String(juce::roundToInt(stereoWidth_ * 100.0f)) + "%", stereoBarArea_, juce::Justification::centred,
               false);
}

} // namespace khensu
