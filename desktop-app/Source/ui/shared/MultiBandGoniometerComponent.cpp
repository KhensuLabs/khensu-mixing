// desktop-app/Source/ui/shared/MultiBandGoniometerComponent.cpp
#include "MultiBandGoniometerComponent.h"
#include "KhensuLookAndFeel.h"
#include "KhensuTheme.h"
#include <algorithm>
#include <cmath>

namespace khensu {

void MultiBandGoniometerComponent::setAnalysisData(const AnalysisResult& r)
{
    hasData_ = r.has_fast_analysis();
    if (!hasData_) {
        points_.clear();
        repaint();
        return;
    }

    // 1. Decimate/decay existing points
    for (auto& p : points_)
        p.alpha -= 0.12f;

    points_.erase(
        std::remove_if(points_.begin(), points_.end(), [](const GoniometerPoint& p) { return p.alpha <= 0.0f; }),
        points_.end());

    // 2. Fetch or simulate band widths and correlations
    if (r.mono_compat) {
        const auto& mc = *r.mono_compat;

        // Low: average of sub and bass
        float subCorr = mc.per_band_correlation.count("sub") ? mc.per_band_correlation.at("sub") : 0.98f;
        float bassCorr = mc.per_band_correlation.count("bass") ? mc.per_band_correlation.at("bass") : 0.92f;
        lowCorr_ = (subCorr + bassCorr) * 0.5f;
        // Low width is derived from low_end_stereo_pct
        lowWidth_ = juce::jlimit(0.01f, 0.40f, mc.low_end_stereo_pct / 100.0f);

        // Mid: midrange
        midCorr_ = mc.per_band_correlation.count("midrange") ? mc.per_band_correlation.at("midrange") : 0.70f;
        midWidth_ = r.stereo.width * 0.8f;

        // High: air
        highCorr_ = mc.per_band_correlation.count("air") ? mc.per_band_correlation.at("air") : 0.40f;
        highWidth_ = juce::jlimit(0.40f, 1.20f, r.stereo.width * 1.3f);
    } else {
        // Fallback to global values with frequency-slope scaling
        float gc = r.stereo.correlation;
        float gw = r.stereo.width;

        lowCorr_ = juce::jlimit(0.85f, 1.0f, gc + 0.2f);
        lowWidth_ = juce::jlimit(0.01f, 0.2f, gw * 0.3f);

        midCorr_ = gc;
        midWidth_ = gw;

        highCorr_ = juce::jlimit(-0.2f, 0.6f, gc - 0.2f);
        highWidth_ = juce::jlimit(0.4f, 1.5f, gw * 1.4f);
    }

    generateScatter();
    repaint();
}

void MultiBandGoniometerComponent::updateRealTime(float globalWidth, float globalCorr)
{
    hasData_ = true;

    // 1. Decimate/decay existing points
    for (auto& p : points_)
        p.alpha -= 0.12f;

    points_.erase(
        std::remove_if(points_.begin(), points_.end(), [](const GoniometerPoint& p) { return p.alpha <= 0.0f; }),
        points_.end());

    // 2. Derive simulated bands from live global values
    float gc = globalCorr;
    float gw = globalWidth;

    lowCorr_ = juce::jlimit(0.85f, 1.0f, gc + 0.2f);
    lowWidth_ = juce::jlimit(0.01f, 0.2f, gw * 0.3f);

    midCorr_ = gc;
    midWidth_ = gw;

    highCorr_ = juce::jlimit(-0.2f, 0.6f, gc - 0.2f);
    highWidth_ = juce::jlimit(0.4f, 1.5f, gw * 1.4f);

    generateScatter();
    repaint();
}

void MultiBandGoniometerComponent::generateScatter()
{
    juce::Random rand;

    auto addBandPoints = [&](int bandIdx, float width, float correlation, float scaleMultiplier) {
        float wVal = juce::jlimit(0.02f, 1.5f, width);
        float cVal = juce::jlimit(-1.0f, 1.0f, correlation);

        // Mid (sum) vs Side (difference) gain coefficients
        float midG = std::sqrt(juce::jmax(0.0f, 1.0f + cVal)) * 0.707f;
        float sideG = std::sqrt(juce::jmax(0.0f, 1.0f - cVal)) * wVal * 0.707f;

        // Generate 45 points per band update
        for (int i = 0; i < 45; ++i) {
            // Gaussian-ish distribution centered around 0
            float mid = (rand.nextFloat() + rand.nextFloat() + rand.nextFloat() - 1.5f) * 0.5f * midG;
            float side = (rand.nextFloat() + rand.nextFloat() + rand.nextFloat() - 1.5f) * 0.5f * sideG;

            float left = (mid + side);
            float right = (mid - side);

            // Polar conversion (45 degree rotation)
            float px = (left - right) * 0.7071f;
            float py = -(left + right) * 0.7071f;

            // Apply scale multiplier for concentric ring positioning
            points_.push_back({px * scaleMultiplier, py * scaleMultiplier, rand.nextFloat() * 0.3f + 0.7f, bandIdx});
        }
    };

    // Inner ring (Low): 0.28 scale multiplier
    addBandPoints(0, lowWidth_, lowCorr_, 0.28f);
    // Middle ring (Mid): 0.62 scale multiplier
    addBandPoints(1, midWidth_, midCorr_, 0.62f);
    // Outer ring (High): 0.92 scale multiplier
    addBandPoints(2, highWidth_, highCorr_, 0.92f);
}

void MultiBandGoniometerComponent::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced(4.0f);
    float cx = bounds.getCentreX();
    float cy = bounds.getCentreY();
    float maxRadius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.45f;

    // Paint background plate
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.40f));
    g.fillEllipse(cx - maxRadius, cy - maxRadius, maxRadius * 2.0f, maxRadius * 2.0f);

    if (!hasData_) {
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(khensu::KhensuTheme::uiFont(11.0f, juce::Font::plain));
        g.drawText("Awaiting track data...", bounds, juce::Justification::centred, false);
        return;
    }

    // Concentric grid rings
    g.setColour(KhensuTheme::textSecondary.withAlpha(0.08f));
    g.drawEllipse(cx - maxRadius * 0.92f, cy - maxRadius * 0.92f, maxRadius * 1.84f, maxRadius * 1.84f,
                  1.0f); // High Ring
    g.drawEllipse(cx - maxRadius * 0.62f, cy - maxRadius * 0.62f, maxRadius * 1.24f, maxRadius * 1.24f,
                  1.0f); // Mid Ring
    g.drawEllipse(cx - maxRadius * 0.28f, cy - maxRadius * 0.28f, maxRadius * 0.56f, maxRadius * 0.56f,
                  1.0f); // Low Ring

    // 45 degree axis lines
    constexpr float s45 = 0.7071f;
    g.setColour(KhensuTheme::textSecondary.withAlpha(0.12f));
    g.drawLine(cx, cy, cx - maxRadius * s45, cy - maxRadius * s45, 1.0f);
    g.drawLine(cx, cy, cx + maxRadius * s45, cy - maxRadius * s45, 1.0f);
    g.drawVerticalLine(juce::roundToInt(cx), cy - maxRadius, cy);

    // Render multi-band particle clouds
    for (const auto& p : points_) {
        float px = cx + p.x * maxRadius;
        float py = cy + p.y * maxRadius;

        // Color coding: Low = Teal, Mid = Blue, High = Amber
        juce::Colour colour = (p.bandIndex == 0)   ? KhensuTheme::accentTeal
                              : (p.bandIndex == 1) ? KhensuTheme::accentBlue
                                                   : KhensuTheme::accentAmber;

        g.setColour(colour.withAlpha(p.alpha * 0.55f));
        g.fillEllipse(px - 1.2f, py - 1.2f, 2.4f, 2.4f);

        if (p.alpha > 0.45f) {
            g.setColour(khensu::KhensuTheme::textPrimary.withAlpha(p.alpha * 0.35f));
            g.fillEllipse(px - 0.6f, py - 0.6f, 1.2f, 1.2f);
        }
    }

    // Legend labels
    g.setFont(KhensuTheme::labelFont(8.0f));
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.7f));
    g.drawText("SUB/BASS", (int)cx - 30, (int)(cy - maxRadius * 0.28f + 2.0f), 60, 10, juce::Justification::centred);

    g.setColour(KhensuTheme::accentBlue.withAlpha(0.7f));
    g.drawText("MID / VOCAL", (int)cx - 40, (int)(cy - maxRadius * 0.62f + 2.0f), 80, 10, juce::Justification::centred);

    g.setColour(KhensuTheme::accentAmber.withAlpha(0.7f));
    g.drawText("HIGH / AIR", (int)cx - 40, (int)(cy - maxRadius * 0.92f + 2.0f), 80, 10, juce::Justification::centred);
}

} // namespace khensu
