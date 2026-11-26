// desktop-app/Source/ui/shared/LiveLufsMeterComponent.cpp
#include "LiveLufsMeterComponent.h"
#include "KhensuTheme.h"

namespace khensu {

LiveLufsMeterComponent::LiveLufsMeterComponent() { histogram_.fill(0); }

int LiveLufsMeterComponent::binFor(float lufs) const
{
    const float clamped = juce::jlimit(kMin, kMax - 0.01f, lufs);
    return juce::jlimit(0, kBins - 1, (int)((clamped - kMin) / (kMax - kMin) * kBins));
}

void LiveLufsMeterComponent::pushReading(float momentaryLufs, float shortTermLufs)
{
    momentary_ = std::isfinite(momentaryLufs) ? momentaryLufs : kMin;
    shortTerm_ = std::isfinite(shortTermLufs) ? shortTermLufs : kMin;
    hasReading_ = true;

    // Silence used to pile into bin 0 and dominate the distribution.
    if (!isGatedReading(momentaryLufs)) {
        const int bin = binFor(momentaryLufs);
        if (++histogram_[(size_t)bin] > histogramMax_) histogramMax_ = histogram_[(size_t)bin];
    }

    repaint();
}

void LiveLufsMeterComponent::reset()
{
    histogram_.fill(0);
    histogramMax_ = 1;
    hasReading_ = false;
    repaint();
}

void LiveLufsMeterComponent::paint(juce::Graphics& g)
{
    auto b = getLocalBounds();
    if (b.isEmpty()) return;

    auto barsArea = b.removeFromTop(juce::jmax(0, b.getHeight() - 16));
    auto histArea = b; // remaining bottom strip for the histogram

    auto barX = [](float lufs) { return juce::jlimit(0.0f, 1.0f, (lufs - kMin) / (kMax - kMin)); };

    if (!hasReading_) {
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.drawText(TRANS("Live level (RMS est., not LUFS) appears while the Copilot plugin plays in your DAW."),
                   barsArea, juce::Justification::centredLeft, false);
        return;
    }

    // Two thin bars: Momentary (top), Short-term (bottom), sharing the same
    // -40..0 LUFS scale so their relationship at a glance is meaningful.
    const int rowH = barsArea.getHeight() / 2;
    struct Row {
        float value;
        const char* label;
        juce::Colour colour;
    };
    const Row rows[2] = {
        {momentary_, "M rms", KhensuTheme::accentTeal},
        {shortTerm_, "S rms", KhensuTheme::accentAmber},
    };

    auto area = barsArea;
    for (const auto& row : rows) {
        auto r = area.removeFromTop(rowH).reduced(0, 1);
        g.setColour(KhensuTheme::bgOverlay);
        g.fillRoundedRectangle(r.toFloat(), 2.0f);

        const float norm = barX(row.value);
        auto fillW = (float)r.getWidth() * norm;
        g.setColour(row.colour.withAlpha(0.75f));
        g.fillRoundedRectangle(r.toFloat().withWidth(juce::jmax(2.0f, fillW)), 2.0f);

        g.setFont(KhensuTheme::monoFont(type::caption).boldened());
        g.setColour(KhensuTheme::textPrimary);
        g.drawText(juce::String(row.label) + " " + juce::String(row.value, 1), r.reduced(4, 0),
                   juce::Justification::centredLeft, false);
    }

    // Loudness-distribution histogram: how much of the session so far sat at
    // each LUFS bin - a flat, wide spread reads as dynamic mastering, a tall
    // narrow spike reads as a brickwalled, one-loudness master.
    if (histArea.getHeight() > 2 && histogramMax_ > 0) {
        const float binW = (float)histArea.getWidth() / (float)kBins;
        for (int i = 0; i < kBins; ++i) {
            const float frac = (float)histogram_[(size_t)i] / (float)histogramMax_;
            const float h = frac * (float)histArea.getHeight();
            g.setColour(KhensuTheme::accentTeal.withAlpha(0.35f));
            g.fillRect((float)histArea.getX() + i * binW, (float)histArea.getBottom() - h,
                       juce::jmax(1.0f, binW - 0.5f), h);
        }
    }
}

} // namespace khensu
