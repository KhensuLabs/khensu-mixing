#include "TargetRingGauge.h"

#include <cmath>

namespace khensu {

void TargetRingGauge::setTarget(const juce::String& label, float value, float target, float tolerance,
                                 const juce::String& unit)
{
    label_ = label;
    value_ = value;
    target_ = target;
    tolerance_ = juce::jmax(tolerance, 0.001f);
    unit_ = unit;

    const float delta = value_ - target_;
    setTooltip(label_ + ": " + juce::String(value_, 1) + unit_ + " vs target " + juce::String(target_, 1) + unit_
               + " (" + (delta >= 0.0f ? "+" : "") + juce::String(delta, 1) + unit_ + ")");
    repaint();
}

float TargetRingGauge::matchScore(float value, float target, float tolerance) noexcept
{
    if (tolerance <= 0.0f) return 0.0f;
    return 1.0f - juce::jlimit(0.0f, 1.0f, std::abs(value - target) / (tolerance * 3.0f));
}

void TargetRingGauge::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().reduced(2);
    const auto title = bounds.removeFromTop(12);
    g.setColour(KhensuTheme::textTertiary);
    g.setFont(KhensuTheme::labelFont(8.5f));
    g.drawText(label_, title, juce::Justification::centred, true);

    const int diameter = juce::jmax(10, juce::jmin(bounds.getWidth(), bounds.getHeight() - 11));
    const auto circle = juce::Rectangle<float>((float)bounds.getCentreX() - diameter * 0.5f, (float)bounds.getY(),
                                               (float)diameter, (float)diameter);
    const float radius = circle.getWidth() * 0.5f - 3.0f;
    const float cx = circle.getCentreX();
    const float cy = circle.getCentreY();
    constexpr float start = -juce::MathConstants<float>::halfPi;

    juce::Path track;
    track.addCentredArc(cx, cy, radius, radius, 0.0f, start, start + juce::MathConstants<float>::twoPi, true);
    g.setColour(KhensuTheme::bgRaised);
    g.strokePath(track, juce::PathStrokeType(3.0f));

    const float delta = std::abs(value_ - target_);
    const juce::Colour colour = delta <= tolerance_ ? KhensuTheme::accentTeal
                              : delta <= tolerance_ * 2.0f ? KhensuTheme::accentAmber
                                                            : KhensuTheme::accentRed;
    juce::Path matchArc;
    matchArc.addCentredArc(cx, cy, radius, radius, 0.0f, start,
                            start + juce::MathConstants<float>::twoPi * matchScore(value_, target_, tolerance_), true);
    g.setColour(colour);
    g.strokePath(matchArc, juce::PathStrokeType(3.0f));

    // The fixed top tick is the zero-distance target marker.
    g.setColour(KhensuTheme::textPrimary.withAlpha(0.8f));
    g.fillRect(cx - 1.0f, circle.getY() - 1.0f, 2.0f, 5.0f);

    const auto valueArea = bounds.withTrimmedTop(diameter + 1);
    g.setColour(colour);
    g.setFont(KhensuTheme::monoFont(9.0f));
    g.drawText(juce::String(delta, 1) + unit_ + " off", valueArea, juce::Justification::centred, true);
}

} // namespace khensu
