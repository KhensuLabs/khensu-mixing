// desktop-app/Source/ui/SystemBrain/FeatureCardComponent.cpp
#include "FeatureCardComponent.h"
#include "../shared/KhensuLookAndFeel.h"

namespace khensu {

void FeatureCardComponent::setEntry(const FeatureEntry& e)
{
    entry_ = e;
    hasSessionValue_ = false;
    repaint();
}

float FeatureCardComponent::normaliseValue(float v) const
{
    float span = rangeMax_ - rangeMin_;
    if (span <= 0.0f) return 0.0f;
    return juce::jlimit(0.0f, 1.0f, (v - rangeMin_) / span);
}

bool FeatureCardComponent::valueInTargetZone() const { return sessionValue_ >= zoneLo_ && sessionValue_ <= zoneHi_; }

juce::Colour FeatureCardComponent::valueColour() const
{
    return valueInTargetZone() ? KhensuTheme::accentGreen : KhensuTheme::accentAmber;
}

void FeatureCardComponent::drawBarVisualization(juce::Graphics& g, const juce::Rectangle<int>& area)
{
    // Background track
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.5f));
    g.fillRoundedRectangle(area.toFloat(), 2.0f);

    float rangeSpan = rangeMax_ - rangeMin_;
    if (rangeSpan <= 0.0f) return;

    // Target zone highlight
    float loFrac = normaliseValue(zoneLo_);
    float hiFrac = normaliseValue(zoneHi_);
    float targetX = (float)area.getX() + loFrac * area.getWidth();
    float targetW = (hiFrac - loFrac) * area.getWidth();
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.25f));
    g.fillRoundedRectangle(juce::Rectangle<float>(targetX, (float)area.getY(), targetW, (float)area.getHeight()), 2.0f);

    // Session value tick
    float valFrac = normaliseValue(sessionValue_);
    float indicatorX = (float)area.getX() + valFrac * area.getWidth();
    g.setColour(valueColour());
    g.fillEllipse(indicatorX - 3.5f, (float)area.getCentreY() - 3.5f, 7.0f, 7.0f);
}

void FeatureCardComponent::drawDotVisualization(juce::Graphics& g, const juce::Rectangle<int>& area)
{
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.5f));
    g.fillRoundedRectangle(area.toFloat(), 2.0f);

    float valFrac = normaliseValue(sessionValue_);
    float cx = (float)area.getX() + valFrac * area.getWidth();
    float cy = (float)area.getCentreY();

    // Centre threshold line
    float zoneFrac = normaliseValue((zoneLo_ + zoneHi_) * 0.5f);
    float zoneX = (float)area.getX() + zoneFrac * area.getWidth();
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.4f));
    g.fillRect(juce::Rectangle<float>(zoneX - 1.0f, (float)area.getY(), 2.0f, (float)area.getHeight()));

    g.setColour(valueColour());
    g.fillEllipse(cx - 4.0f, cy - 4.0f, 8.0f, 8.0f);
}

void FeatureCardComponent::drawPercentileVisualization(juce::Graphics& g, const juce::Rectangle<int>& area)
{
    float frac = normaliseValue(sessionValue_);

    // Background ring
    auto centre = area.getCentre().toFloat();
    float radius = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f - 2.0f;
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.5f));
    g.drawEllipse(centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f, 3.0f);

    // Filled arc
    juce::Path arc;
    arc.addPieSegment(centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f,
                      -juce::MathConstants<float>::halfPi,
                      -juce::MathConstants<float>::halfPi + frac * juce::MathConstants<float>::twoPi, 0.0f);
    g.setColour(valueColour());
    g.fillPath(arc);

    // Value label
    g.setFont(KhensuTheme::monoFont(9.0f));
    g.setColour(KhensuTheme::textTertiary);
    juce::String label((int)std::round(sessionValue_));
    g.drawText(label, area, juce::Justification::centred, false);
}

void FeatureCardComponent::drawPresenceVisualization(juce::Graphics& g, const juce::Rectangle<int>& area)
{
    const auto& labels = entry_.visualization.labels;
    if (labels.empty()) {
        drawBarVisualization(g, area); // fallback
        return;
    }

    int n = (int)labels.size();
    int chipW = area.getWidth() / n;
    int gap = 2;
    int x = area.getX();

    // Normalise value to bucket index
    int activeIdx = (int)std::floor(normaliseValue(sessionValue_) * (float)(n - 1) + 0.5f);
    activeIdx = juce::jlimit(0, n - 1, activeIdx);

    for (int i = 0; i < n; ++i) {
        auto chip = juce::Rectangle<int>(x + i * chipW + gap, area.getY(), chipW - gap * 2, area.getHeight());
        bool active = i == activeIdx;
        g.setColour(active ? valueColour().withAlpha(0.25f) : KhensuTheme::bgVoid.withAlpha(0.5f));
        g.fillRoundedRectangle(chip.toFloat(), 2.0f);

        g.setFont(KhensuTheme::labelFont(7.5f));
        g.setColour(active ? valueColour() : KhensuTheme::textTertiary);
        g.drawText(juce::String(labels[i]), chip, juce::Justification::centred, true);
    }
}

void FeatureCardComponent::drawIconVisualization(juce::Graphics& g, const juce::Rectangle<int>& area)
{
    juce::Colour col = valueColour();
    if (sessionValue_ >= zoneHi_)
        col = KhensuTheme::accentRed;
    else if (sessionValue_ <= zoneLo_)
        col = KhensuTheme::accentAmber;

    auto centre = area.getCentre().toFloat();
    float r = juce::jmin(area.getWidth(), area.getHeight()) * 0.35f;

    g.setColour(col.withAlpha(0.2f));
    g.fillEllipse(centre.x - r, centre.y - r, r * 2.0f, r * 2.0f);
    g.setColour(col);
    g.drawEllipse(centre.x - r, centre.y - r, r * 2.0f, r * 2.0f, 2.0f);

    // Small centre dot
    g.fillEllipse(centre.x - 2.0f, centre.y - 2.0f, 4.0f, 4.0f);

    g.setFont(KhensuTheme::labelFont(8.0f));
    g.setColour(KhensuTheme::textTertiary);
    auto labelArea = area;
    g.drawText(juce::String(entry_.visualization.icon), labelArea.removeFromBottom(12), juce::Justification::centred,
               true);
}

void FeatureCardComponent::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced(2.0f);
    g.setColour(KhensuTheme::bgSurface);
    g.fillRoundedRectangle(b, 5.0f);

    // Category tag: width fits its own text (clamped to a sane range) instead
    // of a fixed 120px, since on a narrow card (two-column grid, ~220px each)
    // a fixed-width tag ate most of the row the title needed. Title now sits
    // on its own row below at the card's FULL width regardless of tag size or
    // card width, so long names ("Reference Spectral Delta", "Streaming
    // Loudness Normalization", ...) stop truncating to "X..." on every narrow
    // card - the original single-row layout reserved x=140 unconditionally,
    // leaving as little as ~68px for the title text.
    const auto tagFont = khensu::KhensuTheme::uiFont(10.0f, juce::Font::bold);
    g.setFont(tagFont);
    const int tagTextW = juce::GlyphArrangement::getStringWidthInt(tagFont, juce::String(entry_.category));
    const int tagW = juce::jlimit(50, getWidth() - 24, tagTextW + 16);
    auto tagArea = juce::Rectangle<int>(12, 8, tagW, 16);
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.18f));
    g.fillRoundedRectangle(tagArea.toFloat(), 3.0f);
    g.setColour(KhensuTheme::accentTeal);
    g.drawText(juce::String(entry_.category), tagArea, juce::Justification::centred, true);

    g.setColour(KhensuTheme::textPrimary);
    g.setFont(khensu::KhensuTheme::uiFont(13.0f, juce::Font::bold));
    g.drawText(juce::String(entry_.name), 12, 27, getWidth() - 24, 18, juce::Justification::centredLeft, true);

    int descWidth = getWidth() - 24;

    if (hasSessionValue_ && entry_.visualization.hasVisualization()) {
        descWidth = getWidth() - 115; // make space for visualizer
        auto vArea = juce::Rectangle<int>(getWidth() - 100, 49, 88, 22);

        const juce::String type = juce::String(entry_.visualization.type);
        if (type == "bar") {
            drawBarVisualization(g, vArea);
        } else if (type == "dot") {
            drawDotVisualization(g, vArea);
        } else if (type == "percentile") {
            drawPercentileVisualization(g, vArea);
        } else if (type == "presence") {
            drawPresenceVisualization(g, vArea);
        } else if (type == "icon") {
            drawIconVisualization(g, vArea);
        } else {
            drawBarVisualization(g, vArea); // safe fallback
        }
    }

    g.setColour(KhensuTheme::textSecondary);
    g.setFont(khensu::KhensuTheme::uiFont(11.0f, juce::Font::plain));
    g.drawText(juce::String(entry_.short_description), 12, 47, descWidth, 20, juce::Justification::topLeft, true);
}

void FeatureCardComponent::mouseDown(const juce::MouseEvent&)
{
    if (onSelect && !entry_.id.empty()) onSelect(entry_.id);
}

} // namespace khensu
