// desktop-app/Source/ui/shared/HeatSpectrumComponent.cpp
#include "HeatSpectrumComponent.h"
#include "KhensuEmptyState.h"
#include "KhensuTheme.h"
#include <numeric>
#include <algorithm>
#include <cmath>

namespace khensu {

void HeatSpectrumComponent::setBands(const std::vector<float>& dbPerOctave)
{
    bands_ = dbPerOctave;
    ranks_ = computeRanks(bands_);
    repaint();
}

void HeatSpectrumComponent::setResonancePins(std::vector<SpectrumPin> pins)
{
    pins_ = std::move(pins);
    repaint();
}

void HeatSpectrumComponent::setGenreReference(float centroid_hz, float low_end_weight)
{
    genreCentroid_ = centroid_hz;
    genreLowEnd_ = low_end_weight;
    repaint();
}

void HeatSpectrumComponent::setTargetTilt(float dbPerOctave, bool show)
{
    targetTiltDbPerOct_ = dbPerOctave;
    showTargetTilt_ = show;
    repaint();
}

void HeatSpectrumComponent::setSampleRate(double sampleRate)
{
    if (sampleRate <= 0.0 || juce::approximatelyEqual(sampleRate, sampleRate_)) return;
    sampleRate_ = sampleRate;
    repaint();
}

void HeatSpectrumComponent::setMidSideBands(const std::vector<float>& midBands, const std::vector<float>& sideBands)
{
    midBands_ = midBands;
    sideBands_ = sideBands;
    repaint();
}

juce::Colour HeatSpectrumComponent::colourForRank(float rank)
{
    struct Stop {
        float pos;
        juce::Colour col;
    };
    static const Stop stops[] = {
        {0.0f, juce::Colour(0xff2255aa)},  {0.25f, juce::Colour(0xff00d4aa)}, {0.50f, juce::Colour(0xfff5c842)}, // theme-exempt(data-viz ramp/palette: perceptual scale, deliberately theme-independent)
        {0.75f, juce::Colour(0xffff8c00)}, {1.0f, juce::Colour(0xffff3030)}, // theme-exempt(data-viz ramp/palette: perceptual scale, deliberately theme-independent)
    };
    rank = juce::jlimit(0.0f, 1.0f, rank);
    for (int i = 0; i < 4; ++i) {
        if (rank <= stops[i + 1].pos) {
            float t = (rank - stops[i].pos) / (stops[i + 1].pos - stops[i].pos);
            return stops[i].col.interpolatedWith(stops[i + 1].col, t);
        }
    }
    return stops[4].col;
}

std::vector<float> HeatSpectrumComponent::computeRanks(const std::vector<float>& bands)
{
    if (bands.empty()) return {};
    float mn = *std::min_element(bands.begin(), bands.end());
    float mx = *std::max_element(bands.begin(), bands.end());
    float range = mx - mn;
    std::vector<float> ranks(bands.size());
    for (size_t i = 0; i < bands.size(); ++i)
        ranks[i] = (range > 0.0f) ? (bands[i] - mn) / range : 0.5f;
    return ranks;
}

// Map octave-band index (0..N-1) to its centre frequency Hz.
// Standard octave-band centres starting at 31 Hz (ISO 266).
static float octaveCentreHz(int bandIndex)
{
    static const float kCentres[] = {31.5f,   63.0f,   125.0f,  250.0f,  500.0f,
                                     1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f};
    if (bandIndex < 0 || bandIndex >= (int)(sizeof(kCentres) / sizeof(*kCentres))) return 0.0f;
    return kCentres[bandIndex];
}

// Convert frequency to normalised x position on a log10 scale from 20 Hz to 20 kHz.
static float logX(float hz, float left, float width)
{
    const float lo = std::log10(20.0f);
    const float hi = std::log10(20000.0f);
    return left + (std::log10(juce::jmax(hz, 1.0f)) - lo) / (hi - lo) * width;
}

void HeatSpectrumComponent::paint(juce::Graphics& g)
{
    // ── Layout ─────────────────────────────────────────────────────────────
    const int kHzLabelH = 16;
    const int kDbLabelW = 28;
    auto fullBounds = getLocalBounds();
    auto hzLabelRow = fullBounds.removeFromBottom(kHzLabelH);
    auto dbLabelCol = fullBounds.removeFromLeft(kDbLabelW);
    auto chartArea = fullBounds.toFloat().reduced(2.0f, 4.0f);

    if (bands_.empty()) {
        KhensuEmptyState::paintInto(g, fullBounds, TRANS("No spectral data"), {}); // UXR-6
        return;
    }

    const float cLeft = chartArea.getX();
    const float cTop = chartArea.getY();
    const float cW = chartArea.getWidth();
    const float cH = chartArea.getHeight();
    const float cBot = chartArea.getBottom();

    // ── dB grid lines (0, -20, -40, -60 dBFS) ─────────────────────────────
    g.setFont(KhensuTheme::labelFont(type::caption));
    const float kDbMin = -60.0f, kDbMax = 0.0f;
    static const float kDbGrid[] = {0.0f, -20.0f, -40.0f, -60.0f};
    for (float db : kDbGrid) {
        float normY = (db - kDbMax) / (kDbMin - kDbMax); // 0=top, 1=bottom
        float y = cTop + normY * cH;
        g.setColour(KhensuTheme::borderSubtle.withAlpha(0.4f));
        g.drawHorizontalLine(juce::roundToInt(y), cLeft, cLeft + cW);
        // dB label on left margin
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(juce::String((int)db) + " dB", dbLabelCol.getX(), (int)y - 6, kDbLabelW - 2, 12,
                   juce::Justification::centredRight, false);
    }

    // ── Gradient bars ──────────────────────────────────────────────────────
    const int n = (int)bands_.size();
    if (n < 100) {
        const float barW = cW / (float)n;
        for (int i = 0; i < n; ++i) {
            const float rank = (i < (int)ranks_.size()) ? ranks_[i] : 0.5f;
            const float normH = juce::jlimit(0.02f, 1.0f, (bands_[i] - kDbMin) / (kDbMax - kDbMin));
            const float bh = cH * normH;
            const float bx = cLeft + (float)i * barW;
            const float by = cBot - bh;
            const juce::Colour col = colourForRank(rank);

            // Gradient: bright/lighter at top, full colour at bottom - depth illusion
            juce::ColourGradient grad(col.brighter(0.5f), bx, by, col, bx, cBot, false);
            grad.addColour(0.7, col);
            g.setGradientFill(grad);
            // Below ~1.5px per band the inset makes these widths NEGATIVE
            // (kimi audit P1 #32); clamp so a very narrow plot degrades to
            // hairline bars instead of drawing inverted rectangles.
            g.fillRoundedRectangle(bx + 0.5f, by, juce::jmax(0.5f, barW - 1.5f), bh, 2.0f);

            // Specular highlight strip at the top of each bar - 3D gloss effect
            g.setColour(KhensuTheme::highlight.withMultipliedAlpha(3.0f));
            g.fillRoundedRectangle(bx + 1.5f, by, juce::jmax(0.5f, (barW - 3.0f) * 0.6f), 3.0f, 1.5f);
        }

        // ── Smooth curve connecting bar tops (cubic catmull-rom spline) ────────
        if (n >= 2) {
            // Gather bar-top points in log-Hz x-coordinate space
            std::vector<juce::Point<float>> pts;
            pts.reserve(n);
            for (int i = 0; i < n; ++i) {
                float hz = octaveCentreHz(i);
                float px = (hz > 0.0f) ? logX(hz, cLeft, cW) : cLeft + ((float)i + 0.5f) * barW;
                float normH = juce::jlimit(0.02f, 1.0f, (bands_[i] - kDbMin) / (kDbMax - kDbMin));
                float py = cBot - cH * normH;
                pts.push_back({px, py});
            }

            // Build a smooth path through the points using cubic Bezier segments
            juce::Path curve;
            curve.startNewSubPath(pts[0]);
            for (int i = 0; i < n - 1; ++i) {
                // Control points for catmull-rom -> bezier conversion
                juce::Point<float> p0 = (i > 0) ? pts[i - 1] : pts[i];
                juce::Point<float> p1 = pts[i];
                juce::Point<float> p2 = pts[i + 1];
                juce::Point<float> p3 = (i + 2 < n) ? pts[i + 2] : pts[i + 1];

                float cp1x = p1.x + (p2.x - p0.x) / 6.0f;
                float cp1y = p1.y + (p2.y - p0.y) / 6.0f;
                float cp2x = p2.x - (p3.x - p1.x) / 6.0f;
                float cp2y = p2.y - (p3.y - p1.y) / 6.0f;
                curve.cubicTo(cp1x, cp1y, cp2x, cp2y, p2.x, p2.y);
            }

            // Filled area under curve (teal tint with alpha)
            juce::Path fill = curve;
            fill.lineTo(pts.back().x, cBot);
            fill.lineTo(pts.front().x, cBot);
            fill.closeSubPath();
            g.setColour(KhensuTheme::accentTeal.withAlpha(0.08f));
            g.fillPath(fill);

            // Stroke the curve - glow effect (wide soft + narrow bright)
            g.setColour(KhensuTheme::accentTeal.withAlpha(0.25f));
            g.strokePath(curve,
                         juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour(KhensuTheme::accentTeal.withAlpha(0.85f));
            g.strokePath(curve,
                         juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            // ── Genre spectral-tilt target line ──
            // A straight slope in dB/octave anchored to the measured bands'
            // mean level, so it compares shape, not loudness. Dashed amber to
            // stand apart from the teal measured curve.
            if (showTargetTilt_) {
                float mean = 0.0f;
                for (int i = 0; i < n; ++i)
                    mean += bands_[i];
                mean /= (float)n;
                const float centreIdx = (float)(n - 1) * 0.5f;

                juce::Path tiltLine;
                bool started = false;
                for (int i = 0; i < n; ++i) {
                    const float db = mean + targetTiltDbPerOct_ * ((float)i - centreIdx);
                    const float normH = juce::jlimit(0.02f, 1.0f, (db - kDbMin) / (kDbMax - kDbMin));
                    const float px = pts[(size_t)i].x;
                    const float py = cBot - cH * normH;
                    if (!started) {
                        tiltLine.startNewSubPath(px, py);
                        started = true;
                    } else
                        tiltLine.lineTo(px, py);
                }
                juce::Path dashed;
                const float dashes[] = {5.0f, 4.0f};
                juce::PathStrokeType(1.2f).createDashedStroke(dashed, tiltLine, dashes, 2);
                g.setColour(KhensuTheme::vizMedian); // VIS-4: the genre's target, not a severity
                g.fillPath(dashed);

                g.setFont(KhensuTheme::labelFont(type::caption));
                g.setColour(KhensuTheme::textSecondary);
                g.drawText("genre tilt " + juce::String(targetTiltDbPerOct_, 1) + " dB/oct", (int)(cLeft + cW) - 120,
                           (int)cTop, 118, 12, juce::Justification::topRight, false);
            }

            // ── Render Mid-Side curves ──
            auto drawCurve = [&](const std::vector<float>& targetBands, juce::Colour colour, float alpha) {
                const int numB = (int)targetBands.size();
                if (numB < 2) return;
                std::vector<juce::Point<float>> ptsMS;
                ptsMS.reserve(numB);
                for (int idx = 0; idx < numB; ++idx) {
                    float hz = octaveCentreHz(idx);
                    float px = (hz > 0.0f) ? logX(hz, cLeft, cW) : cLeft + ((float)idx + 0.5f) * barW;
                    float normH = juce::jlimit(0.02f, 1.0f, (targetBands[idx] - kDbMin) / (kDbMax - kDbMin));
                    float py = cBot - cH * normH;
                    ptsMS.push_back({px, py});
                }
                juce::Path curveMS;
                curveMS.startNewSubPath(ptsMS[0]);
                for (int idx = 0; idx < numB - 1; ++idx) {
                    juce::Point<float> p0 = (idx > 0) ? ptsMS[idx - 1] : ptsMS[idx];
                    juce::Point<float> p1 = ptsMS[idx];
                    juce::Point<float> p2 = ptsMS[idx + 1];
                    juce::Point<float> p3 = (idx + 2 < numB) ? ptsMS[idx + 2] : ptsMS[idx + 1];

                    float cp1x = p1.x + (p2.x - p0.x) / 6.0f;
                    float cp1y = p1.y + (p2.y - p0.y) / 6.0f;
                    float cp2x = p2.x - (p3.x - p1.x) / 6.0f;
                    float cp2y = p2.y - (p3.y - p1.y) / 6.0f;
                    curveMS.cubicTo(cp1x, cp1y, cp2x, cp2y, p2.x, p2.y);
                }
                g.setColour(colour.withAlpha(alpha * 0.3f));
                g.strokePath(curveMS,
                             juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                g.setColour(colour.withAlpha(alpha));
                g.strokePath(curveMS,
                             juce::PathStrokeType(1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            };

            if (midBands_.size() == bands_.size()) {
                drawCurve(midBands_, KhensuTheme::vizCategory(1), 0.85f); // VIS-4: series colours
            }
            if (sideBands_.size() == bands_.size()) {
                drawCurve(sideBands_, KhensuTheme::vizCategory(2), 0.85f);
            }

            // Draw small legend overlay for Mid/Side curves
            g.setFont(KhensuTheme::labelFont(type::caption));
            const int legY = (int)cTop + 14;
            const int legW = 65;

            g.setColour(KhensuTheme::vizCategory(1));
            g.drawHorizontalLine(legY + 5, cLeft + cW - 145, cLeft + cW - 135);
            g.setColour(KhensuTheme::textSecondary);
            g.drawText("Mid (Mono)", (int)cLeft + (int)cW - 130, legY, legW, 10, juce::Justification::centredLeft,
                       false);

            g.setColour(KhensuTheme::vizCategory(2));
            g.drawHorizontalLine(legY + 5, cLeft + cW - 75, cLeft + cW - 65);
            g.setColour(KhensuTheme::textSecondary);
            g.drawText("Side (Stereo)", (int)cLeft + (int)cW - 60, legY, legW, 10, juce::Justification::centredLeft,
                       false);
        }
    } else {
        const float binHz = (float)sampleRate_ / (float)(n * 2);
        auto buildFftPath = [&](const std::vector<float>& targetBands) -> juce::Path {
            juce::Path path;
            bool first = true;
            for (int i = 0; i < (int)targetBands.size(); ++i) {
                float hz = static_cast<float>(i) * binHz;
                if (hz < 20.0f) continue;
                if (hz > 20000.0f) break;
                float px = logX(hz, cLeft, cW);
                float normH = juce::jlimit(0.0f, 1.0f, (targetBands[i] - kDbMin) / (kDbMax - kDbMin));
                float py = cBot - cH * normH;
                if (first) {
                    path.startNewSubPath(px, py);
                    first = false;
                } else
                    path.lineTo(px, py);
            }
            return path;
        };

        juce::Path mainPath = buildFftPath(bands_);
        if (!mainPath.isEmpty()) {
            juce::Path fill = mainPath;
            fill.lineTo(fill.getCurrentPosition().x, cBot);
            fill.lineTo(logX(20.0f, cLeft, cW), cBot);
            fill.closeSubPath();
            g.setColour(KhensuTheme::accentTeal.withAlpha(0.08f));
            g.fillPath(fill);

            g.setColour(KhensuTheme::accentTeal.withAlpha(0.25f));
            g.strokePath(mainPath, juce::PathStrokeType(3.0f));
            g.setColour(KhensuTheme::accentTeal.withAlpha(0.85f));
            g.strokePath(mainPath, juce::PathStrokeType(1.2f));
        }

        if (midBands_.size() == bands_.size()) {
            juce::Path midPath = buildFftPath(midBands_);
            if (!midPath.isEmpty()) {
                g.setColour(KhensuTheme::vizCategory(1).withAlpha(0.25f));
                g.strokePath(midPath, juce::PathStrokeType(3.0f));
                g.setColour(KhensuTheme::vizCategory(1).withAlpha(0.85f));
                g.strokePath(midPath, juce::PathStrokeType(1.2f));
            }
        }
        if (sideBands_.size() == bands_.size()) {
            juce::Path sidePath = buildFftPath(sideBands_);
            if (!sidePath.isEmpty()) {
                g.setColour(KhensuTheme::vizCategory(2).withAlpha(0.25f));
                g.strokePath(sidePath, juce::PathStrokeType(3.0f));
                g.setColour(KhensuTheme::vizCategory(2).withAlpha(0.85f));
                g.strokePath(sidePath, juce::PathStrokeType(1.2f));
            }
        }

        // Draw small legend overlay for Mid/Side curves
        g.setFont(KhensuTheme::labelFont(type::caption));
        const int legY = (int)cTop + 14;
        const int legW = 65;

        g.setColour(KhensuTheme::vizCategory(1));
        g.drawHorizontalLine(legY + 5, cLeft + cW - 145, cLeft + cW - 135);
        g.setColour(KhensuTheme::textSecondary);
        g.drawText("Mid (Mono)", (int)cLeft + (int)cW - 130, legY, legW, 10, juce::Justification::centredLeft, false);

        g.setColour(KhensuTheme::vizCategory(2));
        g.drawHorizontalLine(legY + 5, cLeft + cW - 75, cLeft + cW - 65);
        g.setColour(KhensuTheme::textSecondary);
        g.drawText("Side (Stereo)", (int)cLeft + (int)cW - 60, legY, legW, 10, juce::Justification::centredLeft, false);
    }

    // ── Genre centroid reference line ──────────────────────────────────────
    if (genreCentroid_ > 0.0f) {
        const float cx = logX(genreCentroid_, cLeft, cW);
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.5f));
        float dashes[] = {4.0f, 3.0f};
        g.drawDashedLine({cx, cTop, cx, cBot}, dashes, 2, 1.0f);
    }

    // ── Resonance pins ─────────────────────────────────────────────────────
    for (int i = 0; i < (int)pins_.size(); ++i) {
        if (pins_[i].hz <= 0.0f) continue;
        const float px = logX(pins_[i].hz, cLeft, cW);
        juce::Path triangle;
        triangle.addTriangle(px - 5.0f, cTop, px + 5.0f, cTop, px, cTop + 10.0f);
        const bool hovered = (i == hoveredPin_);
        g.setColour(hovered ? KhensuTheme::accentAmber : KhensuTheme::accentAmber.withAlpha(0.85f));
        g.fillPath(triangle);
        // Pin label tooltip on hover
        if (hovered) {
            juce::String lbl = juce::String((int)pins_[i].hz) + " Hz";
            g.setFont(KhensuTheme::labelFont(type::caption));
            g.setColour(KhensuTheme::textPrimary);
            g.drawText(lbl, (int)px - 20, (int)cTop + 12, 40, 12, juce::Justification::centred, false);
        }
    }

    // ── Band hover tooltip ─────────────────────────────────────────────────
    if (n < 100 && hoveredBand_ >= 0 && hoveredBand_ < n) {
        const float barW = cW / (float)n;
        const float hz = octaveCentreHz(hoveredBand_);
        const float db = bands_[hoveredBand_];
        const float bxCtr = cLeft + ((float)hoveredBand_ + 0.5f) * barW;
        const float normH = juce::jlimit(0.02f, 1.0f, (db - kDbMin) / (kDbMax - kDbMin));
        const float barTop = cBot - cH * normH;

        juce::String tip = (hz >= 1000.0f) ? juce::String((int)(hz / 1000)) + "k Hz  " + juce::String(db, 1) + " dB"
                                           : juce::String((int)hz) + " Hz  " + juce::String(db, 1) + " dB";
        const int tipW = 96, tipH = 18;
        const int tipX = juce::jlimit((int)cLeft, (int)(cLeft + cW) - tipW, (int)(bxCtr - tipW * 0.5f));
        const int tipY = juce::jmax((int)cTop + 2, (int)barTop - tipH - 4);

        g.setColour(KhensuTheme::bgRaised.withAlpha(0.92f));
        g.fillRoundedRectangle((float)tipX, (float)tipY, (float)tipW, (float)tipH, 3.0f);
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.8f));
        g.drawRoundedRectangle((float)tipX, (float)tipY, (float)tipW, (float)tipH, 3.0f, 0.8f);
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.setColour(KhensuTheme::textPrimary);
        g.drawText(tip, tipX + 5, tipY + 2, tipW - 8, tipH - 4, juce::Justification::centredLeft, false);
    }

    // ── Hz axis labels (log scale) ─────────────────────────────────────────
    struct HzLabel {
        float hz;
        const char* label;
    };
    static const HzLabel kHzLabels[] = {
        {20.0f, "20"},   {50.0f, "50"},   {100.0f, "100"}, {200.0f, "200"},   {500.0f, "500"},
        {1000.0f, "1k"}, {2000.0f, "2k"}, {5000.0f, "5k"}, {10000.0f, "10k"}, {20000.0f, "20k"},
    };
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.setColour(KhensuTheme::textTertiary);
    for (const auto& lbl : kHzLabels) {
        const float lx = logX(lbl.hz, cLeft, cW);
        // Tick mark
        g.setColour(KhensuTheme::borderSubtle.withAlpha(0.5f));
        g.drawVerticalLine(juce::roundToInt(lx), cBot, cBot + 3.0f);
        // Label
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(lbl.label, (int)lx - 14, hzLabelRow.getY(), 28, kHzLabelH, juce::Justification::centred, false);
    }

    // "Hz" unit label at right edge
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.setColour(KhensuTheme::textTertiary.withAlpha(0.6f));
    g.drawText("kHz", hzLabelRow.getRight() - 22, hzLabelRow.getY(), 22, kHzLabelH, juce::Justification::centredRight,
               false);
}

void HeatSpectrumComponent::mouseMove(const juce::MouseEvent& e)
{
    auto chartArea = getLocalBounds().withTrimmedBottom(16).withTrimmedLeft(28).toFloat().reduced(2.0f, 4.0f);
    const float cLeft = chartArea.getX();
    const float cW = chartArea.getWidth();

    // Band hover detection
    int newBand = -1;
    if (!bands_.empty() && chartArea.toNearestInt().contains(e.x, e.y)) {
        const float barW = cW / (float)bands_.size();
        int idx = (int)((e.x - cLeft) / barW);
        if (idx >= 0 && idx < (int)bands_.size()) newBand = idx;
    }

    // Pin hover detection
    int newPin = -1;
    for (int i = 0; i < (int)pins_.size(); ++i) {
        if (pins_[i].hz <= 0.0f) continue;
        const float px = logX(pins_[i].hz, cLeft, cW);
        if (std::abs((float)e.x - px) < 12.0f) {
            newPin = i;
            break;
        }
    }

    if (newBand != hoveredBand_ || newPin != hoveredPin_) {
        hoveredBand_ = newBand;
        hoveredPin_ = newPin;
        repaint();
    }
}

void HeatSpectrumComponent::mouseExit(const juce::MouseEvent&)
{
    hoveredPin_ = -1;
    hoveredBand_ = -1;
    repaint();
}

void HeatSpectrumComponent::setEqSuggestions(std::vector<EqPoint> pts)
{
    eqPoints_ = std::move(pts);
    repaint();
}

} // namespace khensu

namespace khensu {

juce::String HeatSpectrumComponent::summary() const
{
    // VIS-8: the octave bands' levels, the loudest and quietest named.
    if (bands_.empty()) return TRANS("Spectrum: not measured yet.");
    juce::String s = TRANS("Spectrum") + ", " + juce::String(static_cast<int>(bands_.size())) + " " + TRANS("bands");
    if (bands_.size() <= 10) { // octave bands (raw FFT bins have no centre table)
        size_t hi = 0, lo = 0;
        for (size_t i = 1; i < bands_.size(); ++i) {
            if (bands_[i] > bands_[hi]) hi = i;
            if (bands_[i] < bands_[lo]) lo = i;
        }
        ui::chart::Axis hz;
        hz.format = ui::chart::Format::Hz;
        s << ": " << TRANS("loudest around") << " " << hz.label(octaveCentreHz(static_cast<int>(hi)), true) << " ("
          << juce::String(bands_[hi], 1) << " dB), " << TRANS("quietest around") << " "
          << hz.label(octaveCentreHz(static_cast<int>(lo)), true) << " (" << juce::String(bands_[lo], 1) << " dB)";
    }
    if (showTargetTilt_) s << "; " << TRANS("genre tilt") << " " << juce::String(targetTiltDbPerOct_, 1) << " dB/oct";
    if (!pins_.empty()) s << "; " << static_cast<int>(pins_.size()) << " " << TRANS("resonances marked");
    return s + ".";
}

std::unique_ptr<juce::AccessibilityHandler> HeatSpectrumComponent::createAccessibilityHandler()
{
    return ui::chart::makeChartAccessibilityHandler(*this, [this] { return summary(); });
}

} // namespace khensu
