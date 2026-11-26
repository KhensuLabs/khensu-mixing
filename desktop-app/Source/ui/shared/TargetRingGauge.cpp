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
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.drawText(label_, title, juce::Justification::centred, true);

    const int diameter = juce::jmax(10, juce::jmin(bounds.getWidth(), bounds.getHeight() - 11));
    const auto circle = juce::Rectangle<float>((float)bounds.getCentreX() - diameter * 0.5f, (float)bounds.getY(),
                                               (float)diameter, (float)diameter);
    const float radius = circle.getWidth() * 0.5f - 3.0f;
    const float cx = circle.getCentreX();
    const float cy = circle.getCentreY();
    constexpr float start = -juce::MathConstants<float>::halfPi;

    // LG-14: subtle tinted inner glass disc with top specular sheen.
    const float discR = juce::jmax(2.0f, radius - 2.5f);
    const auto disc = juce::Rectangle<float>(cx - discR, cy - discR, discR * 2.0f, discR * 2.0f);
    juce::ColourGradient discGrad(KhensuTheme::bgSurface.brighter(0.08f).withAlpha(0.55f), cx, disc.getY(),
                                  KhensuTheme::bgVoid.withAlpha(0.45f), cx, disc.getBottom(), false);
    g.setGradientFill(discGrad);
    g.fillEllipse(disc);
    g.setColour(KhensuTheme::borderSubtle);
    g.drawEllipse(disc.reduced(0.5f), 0.8f);

    // Track arc with rounded caps.
    juce::Path track;
    track.addCentredArc(cx, cy, radius, radius, 0.0f, start, start + juce::MathConstants<float>::twoPi, true);
    g.setColour(KhensuTheme::bgRaised.withAlpha(0.85f));
    g.strokePath(track, juce::PathStrokeType(3.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float delta = std::abs(value_ - target_);
    const juce::Colour colour = delta <= tolerance_ ? KhensuTheme::accentTeal
                              : delta <= tolerance_ * 2.0f ? KhensuTheme::accentAmber
                                                            : KhensuTheme::accentRed;
    juce::Path matchArc;
    matchArc.addCentredArc(cx, cy, radius, radius, 0.0f, start,
                            start + juce::MathConstants<float>::twoPi * matchScore(value_, target_, tolerance_), true);
    // LG-14: soft halo glow behind the active arc + crisp rounded core.
    g.setColour(colour.withAlpha(0.24f));
    g.strokePath(matchArc, juce::PathStrokeType(6.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(colour);
    g.strokePath(matchArc, juce::PathStrokeType(3.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // The fixed top tick is the zero-distance target marker.
    g.setColour(KhensuTheme::textPrimary.withAlpha(0.8f));
    g.fillRect(cx - 1.0f, circle.getY() - 1.0f, 2.0f, 5.0f);

    const auto valueArea = bounds.withTrimmedTop(diameter + 1);
    g.setColour(colour);
    g.setFont(KhensuTheme::monoFont(type::caption));
    g.drawText(juce::String(delta, 1) + unit_ + " off", valueArea, juce::Justification::centred, true);
}

} // namespace khensu
