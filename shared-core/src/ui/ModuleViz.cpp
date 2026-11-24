#include <khensu/ui/ModuleViz.h>
#include <khensu/ui/KhensuPalette.h>
#include <algorithm>
#include <cmath>

namespace khensu::ui::module_viz {

Style Style::fromPalette()
{
    Style s;
    s.background = Palette::bgVoid.withAlpha(0.6f);
    s.grid = Palette::textPrimary.withAlpha(0.08f);
    s.curve = Palette::textPrimary;
    s.accent = Palette::accentTeal;
    s.text = Palette::textMuted;
    s.warn = Palette::statusWarn;
    return s;
}

namespace {
void frame(juce::Graphics& g, juce::Rectangle<float> r, const Style& s)
{
    g.setColour(s.background);
    g.fillRoundedRectangle(r, 3.0f);
}
} // namespace

void paintEqResponse(juce::Graphics& g, juce::Rectangle<float> r, const std::vector<EqBand>& bands,
                     double sampleRate, const Style& s, float rangeDb)
{
    frame(g, r, s);
    if (r.getWidth() < 8.0f || r.getHeight() < 6.0f) return;
    g.setColour(s.grid);
    g.drawHorizontalLine(juce::roundToInt(r.getCentreY()), r.getX(), r.getRight());
    for (const float hz : {100.0f, 1000.0f, 10000.0f}) {
        const float x = r.getX() + r.getWidth() * std::log(hz / 20.0f) / std::log(1000.0f);
        g.drawVerticalLine(juce::roundToInt(x), r.getY(), r.getBottom());
    }
    juce::Path curve, fill;
    const int n = std::max(2, juce::roundToInt(r.getWidth()));
    for (int i = 0; i <= n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n);
        const float hz = 20.0f * std::pow(1000.0f, t);
        float db = 0.0f;
        for (const auto& b : bands)
            if (b.enabled) db += EqCurveComponent::bandMagnitudeDb(b, hz, sampleRate);
        const float y = r.getCentreY() - juce::jlimit(-1.0f, 1.0f, db / rangeDb) * (r.getHeight() * 0.5f - 2.0f);
        const float x = r.getX() + t * r.getWidth();
        if (i == 0) {
            curve.startNewSubPath(x, y);
            fill.startNewSubPath(x, r.getCentreY());
        } else {
            curve.lineTo(x, y);
        }
        fill.lineTo(x, y);
    }
    fill.lineTo(r.getRight(), r.getCentreY());
    fill.closeSubPath();
    g.setColour(s.accent.withAlpha(0.18f));
    g.fillPath(fill);
    g.setColour(s.accent);
    g.strokePath(curve, juce::PathStrokeType(1.5f));
}

float compressorOutputDb(float x, float t, float ratio, float knee) noexcept
{
    ratio = std::max(1.0f, ratio);
    const float slope = 1.0f / ratio - 1.0f;
    if (knee > 0.0f && 2.0f * std::abs(x - t) <= knee) {
        const float d = x - t + knee * 0.5f;
        return x + slope * d * d / (2.0f * knee);
    }
    return x > t ? t + (x - t) / ratio : x;
}

void paintCompressor(juce::Graphics& g, juce::Rectangle<float> r, const CompressorState& c, const Style& s)
{
    frame(g, r, s);
    if (r.getWidth() < 20.0f || r.getHeight() < 12.0f) return;
    auto meter = r.removeFromRight(std::min(12.0f, r.getWidth() * 0.15f));
    r.removeFromRight(3.0f);
    constexpr float lo = -60.0f, hi = 0.0f;
    const float side = std::min(r.getWidth(), r.getHeight());
    auto plot = r.withSizeKeepingCentre(side, side).reduced(2.0f);
    auto px = [&](float db) { return plot.getX() + (db - lo) / (hi - lo) * plot.getWidth(); };
    auto py = [&](float db) { return plot.getBottom() - (db - lo) / (hi - lo) * plot.getHeight(); };
    g.setColour(s.grid);
    g.drawLine(px(lo), py(lo), px(hi), py(hi), 1.0f); // unity
    g.drawVerticalLine(juce::roundToInt(px(c.thresholdDb)), plot.getY(), plot.getBottom());
    juce::Path curve;
    for (int i = 0; i <= 60; ++i) {
        const float x = lo + (hi - lo) * static_cast<float>(i) / 60.0f;
        const float y = juce::jlimit(lo, hi, compressorOutputDb(x, c.thresholdDb, c.ratio, c.kneeDb));
        if (i == 0) curve.startNewSubPath(px(x), py(y));
        else curve.lineTo(px(x), py(y));
    }
    g.setColour(s.curve);
    g.strokePath(curve, juce::PathStrokeType(1.6f));
    if (c.inputDb) {
        const float x = juce::jlimit(lo, hi, *c.inputDb);
        const float y = juce::jlimit(lo, hi, compressorOutputDb(x, c.thresholdDb, c.ratio, c.kneeDb));
        g.setColour(s.accent);
        g.fillEllipse(px(x) - 3.0f, py(y) - 3.0f, 6.0f, 6.0f);
    }
    paintReductionMeter(g, meter, c.reductionDb.value_or(0.0f), s);
}

void paintReductionMeter(juce::Graphics& g, juce::Rectangle<float> r, float reductionDb, const Style& s, float maxDb)
{
    g.setColour(s.grid);
    g.fillRoundedRectangle(r, 2.0f);
    const float t = juce::jlimit(0.0f, 1.0f, reductionDb / maxDb);
    g.setColour(reductionDb > maxDb * 0.5f ? s.warn : s.accent);
    g.fillRoundedRectangle(r.withHeight(r.getHeight() * t), 2.0f);
}

void paintReductionHistory(juce::Graphics& g, juce::Rectangle<float> r, const std::vector<float>& historyDb,
                           const Style& s, float maxDb)
{
    frame(g, r, s);
    if (r.getWidth() < 8.0f) return;
    g.setColour(s.grid);
    for (const float db : {1.0f, 3.0f})
        if (db < maxDb) g.drawHorizontalLine(juce::roundToInt(r.getY() + r.getHeight() * db / maxDb), r.getX(), r.getRight());
    if (historyDb.empty()) return;
    juce::Path p;
    p.startNewSubPath(r.getX(), r.getY());
    const size_t n = historyDb.size();
    for (size_t i = 0; i < n; ++i) {
        const float x = r.getX() + r.getWidth() * static_cast<float>(i) / static_cast<float>(std::max<size_t>(1, n - 1));
        const float y = r.getY() + r.getHeight() * juce::jlimit(0.0f, 1.0f, historyDb[i] / maxDb);
        p.lineTo(x, y);
    }
    p.lineTo(r.getRight(), r.getY());
    p.closeSubPath();
    g.setColour(s.accent.withAlpha(0.35f));
    g.fillPath(p);
    g.setColour(s.accent);
    g.strokePath(p, juce::PathStrokeType(1.0f));
}

} // namespace khensu::ui::module_viz
