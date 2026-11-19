// desktop-app/Source/ui/shared/MeterBarComponent.cpp
#include "MeterBarComponent.h"
#include "KhensuLookAndFeel.h"

namespace khensu {

void MeterBarComponent::setRange(float a, float b)
{
    min_ = a;
    max_ = b;
    repaint();
}
void MeterBarComponent::setTargetZone(float lo, float hi)
{
    zoneLo_ = lo;
    zoneHi_ = hi;
    repaint();
}
void MeterBarComponent::setValue(float v, const juce::String& label)
{
    value_ = v;
    label_ = label;
    if (v >= peakHold_) peakHold_ = v;
    if (!animating_) lastFrameSec_ = -1.0;
    animating_ = true;
    repaint();
}

void MeterBarComponent::onVBlank(double timestampSec)
{
    if (!animating_) return;
    // Frame time, clamped so a stall doesn't jump the meter. The constants
    // are the old 30 Hz timer's per-tick values expressed per second.
    const double dt = lastFrameSec_ < 0.0 ? 1.0 / 60.0 : juce::jlimit(0.0, 0.1, timestampSec - lastFrameSec_);
    lastFrameSec_ = timestampSec;
    const auto ticks = static_cast<float>(dt * 30.0);
    bool needRepaint = false;

    // Peak-hold decay (0.012 per 1/30 s).
    if (peakHold_ > value_ + 0.005f) {
        peakHold_ = juce::jmax(value_, peakHold_ - 0.012f * ticks);
        needRepaint = true;
    }

    // Display-value ease (35 % of the gap per 1/30 s: ~270 ms to 99 %).
    const float diff = value_ - displayValue_;
    if (std::abs(diff) >= 0.002f) {
        displayValue_ += diff * (1.0f - std::pow(0.65f, ticks));
        needRepaint = true;
    } else {
        displayValue_ = value_;
    }

    if (needRepaint) repaint();

    // Idle until the next setValue() once both have settled.
    if (peakHold_ <= value_ + 0.005f && std::abs(value_ - displayValue_) < 0.002f) animating_ = false;
}

float MeterBarComponent::norm(float v) const
{
    if (max_ <= min_) return 0.0f;
    return juce::jlimit(0.0f, 1.0f, (v - min_) / (max_ - min_));
}

void MeterBarComponent::paint(juce::Graphics& g)
{
    auto b = getLocalBounds();
    auto labelArea = b.removeFromTop(16);
    g.setColour(KhensuTheme::textSecondary);
    g.setFont(khensu::KhensuTheme::uiFont(12.0f, juce::Font::plain));
    g.drawText(label_, labelArea, juce::Justification::centredLeft, true);

    auto track = b.reduced(0, 2).toFloat();
    g.setColour(KhensuTheme::bgVoid);
    g.fillRoundedRectangle(track, 3.0f);

    if (zoneHi_ > zoneLo_) {
        auto zx = track.getX() + norm(zoneLo_) * track.getWidth();
        auto zw = (norm(zoneHi_) - norm(zoneLo_)) * track.getWidth();
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.20f));
        g.fillRect(zx, track.getY(), zw, track.getHeight());
    }

    auto fillW = norm(displayValue_) * track.getWidth();
    g.setColour(KhensuTheme::accentTeal);
    g.fillRoundedRectangle(track.getX(), track.getY(), fillW, track.getHeight(), 3.0f);

    // Peak-hold tick
    if (peakHold_ > 0.001f) {
        auto px = track.getX() + norm(peakHold_) * track.getWidth() - 1.5f;
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.8f));
        g.fillRect(px, track.getY(), 3.0f, track.getHeight());
    }
}

} // namespace khensu
