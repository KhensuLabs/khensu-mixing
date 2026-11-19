// desktop-app/Source/ui/shared/SpectrumComponent.cpp
#include "SpectrumComponent.h"
#include "KhensuLookAndFeel.h"
#include <cmath>

namespace khensu {

void SpectrumComponent::setBands(const std::vector<float>& b)
{
    bands_ = b;
    repaint();
}
void SpectrumComponent::setResonancePins(std::vector<SpectrumPin> p)
{
    pins_ = std::move(p);
    repaint();
}
void SpectrumComponent::setGenreReference(float c, float l)
{
    genreCentroid_ = c;
    genreLowEnd_ = l;
    repaint();
}
void SpectrumComponent::setEqSuggestions(std::vector<EqPoint> pts)
{
    eqPoints_ = std::move(pts);
    repaint();
}

void SpectrumComponent::paint(juce::Graphics& g)
{
    const int n = (int)bands_.size();
    if (n == 0) return;
    static const char* labels[10] = {"31", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"};

    auto b = getLocalBounds().reduced(4);
    auto labelRow = b.removeFromBottom(14);
    const float minDb = -60.0f, maxDb = 0.0f;
    float bw = (float)b.getWidth() / (float)n;

    // Low-end genre tint (first 3 bars) - drawn before bars so bars overlay it
    if (genreLowEnd_ > 0.0f && n >= 3) {
        for (int i = 0; i < 3 && i < n; ++i) {
            g.setColour(KhensuTheme::accentTeal.withAlpha(genreLowEnd_ * 0.15f));
            g.fillRect(b.getX() + i * bw + 1.0f, (float)b.getY(), bw - 2.0f, (float)b.getHeight());
        }
    }

    // Bars
    for (int i = 0; i < n; ++i) {
        float db = juce::jlimit(minDb, maxDb, bands_[(size_t)i]);
        float h = (db - minDb) / (maxDb - minDb) * b.getHeight();
        float hue = (n > 1) ? juce::jmap((float)i, 0.0f, (float)(n - 1), 0.02f, 0.62f) : 0.32f;
        g.setColour(juce::Colour::fromHSV(hue, 0.55f, 0.9f, 1.0f)); // theme-exempt(frequency rainbow: hue encodes the band position, identical in every theme)
        g.fillRect(b.getX() + i * bw + 1.0f, b.getBottom() - h, bw - 2.0f, h);
        if (i < 10) {
            g.setColour(KhensuTheme::textSecondary);
            g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::plain));
            g.drawText(labels[i], (int)(b.getX() + i * bw), labelRow.getY(), (int)bw, 12, juce::Justification::centred,
                       false);
        }
    }

    // Genre centroid needle - vertical line at the band position for the centroid frequency
    // Band index = log2(hz / 31.5): 31.5Hz->0, 63Hz->1, ..., 16kHz->9
    if (genreCentroid_ > 0.0f) {
        float xFrac = juce::jlimit(0.0f, 1.0f, log2f(genreCentroid_ / 31.5f) / 10.0f);
        int needleX = b.getX() + (int)(xFrac * (float)b.getWidth());
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.55f));
        g.drawLine((float)needleX, (float)b.getY(), (float)needleX, (float)b.getBottom(), 1.5f);

        // Dashed reference target curve
        juce::Path refPath;
        for (int i = 0; i < n; ++i) {
            float hz = 31.5f * std::pow(2.0f, (float)i);
            float baseDb = -18.0f - 3.0f * std::log2(hz / 1000.0f);
            if (hz < 250.0f) {
                float lowBoost = (genreLowEnd_ - 0.5f) * 12.0f;
                baseDb += lowBoost * (1.0f - hz / 250.0f);
            }
            float centroidOctaves = std::log2(genreCentroid_ / 1000.0f);
            float bandOctaves = std::log2(hz / 1000.0f);
            baseDb += (bandOctaves - centroidOctaves) * -2.0f;
            float targetDb = juce::jlimit(minDb, maxDb, baseDb);

            float ty = b.getBottom() - (targetDb - minDb) / (maxDb - minDb) * b.getHeight();
            float tx = b.getX() + i * bw + bw * 0.5f;
            if (i == 0)
                refPath.startNewSubPath(tx, ty);
            else
                refPath.lineTo(tx, ty);
        }
        juce::Path dashedPath;
        float dashLengths[] = {4.0f, 3.0f};
        juce::PathStrokeType(1.5f).createDashedStroke(dashedPath, refPath, dashLengths, 2);
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.5f));
        g.fillPath(dashedPath); // createDashedStroke() yields a filled outline
    }

    // Resonance pins - downward triangles above bar tops
    for (const auto& pin : pins_) {
        int bandIdx = (int)juce::jlimit(0.0f, (float)(n - 1), std::log2(juce::jmax(1.0f, pin.hz) / 31.5f));
        float db = juce::jlimit(minDb, maxDb, bands_[(size_t)bandIdx]);
        float barH = (db - minDb) / (maxDb - minDb) * b.getHeight();
        float px = b.getX() + bandIdx * bw + bw * 0.5f;
        float py = juce::jmax((float)(b.getY() + 2), b.getBottom() - barH - 5.0f);

        juce::Path tri;
        tri.addTriangle(px - 4.0f, py, px + 4.0f, py, px, py + 5.0f);
        g.setColour(KhensuTheme::accentAmber);
        g.fillPath(tri);

        if (pin.prominence > 6.0f) {
            g.setColour(KhensuTheme::textSecondary);
            g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::plain));
            g.drawText(juce::String((int)pin.hz) + "Hz", (int)(px - 20), (int)(py - 12), 40, 11,
                       juce::Justification::centred, false);
        }
    }

    // --- EQ suggestion overlay ---
    // Draws parametric bell curves for each recommendation that has EQ parameters.
    // Curves are colour-coded by severity: red = critical, amber = warning, green = informational.
    if (!eqPoints_.empty()) {
        const float bW = (float)b.getWidth();
        const float bH = (float)b.getHeight();
        // Map 0 dB at mid-height; ±12 dB spans ±35% of bar height.
        const float gainScale = bH * 0.35f / 12.0f;

        for (const auto& pt : eqPoints_) {
            if (std::abs(pt.gain_db) < 0.1f) continue;
            const juce::Colour ptCol = pt.severity > 60.0f   ? KhensuTheme::accentRed.withAlpha(0.70f)
                                       : pt.severity > 30.0f ? KhensuTheme::accentAmber.withAlpha(0.70f)
                                                             : KhensuTheme::accentTeal.withAlpha(0.55f);

            // Convert freq to band fraction (log scale 31Hz-16kHz)
            const auto freqToX = [&](float hz) -> float {
                return juce::jlimit(0.0f, bW,
                                    bW * juce::jlimit(0.0f, 1.0f, std::log2(juce::jmax(1.0f, hz) / 31.5f) / 10.0f));
            };
            const float centerX = (float)b.getX() + freqToX(pt.freq_hz);

            // Draw parametric EQ bell: magnitude = gain / (1 + (log2(f/fc) * Q)^2)
            juce::Path eqCurve;
            bool first = true;
            const int nSteps = 200;
            for (int step = 0; step <= nSteps; ++step) {
                const float hz = 31.5f * std::pow(2.0f, 10.0f * (float)step / (float)nSteps);
                const float logRatio = std::log2(hz / pt.freq_hz);
                const float bellGain = pt.gain_db / (1.0f + (logRatio * pt.q) * (logRatio * pt.q));
                const float px = (float)b.getX() + freqToX(hz);
                // 0 dB maps to mid-height of bar area (not 0 dB display level)
                const float py = (float)b.getCentreY() - bellGain * gainScale;
                if (first) {
                    eqCurve.startNewSubPath(px, py);
                    first = false;
                } else
                    eqCurve.lineTo(px, py);
            }
            g.setColour(ptCol);
            g.strokePath(eqCurve, juce::PathStrokeType(1.8f));

            // Vertical marker at the centre frequency
            g.setColour(ptCol.withAlpha(0.30f));
            g.drawVerticalLine(juce::roundToInt(centerX), (float)b.getY(), (float)b.getBottom());

            // Gain label at the peak
            g.setColour(ptCol);
            g.setFont(khensu::KhensuTheme::uiFont(8.5f, juce::Font::bold));
            juce::String lbl = (pt.gain_db >= 0.0f ? "+" : "") + juce::String(pt.gain_db, 1) + "dB";
            g.drawText(lbl, (int)centerX - 20, b.getY() + 2, 40, 10, juce::Justification::centred, false);
        }
    }
}

} // namespace khensu
