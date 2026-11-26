// desktop-app/Source/ui/shared/ZoneMeterComponent.cpp
#include "ZoneMeterComponent.h"

namespace khensu {

void ZoneMeterComponent::setRange(float minVal, float maxVal)
{
    min_ = minVal;
    max_ = maxVal;
    repaint();
}
void ZoneMeterComponent::setTargetZone(float lo, float hi)
{
    zoneLo_ = lo;
    zoneHi_ = hi;
    repaint();
}
void ZoneMeterComponent::setValue(float v, const juce::String& caption)
{
    value_ = v;
    caption_ = caption;
    if (!animating_) lastFrameSec_ = -1.0;
    animating_ = true;
}
void ZoneMeterComponent::addZoneLabel(float normPos, const juce::String& label)
{
    zoneLabels_.push_back({normPos, label});
    repaint();
}
void ZoneMeterComponent::clearZoneLabels()
{
    zoneLabels_.clear();
    repaint();
}

float ZoneMeterComponent::normalise(float v) const
{
    if (max_ <= min_) return 0.0f;
    return juce::jlimit(0.0f, 1.0f, (v - min_) / (max_ - min_));
}

juce::Colour ZoneMeterComponent::fillColourForValue(float v) const
{
    if (v >= zoneLo_ && v <= zoneHi_) return KhensuTheme::accentTeal;
    if (v > zoneHi_) return KhensuTheme::accentRed;
    return KhensuTheme::accentAmber;
}

void ZoneMeterComponent::onVBlank(double timestampSec)
{
    if (!animating_) return;
    const double dt = lastFrameSec_ < 0.0 ? 1.0 / 60.0 : juce::jlimit(0.0, 0.1, timestampSec - lastFrameSec_);
    lastFrameSec_ = timestampSec;
    const float delta = value_ - displayValue_;
    if (std::abs(delta) < 0.001f) {
        displayValue_ = value_;
        animating_ = false;
    } else {
        // 18 % of the gap per 1/60 s (~300 ms ease-out), at any refresh rate.
        displayValue_ += delta * (1.0f - std::pow(0.82f, static_cast<float>(dt * 60.0)));
    }
    repaint();
}

void ZoneMeterComponent::paint(juce::Graphics& g)
{
    auto b = getLocalBounds();
    constexpr float radius = 4.0f;
    constexpr int barH = 20;
    constexpr int labelH = 14;

    // Track background
    auto trackBounds = b.removeFromTop(barH).toFloat();
    g.setColour(KhensuTheme::bgRaised);
    g.fillRoundedRectangle(trackBounds, radius);

    // Fill
    float norm = normalise(displayValue_);
    float fillW = trackBounds.getWidth() * norm;
    if (fillW > 0.0f) {
        auto fillRect = trackBounds.withWidth(fillW);
        juce::Colour fillCol = fillColourForValue(displayValue_);
        g.setColour(fillCol);
        g.fillRoundedRectangle(fillRect, radius);

        // Glow behind fill
        juce::ColourGradient glow(fillCol.withAlpha(0.20f), fillRect.getCentreX(), fillRect.getCentreY(),
                                  juce::Colours::transparentBlack, fillRect.getCentreX(), fillRect.getBottom() + 8.0f,
                                  false);
        g.setGradientFill(glow);
        g.fillRoundedRectangle(fillRect.expanded(0.0f, 4.0f), radius + 2.0f);
    }

    // Value marker line
    float markerX = trackBounds.getX() + norm * trackBounds.getWidth();
    g.setColour(KhensuTheme::textPrimary.withAlpha(0.8f));
    g.fillRect(juce::Rectangle<float>(markerX - 1.0f, trackBounds.getY(), 2.0f, trackBounds.getHeight()));

    // Zone labels below bar
    b.removeFromTop(2);
    auto labelArea = b.removeFromTop(labelH);
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.setColour(KhensuTheme::textTertiary);
    constexpr int kZoneLabelW = 48;
    for (const auto& zl : zoneLabels_) {
        float lx = (float)labelArea.getX() + zl.normPos * (float)labelArea.getWidth();
        // Clamp the 48px label box inside the meter: a zone at normPos 0 or 1
        // was centred on the track edge, so half the text hung outside the
        // component and got clipped by the parent (kimi audit P1 #34).
        const int x = juce::jlimit(labelArea.getX(), juce::jmax(labelArea.getX(), labelArea.getRight() - kZoneLabelW),
                                   juce::roundToInt(lx) - kZoneLabelW / 2);
        g.drawText(zl.text.toUpperCase(), x, labelArea.getY(), kZoneLabelW, labelH, juce::Justification::centred, false);
    }

    // Caption (value label)
    if (caption_.isNotEmpty()) {
        g.setFont(KhensuTheme::monoFont(type::caption));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(caption_, b, juce::Justification::centredLeft, true);
    }
}

} // namespace khensu
