// desktop-app/Source/ui/shared/RadarChartComponent.cpp
#include "RadarChartComponent.h"
#include "KhensuLookAndFeel.h"
#include "KhensuTheme.h"
#include <algorithm>

namespace khensu {

void RadarChartComponent::setAxes(std::vector<RadarAxis> axes)
{
    axes_ = std::move(axes);
    repaint();
}
void RadarChartComponent::setReference(std::vector<float> values)
{
    reference_ = std::move(values);
    repaint();
}

void RadarChartComponent::paint(juce::Graphics& g)
{
    if (axes_.empty()) return;

    // If every axis is at or near zero the data polygon collapses to an invisible
    // centre point. Show a placeholder instead so the panel doesn't look broken.
    const bool hasData = std::any_of(axes_.begin(), axes_.end(), [](const RadarAxis& a) { return a.value > 0.01f; });
    if (!hasData) {
        g.setFont(KhensuTheme::labelFont(11.0f));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText("Deep analysis pending...", getLocalBounds(), juce::Justification::centred, false);
        return;
    }

    auto b = getLocalBounds().toFloat().reduced(24.0f);
    float r = juce::jmin(b.getWidth(), b.getHeight()) * 0.5f - 20.0f;
    // Below ~88px the fixed 24px inset and 20px label margin exceed the half-
    // extent and r goes NEGATIVE, which mirrors every vertex back through the
    // centre and draws an inside-out web (kimi audit P1 #33). There is no
    // useful chart at that size, so draw nothing rather than garbage.
    if (r < 1.0f) return;

    float cx = b.getCentreX(), cy = b.getCentreY();
    int n = (int)axes_.size();

    auto angleFor = [&](int i) {
        return juce::MathConstants<float>::twoPi * i / n - juce::MathConstants<float>::halfPi;
    };
    auto pointFor = [&](int i, float norm) {
        float a = angleFor(i);
        return juce::Point<float>(cx + r * norm * std::cos(a), cy + r * norm * std::sin(a));
    };

    // Ring guides
    g.setColour(KhensuTheme::borderSubtle);
    for (float f : {0.33f, 0.66f, 1.0f}) {
        juce::Path ring;
        for (int i = 0; i < n; ++i) {
            auto pt = pointFor(i, f);
            if (i == 0)
                ring.startNewSubPath(pt);
            else
                ring.lineTo(pt);
        }
        ring.closeSubPath();
        g.strokePath(ring, juce::PathStrokeType(0.5f));
    }

    // Axis lines
    g.setColour(KhensuTheme::borderSubtle);
    for (int i = 0; i < n; ++i)
        g.drawLine(cx, cy, pointFor(i, 1.0f).x, pointFor(i, 1.0f).y, 0.5f);

    // Reference polygon (amber dashed)
    if (!reference_.empty()) {
        juce::Path refPath;
        for (int i = 0; i < n && i < (int)reference_.size(); ++i) {
            auto pt = pointFor(i, reference_[i]);
            if (i == 0)
                refPath.startNewSubPath(pt);
            else
                refPath.lineTo(pt);
        }
        refPath.closeSubPath();
        g.setColour(KhensuTheme::accentAmber.withAlpha(0.7f));
        juce::PathStrokeType dashType(1.0f);
        float dashes[] = {4.0f, 3.0f};
        juce::Path dashed;
        dashType.createDashedStroke(dashed, refPath, dashes, 2);
        g.fillPath(dashed); // createDashedStroke() yields a filled outline
    }

    // Data polygon - filled with radial gradient for 3D depth illusion, then stroked with glow
    juce::Path dataPath;
    for (int i = 0; i < n; ++i) {
        float val = juce::jlimit(0.0f, 1.0f, axes_[i].value);
        auto pt = pointFor(i, val);
        if (i == 0)
            dataPath.startNewSubPath(pt);
        else
            dataPath.lineTo(pt);
    }
    dataPath.closeSubPath();

    // Radial gradient: bright teal at polygon interior -> dim at edges
    juce::ColourGradient radial(KhensuTheme::accentTeal.withAlpha(0.35f), cx, cy,
                                KhensuTheme::accentTealDim.withAlpha(0.10f), cx + r, cy,
                                true // radial
    );
    g.setGradientFill(radial);
    g.fillPath(dataPath);

    // Glow: wide soft stroke, then sharp bright stroke on top
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.20f));
    g.strokePath(dataPath, juce::PathStrokeType(5.0f));
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.90f));
    g.strokePath(dataPath, juce::PathStrokeType(1.5f));

    // Axis tip labels
    g.setFont(KhensuTheme::labelFont(9.0f));
    for (int i = 0; i < n; ++i) {
        auto pt = pointFor(i, 1.15f);
        juce::String lbl = axes_[i].label.toUpperCase();
        bool hovered = (i == hoveredAxis_);
        g.setColour(hovered ? KhensuTheme::accentTeal : KhensuTheme::textSecondary);
        g.drawText(lbl, (int)(pt.x - 24.0f), (int)(pt.y - 7.0f), 48, 14, juce::Justification::centred, false);
    }
}

void RadarChartComponent::mouseMove(const juce::MouseEvent& e)
{
    mousePos_ = e.position;
    const int n = (int)axes_.size();
    if (n < 3) return;

    auto bounds = getLocalBounds().toFloat().reduced(24.0f);
    float cx = bounds.getCentreX(), cy = bounds.getCentreY();
    float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const float twoPi = juce::MathConstants<float>::twoPi;
    const float kHitR2 = 16.0f * 16.0f; // 16px hit radius, squared

    int hit = -1;
    for (int i = 0; i < n; ++i) {
        float a = -juce::MathConstants<float>::halfPi + twoPi * i / n;
        float vx = cx + std::cos(a) * radius;
        float vy = cy + std::sin(a) * radius;
        float dx = e.position.x - vx;
        float dy = e.position.y - vy;
        if (dx * dx + dy * dy <= kHitR2) {
            hit = i;
            break;
        }
    }

    if (hit != hoveredAxis_) {
        hoveredAxis_ = hit;
        repaint();
    }
}

void RadarChartComponent::mouseExit(const juce::MouseEvent&)
{
    if (hoveredAxis_ != -1) {
        hoveredAxis_ = -1;
        repaint();
    }
}

} // namespace khensu
