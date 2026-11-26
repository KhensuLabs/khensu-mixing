// desktop-app/Source/ui/shared/VizMaps.cpp
#include "VizMaps.h"
#include "KhensuTheme.h"

namespace khensu::ui::viz {

const std::array<juce::PixelARGB, 256>& spectrogramLut()
{
    static const auto lut = [] {
        std::array<juce::PixelARGB, 256> t{};
        for (size_t i = 0; i < t.size(); ++i)
            t[i] = spectrogram(static_cast<float>(i) / 255.0f).getPixelARGB();
        return t;
    }();
    return lut;
}

juce::Colour themedDiverging(float t) { return KhensuTheme::vizDiverging(t); }

namespace {

void paintBarLabels(juce::Graphics& g, juce::Rectangle<float> bar, float labelW, const juce::String& top,
                    const juce::String& mid, const juce::String& bottom)
{
    g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
    g.setColour(KhensuTheme::textTertiary);
    const auto at = [&](float y, const juce::String& t) {
        if (t.isEmpty()) return;
        g.drawText(t, juce::Rectangle<float>(bar.getRight() + 3.0f, y - 6.5f, labelW, 13.0f),
                   juce::Justification::centredLeft, false);
    };
    at(bar.getY() + 4.0f, top);
    at(bar.getCentreY(), mid);
    at(bar.getBottom() - 4.0f, bottom);
}

} // namespace

void paintSpectrogramBar(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& top,
                         const juce::String& bottom, const juce::String& caption)
{
    if (area.getHeight() < 16.0f || area.getWidth() < 8.0f) return;
    auto r = area;
    juce::Rectangle<float> captionArea;
    if (caption.isNotEmpty() && r.getHeight() > 60.0f) captionArea = r.removeFromBottom(14.0f);
    const auto bar = r.withWidth(6.0f);
    juce::ColourGradient grad(spectrogram(1.0f), 0.0f, bar.getY(), spectrogram(0.0f), 0.0f, bar.getBottom(), false);
    for (int i = 1; i < 8; ++i)
        grad.addColour(i / 8.0, spectrogram(1.0f - static_cast<float>(i) / 8.0f));
    g.setGradientFill(grad);
    g.fillRoundedRectangle(bar, 3.0f);
    g.setColour(KhensuTheme::borderSubtle);
    g.drawRoundedRectangle(bar, 3.0f, 0.75f);
    paintBarLabels(g, bar, r.getWidth() - 9.0f, top, {}, bottom);
    if (!captionArea.isEmpty()) {
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(caption, captionArea, juce::Justification::centredLeft, true);
    }
}

void paintDivergingBar(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& high,
                       const juce::String& mid, const juce::String& low)
{
    if (area.getHeight() < 16.0f || area.getWidth() < 8.0f) return;
    const auto bar = area.withWidth(6.0f);
    juce::ColourGradient grad(themedDiverging(1.0f), 0.0f, bar.getY(), themedDiverging(-1.0f), 0.0f, bar.getBottom(),
                              false);
    for (int i = 1; i < 8; ++i)
        grad.addColour(i / 8.0, themedDiverging(1.0f - 2.0f * static_cast<float>(i) / 8.0f));
    g.setGradientFill(grad);
    g.fillRoundedRectangle(bar, 3.0f);
    g.setColour(KhensuTheme::borderSubtle);
    g.drawRoundedRectangle(bar, 3.0f, 0.75f);
    paintBarLabels(g, bar, area.getWidth() - 9.0f, high, mid, low);
}

void paintFrequencyTicks(juce::Graphics& g, juce::Rectangle<float> plot, float labelW, bool grid, float lowHz,
                         float highHz)
{
    if (plot.getHeight() < 20.0f) return;
    g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
    // Thin the ticks when the plot is short (about 14 px a label).
    const int every = plot.getHeight() < 90.0f ? 3 : (plot.getHeight() < 150.0f ? 2 : 1);
    int i = 0;
    for (const float hz : kFreqTicksHz) {
        const bool decade = hz == 100.0f || hz == 1000.0f || hz == 10000.0f;
        if (hz < lowHz || hz > highHz) continue;
        if (!decade && (i++ % every) != 0) continue;
        const float y = yForHz(plot, hz);
        if (grid) {
            g.setColour(KhensuTheme::textPrimary.withAlpha(0.08f));
            g.drawHorizontalLine(juce::roundToInt(y), plot.getX(), plot.getRight());
        }
        g.setColour(KhensuTheme::textTertiary);
        const float ty = juce::jlimit(plot.getY(), plot.getBottom() - 12.0f, y - 6.0f);
        g.drawText(freqTickLabel(hz), juce::Rectangle<float>(plot.getX() - labelW, ty, labelW - 4.0f, 12.0f),
                   juce::Justification::centredRight, false);
    }
}

juce::Colour stoppedTint(juce::Colour c)
{
    const auto grey = KhensuTheme::textTertiary;
    return c.interpolatedWith(grey.withAlpha(c.getFloatAlpha()), 0.65f).withMultipliedAlpha(0.8f);
}

juce::String stoppedCaptionText() { return TRANS("Stopped - analysed values"); }

void paintStoppedCaption(juce::Graphics& g, juce::Rectangle<float> area)
{
    if (area.getWidth() < 60.0f || area.getHeight() < 14.0f) return;
    const auto font = KhensuTheme::labelFont(KhensuTheme::kFontCaption);
    g.setFont(font);
    const auto text = stoppedCaptionText();
    const float w = std::min(area.getWidth(), juce::GlyphArrangement::getStringWidth(font, text) + 12.0f);
    const auto chip = juce::Rectangle<float>(area.getRight() - w, area.getY(), w, 14.0f);
    g.setColour(KhensuTheme::bgOverlay.withAlpha(0.85f));
    g.fillRoundedRectangle(chip, 3.0f);
    g.setColour(KhensuTheme::textTertiary);
    g.drawText(text, chip, juce::Justification::centred, true);
}

} // namespace khensu::ui::viz
