// desktop-app/Source/ui/MixMasterEngine/GainStagingPanel.cpp
#include "GainStagingPanel.h"
#include "../shared/KhensuTheme.h"
#include "../shared/ChartStyle.h"
#include <khensu/targets/PlatformTargets.h>
#include <khensu/ui/KhensuPalette.h>

namespace khensu {

namespace {
juce::Colour fitColour(gain_staging::Fit fit)
{
    switch (fit) {
    case gain_staging::Fit::Far: return KhensuTheme::accentRed;
    case gain_staging::Fit::Near: return KhensuTheme::accentAmber;
    case gain_staging::Fit::InTarget: break;
    }
    return KhensuTheme::textPrimary;
}

juce::Font numberFont(float size)
{
    return KhensuTheme::numericFont(size, juce::Font::plain);
}

/// VIS-4: where a value sits against its band, in words ("in target",
/// "2.1 over", "3.0 under") - the colour is never the only cue.
juce::String fitText(const GainStagingPanel::Gauge& gauge)
{
    if (gauge.value > gauge.hi) return "+" + juce::String(gauge.value - gauge.hi, 1) + " " + TRANS("over");
    if (gauge.value < gauge.lo) return "-" + juce::String(gauge.lo - gauge.value, 1) + " " + TRANS("under");
    return TRANS("in target");
}

/// VIS-1: the gauge's bar as a ChartKit axis (ticks, labels, the unit).
ui::chart::Axis gaugeAxis(const GainStagingPanel::Gauge& gauge)
{
    ui::chart::Axis a;
    a.lo = gauge.min;
    a.hi = gauge.max;
    a.unit = gauge.unit;
    return a;
}
} // namespace

GainStagingPanel::GainStagingPanel(IAppController& c) : PanelCard(c, TRANS("Gain staging"))
{
    setExplainer(TRANS("Gain staging"),
                 TRANS("Loudness and dynamics from the analysis, each against its target band (the outlined part of "
                       "the bar; the tick is the measured value). Integrated: whole-track loudness (LUFS). Range: "
                       "loudness range (LRA). Crest: peak-to-RMS ratio. DR: dynamic range. True peak: the highest "
                       "inter-sample peak. A value turns amber just outside its band and red further out."));
    gauges_[0] = {TRANS("Integrated"), "LUFS", 0.0f, -36.0f, 0.0f, -18.0f, -9.0f, false};
    gauges_[1] = {TRANS("Range"), "LU", 0.0f, 0.0f, 20.0f, 4.0f, 9.0f, false};
    gauges_[2] = {TRANS("Crest"), "dB", 0.0f, 0.0f, 30.0f, 6.0f, 18.0f, false};
    gauges_[3] = {TRANS("DR"), "dB", 0.0f, 0.0f, 20.0f, 5.0f, 14.0f, false};
    gauges_[4] = {TRANS("True peak"), "dBTP", 0.0f, -12.0f, 3.0f, -12.0f, -1.0f, false};
    // VIS-8: the gauges read as one image with a text summary.
    gaugesSummary_.setTitle(TRANS("Gain staging gauges"));
    gaugesSummary_.describe = [this] { return summary(); };
    addAndMakeVisible(gaugesSummary_);
}

juce::String GainStagingPanel::summary() const
{
    if (!hasData_) return TRANS("Gain staging: not measured yet.");
    juce::StringArray parts;
    for (const auto& gauge : gauges_) {
        if (!gauge.known) {
            parts.add(gauge.label + " " + TRANS("not measured"));
            continue;
        }
        parts.add(gauge.label + " " + juce::String(gauge.value, 1) + " " + gauge.unit + " (" + TRANS("target") + " " +
                  juce::String(gauge.lo, 1) + " " + TRANS("to") + " " + juce::String(gauge.hi, 1) + ", " + fitText(gauge) + ")");
    }
    return TRANS("Gain staging") + ": " + parts.joinIntoString("; ") + ". " + playbackLine_ + ". " + dynamicsLine_ + ".";
}

int GainStagingPanel::gaugeAt(juce::Point<int> pos) const
{
    for (size_t i = 0; i < gaugeAreas_.size(); ++i)
        if (gaugeAreas_[i].contains(pos)) return static_cast<int>(i);
    return -1;
}

void GainStagingPanel::mouseMove(const juce::MouseEvent& e)
{
    const int g = gaugeAt(e.getPosition());
    if (g != hoveredGauge_ || g >= 0) {
        hoveredGauge_ = g;
        hoverPos_ = e.position;
        repaint();
    }
}

void GainStagingPanel::mouseExit(const juce::MouseEvent&)
{
    if (hoveredGauge_ < 0) return;
    hoveredGauge_ = -1;
    repaint();
}

void GainStagingPanel::setData(const AnalysisResult& r)
{
    gauges_[0].value = r.loudness.lufs_integrated;
    gauges_[1].value = r.loudness.lra;
    gauges_[2].value = r.dynamics.crest_factor_db;
    gauges_[3].value = r.dynamics.dynamic_range_db;
    gauges_[4].value = r.loudness.true_peak_dbtp;
    for (auto& gauge : gauges_)
        gauge.known = std::isfinite(gauge.value);
    // VIZ-P6: a value the file is too short to measure is undefined ("-",
    // neutral), not a bad reading in red.
    gauges_[1].known = gauges_[1].known && r.loudness.lra_measured;
    gauges_[3].known = gauges_[3].known && r.dynamics.dynamic_range_measured;
    gauges_[4].known = gauges_[4].known && r.loudness.true_peak_dbtp > -98.0f;

    // What the first streaming platform's playback does (never past its peak
    // ceiling, UIX-12), and the peak after it.
    float ceiling = -1.0f, adjust = 0.0f;
    juce::String platform;
    const auto& platforms = getPlatformLoudnessTargets();
    if (!platforms.empty()) {
        ceiling = platforms[0].dbtp;
        platform = juce::String(platforms[0].name);
        adjust = platformPlaybackGainDb(platforms[0], r.loudness.lufs_integrated, r.loudness.true_peak_dbtp);
    }
    const float peakAfter = r.loudness.true_peak_dbtp + adjust;
    const bool willClip = peakAfter > ceiling;
    playbackFit_ = willClip ? gain_staging::Fit::Near : gain_staging::Fit::InTarget;
    playbackLine_ = TRANS("%p playback: %g dB, peak after %a dBTP - %v")
                        .replace("%p", platform.isNotEmpty() ? platform : TRANS("Streaming"))
                        .replace("%g", (adjust > 0.0f ? "+" : "") + juce::String(adjust, 1))
                        .replace("%a", juce::String(peakAfter, 1))
                        .replace("%v", willClip ? TRANS("will clip") : TRANS("safe"));

    // Hypercompression from crest + PLR: < 6 dB crest or < 8 dB PLR is hot.
    const float crest = r.dynamics.crest_factor_db;
    const float plr = r.loudness.plr;
    if (crest < 4.0f || plr < 6.0f) {
        dynamicsFit_ = gain_staging::Fit::Far;
        dynamicsLine_ = TRANS("Dynamics: over-compressed (PLR %v dB)").replace("%v", juce::String(plr, 1));
    } else if (crest < 6.0f || plr < 8.0f) {
        dynamicsFit_ = gain_staging::Fit::Near;
        dynamicsLine_ = TRANS("Dynamics: hot and tight (PLR %v dB)").replace("%v", juce::String(plr, 1));
    } else {
        dynamicsFit_ = gain_staging::Fit::InTarget;
        dynamicsLine_ = TRANS("Dynamics: healthy (PLR %v dB)").replace("%v", juce::String(plr, 1));
    }

    balanceLow_ = r.spectral.balance_low;
    balanceMid_ = r.spectral.balance_mid;
    balanceHigh_ = r.spectral.balance_high;
    stereoWidth_ = r.stereo.width;
    stereoCorr_ = r.stereo.correlation;
    hasData_ = true;
    setTitle(TRANS("Gain staging") + ": " + juce::String(gauges_[0].value, 1) + " LUFS, " +
             juce::String(gauges_[4].value, 1) + " dBTP");
    repaint();
}

void GainStagingPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea().reduced(KhensuTheme::sp1, 0);
    // Five tiles in a row when there is room, else three over two.
    const bool oneRow = b.getWidth() >= 5 * 112;
    const int tileH = 96;
    auto tiles = b.removeFromTop(oneRow ? tileH : 2 * tileH + KhensuTheme::sp2);
    if (oneRow) {
        const int w = tiles.getWidth() / 5;
        for (auto& a : gaugeAreas_)
            a = tiles.removeFromLeft(w).reduced(KhensuTheme::sp1, 0);
    } else {
        auto top = tiles.removeFromTop(tileH);
        tiles.removeFromTop(KhensuTheme::sp2);
        const int w = top.getWidth() / 3;
        for (size_t i = 0; i < 3; ++i)
            gaugeAreas_[i] = top.removeFromLeft(w).reduced(KhensuTheme::sp1, 0);
        for (size_t i = 3; i < 5; ++i)
            gaugeAreas_[i] = tiles.removeFromLeft(w).reduced(KhensuTheme::sp1, 0);
    }
    b.removeFromTop(KhensuTheme::sp3);
    gaugesSummary_.setBounds(gaugeAreas_[0].getUnion(gaugeAreas_[4]).getUnion(gaugeAreas_[2]));
    statusArea_ = b.removeFromTop(40).reduced(KhensuTheme::sp1, 0);
    b.removeFromTop(KhensuTheme::sp3);
    auto bottom = b.removeFromTop(44).reduced(KhensuTheme::sp1, 0);
    specBalanceArea_ = bottom.removeFromLeft(bottom.getWidth() * 3 / 5);
    bottom.removeFromLeft(KhensuTheme::sp4);
    stereoBarArea_ = bottom;
}

void GainStagingPanel::paintGauge(juce::Graphics& g, const Gauge& gauge, juce::Rectangle<int> area) const
{
    auto a = area.toFloat();
    g.setColour(KhensuTheme::bgRaised.withAlpha(0.45f));
    g.fillRoundedRectangle(a, 6.0f);
    a = a.reduced(10.0f, 8.0f);

    g.setColour(KhensuTheme::textTertiary);
    g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
    g.drawText(gauge.label, a.removeFromTop(14.0f), juce::Justification::centredLeft, true);

    const auto fit = gauge.known ? gain_staging::fit(gauge.value, gauge.lo, gauge.hi) : gain_staging::Fit::InTarget;
    auto valueRow = a.removeFromTop(36.0f);
    const auto valueFont = numberFont(KhensuTheme::kFontMetric);
    const juce::String valueText = gauge.known && hasData_ ? juce::String(gauge.value, 1) : juce::String("-");
    g.setFont(valueFont);
    g.setColour(hasData_ && gauge.known ? fitColour(fit) : KhensuTheme::textTertiary);
    const float vw = juce::GlyphArrangement::getStringWidth(valueFont, valueText);
    g.drawText(valueText, valueRow, juce::Justification::centredLeft, false);
    g.setColour(KhensuTheme::textTertiary);
    g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
    const float uw = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), gauge.unit) + 8.0f;
    g.drawText(gauge.unit, valueRow.withTrimmedLeft(vw + 5.0f).withTrimmedBottom(5.0f),
               juce::Justification::bottomLeft, false);
    // VIS-4: the verdict in words beside the number, not by colour alone.
    if (gauge.known && hasData_) {
        g.setColour(fit == gain_staging::Fit::InTarget ? KhensuTheme::textTertiary : fitColour(fit));
        g.drawText(fitText(gauge), valueRow.withTrimmedLeft(vw + 5.0f + uw).withTrimmedBottom(5.0f),
                   juce::Justification::bottomLeft, true);
    }

    // The bar: a neutral track, the target band outlined, the value as a tick.
    a.removeFromTop(6.0f);
    auto bar = a.removeFromTop(6.0f);
    g.setColour(KhensuTheme::textPrimary.withAlpha(0.08f));
    g.fillRoundedRectangle(bar, 3.0f);
    const float x0 = bar.getX() + bar.getWidth() * gain_staging::proportion(gauge.lo, gauge.min, gauge.max);
    const float x1 = bar.getX() + bar.getWidth() * gain_staging::proportion(gauge.hi, gauge.min, gauge.max);
    const auto band = juce::Rectangle<float>(x0, bar.getY(), juce::jmax(2.0f, x1 - x0), bar.getHeight());
    g.setColour(KhensuTheme::textPrimary.withAlpha(0.16f));
    g.fillRoundedRectangle(band, 3.0f);
    g.setColour(KhensuTheme::textSecondary.withAlpha(0.6f));
    g.drawRoundedRectangle(band, 3.0f, 1.0f);
    if (gauge.known && hasData_) {
        const float x = bar.getX() + bar.getWidth() * gain_staging::proportion(gauge.value, gauge.min, gauge.max);
        g.setColour(fit == gain_staging::Fit::InTarget ? KhensuTheme::textPrimary : fitColour(fit));
        g.fillRoundedRectangle(juce::Rectangle<float>(3.0f, bar.getHeight() + 8.0f).withCentre({x, bar.getCentreY()}),
                               1.5f);
    }
    // VIS-1: the bar's scale - the ChartKit ticks and labels, the unit on the last.
    if (a.getHeight() >= 11.0f) {
        const auto axis = gaugeAxis(gauge);
        const auto style = desktopChartStyle();
        g.setFont(ui::Palette::uiFont(style.fontSize * 0.85f));
        const auto ticks = axis.ticks(std::max(2, static_cast<int>(bar.getWidth() / 44.0f)));
        auto labels = a.removeFromTop(12.0f);
        for (size_t i = 0; i < ticks.size(); ++i) {
            const float x = bar.getX() + bar.getWidth() * static_cast<float>(axis.norm(ticks[i]));
            g.setColour(style.axis);
            g.drawVerticalLine(juce::roundToInt(x), bar.getBottom(), bar.getBottom() + 3.0f);
            const bool last = i + 1 == ticks.size();
            const auto text = last ? axis.label(ticks[i]) + " " + gauge.unit : axis.label(ticks[i]);
            const float w = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), text) + 4.0f;
            auto r = juce::Rectangle<float>(x - w * 0.5f, labels.getY() + 2.0f, w, 10.0f);
            if (r.getRight() > area.toFloat().getRight() - 2.0f) r.setX(area.toFloat().getRight() - 2.0f - w);
            if (r.getX() < area.toFloat().getX() + 2.0f) r.setX(area.toFloat().getX() + 2.0f);
            g.setColour(style.muted);
            g.drawText(text, r, juce::Justification::centred, false);
        }
    }
}

void GainStagingPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    for (size_t i = 0; i < gauges_.size(); ++i)
        if (!gaugeAreas_[i].isEmpty()) paintGauge(g, gauges_[i], gaugeAreas_[i]);
    if (!hasData_ || statusArea_.isEmpty()) return;
    paintBody(g);
    // VIS-1: the ChartKit readout for the gauge under the pointer.
    if (hoveredGauge_ >= 0) {
        const auto& gauge = gauges_[static_cast<size_t>(hoveredGauge_)];
        ui::chart::Readout r;
        r.title = gauge.label;
        if (gauge.known) {
            const auto fit = gain_staging::fit(gauge.value, gauge.lo, gauge.hi);
            r.lines.push_back({juce::String(gauge.value, 1) + " " + gauge.unit + " - " + fitText(gauge),
                               fit == gain_staging::Fit::InTarget ? KhensuTheme::vizTrack : fitColour(fit)});
        } else {
            r.lines.push_back({TRANS("Not measured on this file"), KhensuTheme::textTertiary});
        }
        r.lines.push_back({TRANS("Target") + " " + juce::String(gauge.lo, 1) + " " + TRANS("to") + " " +
                               juce::String(gauge.hi, 1) + " " + gauge.unit,
                           KhensuTheme::vizSpreadInner.withAlpha(1.0f)});
        ui::chart::ChartView::paintReadout(g, r, hoverPos_, getLocalBounds().toFloat(), desktopChartStyle());
    }
}

void GainStagingPanel::paintBody(juce::Graphics& g)
{

    // Status lines: neutral unless something is out of target.
    auto s = statusArea_.toFloat();
    g.setFont(KhensuTheme::bodyFont());
    g.setColour(playbackFit_ == gain_staging::Fit::InTarget ? KhensuTheme::textSecondary : fitColour(playbackFit_));
    g.drawText(playbackLine_, s.removeFromTop(20.0f), juce::Justification::centredLeft, true);
    g.setColour(dynamicsFit_ == gain_staging::Fit::InTarget ? KhensuTheme::textSecondary : fitColour(dynamicsFit_));
    g.drawText(dynamicsLine_, s.removeFromTop(20.0f), juce::Justification::centredLeft, true);

    // Spectral balance: three neutral shades, labelled.
    auto sa = specBalanceArea_.toFloat();
    g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText(TRANS("Spectral balance"), sa.removeFromTop(16.0f), juce::Justification::centredLeft, false);
    const float total = juce::jmax(0.001f, balanceLow_ + balanceMid_ + balanceHigh_);
    const float shares[3] = {balanceLow_ / total, balanceMid_ / total, balanceHigh_ / total};
    const juce::String names[3] = {TRANS("Low"), TRANS("Mid"), TRANS("High")};
    const float alphas[3] = {0.34f, 0.22f, 0.12f};
    auto bar = sa.removeFromTop(20.0f);
    float x = bar.getX();
    for (int i = 0; i < 3; ++i) {
        const float w = bar.getWidth() * shares[i];
        const auto seg = juce::Rectangle<float>(x, bar.getY(), w, bar.getHeight());
        g.setColour(KhensuTheme::textPrimary.withAlpha(alphas[i]));
        g.fillRect(seg.reduced(i == 0 ? 0.0f : 0.5f, 0.0f));
        if (w > 64.0f) {
            g.setColour(KhensuTheme::textPrimary);
            g.setFont(KhensuTheme::numericFont(KhensuTheme::kFontLabel));
            g.drawText(names[i] + " " + juce::String(juce::roundToInt(shares[i] * 100.0f)) + "%", seg.reduced(6.0f, 0.0f),
                       juce::Justification::centredLeft, false);
        }
        x += w;
    }

    // Stereo width: a neutral bar; colour only for a real problem.
    auto st = stereoBarArea_.toFloat();
    g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
    g.setColour(KhensuTheme::textTertiary);
    auto widthHeader = st.removeFromTop(16.0f);
    g.drawText(TRANS("Stereo width"), widthHeader, juce::Justification::centredLeft, false);
    g.setColour(KhensuTheme::textPrimary);
    g.setFont(KhensuTheme::numericFont(KhensuTheme::kFontLabel));
    g.drawText(juce::String(juce::roundToInt(stereoWidth_ * 100.0f)) + "%  " + TRANS("corr") + " " +
                   juce::String(stereoCorr_, 2),
               widthHeader, juce::Justification::centredRight, false);
    auto track = st.removeFromTop(20.0f).withSizeKeepingCentre(st.getWidth(), 6.0f);
    g.setColour(KhensuTheme::textPrimary.withAlpha(0.08f));
    g.fillRoundedRectangle(track, 3.0f);
    const auto widthFit = stereoCorr_ < -0.2f ? gain_staging::Fit::Far
                          : stereoWidth_ < 0.2f ? gain_staging::Fit::Near
                                                : gain_staging::Fit::InTarget;
    g.setColour(widthFit == gain_staging::Fit::InTarget ? KhensuTheme::textPrimary.withAlpha(0.45f) : fitColour(widthFit));
    g.fillRoundedRectangle(track.withWidth(track.getWidth() * juce::jlimit(0.0f, 1.0f, stereoWidth_)), 3.0f);
}

} // namespace khensu
