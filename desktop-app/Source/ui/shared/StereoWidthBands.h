// desktop-app/Source/ui/shared/StereoWidthBands.h
// VIZ-4b: stereo width per octave - where in the spectrum the mix is wide
// and where it is narrow - beside the vectorscope (which shows the whole
// image at once and nothing at rest). Width is the app's one definition,
// clamp(2 S^2 / (M^2 + S^2), 0, 1) (uncorrelated ~ 1, mono 0), per octave:
//   analysed - from the analysis' mid / side octave levels (always shown);
//   live     - from what is playing, measured from the tap's width ring.
// The genre's width window is the band behind the bars.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <array>
#include <optional>
#include <vector>
#include <juce_dsp/juce_dsp.h>
#include "ChartStyle.h"
#include <khensu/ui/VizPalette.h>

namespace khensu {

namespace width_bands {

inline constexpr int kBands = 10;
inline constexpr std::array<float, kBands> kCentres{31.5f, 63.0f, 125.0f, 250.0f, 500.0f,
                                                    1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f};

inline float widthOf(double midPower, double sidePower) noexcept
{
    const double total = midPower + sidePower;
    return total > 1e-15 ? static_cast<float>(juce::jlimit(0.0, 1.0, 2.0 * sidePower / total)) : 0.0f;
}

/// From the analysis' mid / side octave levels (dB, 10 bands).
inline std::vector<float> fromOctaveLevels(const std::vector<float>& midDb, const std::vector<float>& sideDb)
{
    std::vector<float> out;
    if (midDb.size() < kBands || sideDb.size() < kBands) return out;
    for (size_t b = 0; b < kBands; ++b) {
        if (midDb[b] < -89.0f && sideDb[b] < -89.0f) {
            out.push_back(std::numeric_limits<float>::quiet_NaN());
            continue;
        }
        out.push_back(widthOf(std::pow(10.0, midDb[b] / 10.0), std::pow(10.0, sideDb[b] / 10.0)));
    }
    return out;
}

/// From a block of stereo samples (power-of-two FFT of mid and side).
inline std::vector<float> fromSamples(const float* left, const float* right, int n, double sampleRate)
{
    std::vector<float> out;
    int order = 0;
    while ((1 << (order + 1)) <= n && order < 13)
        ++order;
    if (order < 9 || sampleRate <= 0.0) return out;
    const int size = 1 << order;
    juce::dsp::FFT fft(order);
    std::vector<float> m(2 * static_cast<size_t>(size), 0.0f), s(2 * static_cast<size_t>(size), 0.0f);
    for (int i = 0; i < size; ++i) {
        const float w = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * static_cast<float>(i) / static_cast<float>(size - 1));
        m[static_cast<size_t>(i)] = 0.5f * (left[i] + right[i]) * w;
        s[static_cast<size_t>(i)] = 0.5f * (left[i] - right[i]) * w;
    }
    fft.performFrequencyOnlyForwardTransform(m.data(), true);
    fft.performFrequencyOnlyForwardTransform(s.data(), true);
    for (const float fc : kCentres) {
        const int k0 = std::max(1, juce::roundToInt(fc / std::sqrt(2.0f) * static_cast<float>(size) / static_cast<float>(sampleRate)));
        const int k1 = std::min(size / 2, juce::roundToInt(fc * std::sqrt(2.0f) * static_cast<float>(size) / static_cast<float>(sampleRate)));
        double pm = 0.0, ps = 0.0;
        for (int k = k0; k <= k1; ++k) {
            pm += static_cast<double>(m[static_cast<size_t>(k)]) * m[static_cast<size_t>(k)];
            ps += static_cast<double>(s[static_cast<size_t>(k)]) * s[static_cast<size_t>(k)];
        }
        out.push_back(pm + ps > 1e-12 ? widthOf(pm, ps) : std::numeric_limits<float>::quiet_NaN());
    }
    return out;
}

} // namespace width_bands

class StereoWidthBandsChart final : public ThemedChart {
  public:
    StereoWidthBandsChart()
    {
        x.lo = 22.0;
        x.hi = 22000.0;
        x.scale = ui::chart::Scale::Log;
        x.format = ui::chart::Format::Hz;
        setXRange(22.0, 22000.0);
        y.lo = 0.0;
        y.hi = 1.0;
        y.format = ui::chart::Format::Percent;
        setMargins(34, 18, 6, 18);
        setTitle(TRANS("Stereo width by frequency"));
        setTooltip(TRANS("Stereo width per octave: 0 is mono, 100 % uncorrelated. Outline: the analysed track; "
                         "filled: what is playing now. The band is the genre's width window."));
    }

    void setAnalysed(std::vector<float> w)
    {
        analysed_ = std::move(w);
        repaint();
    }
    void setLive(std::vector<float> w)
    {
        live_ = std::move(w);
        repaint();
    }
    void setWindow(std::optional<std::pair<float, float>> window)
    {
        window_ = window;
        repaint();
    }
    const std::vector<float>& analysed() const noexcept { return analysed_; }

  protected:
    void paintData(juce::Graphics& g, juce::Rectangle<float> plot) override
    {
        if (window_) {
            g.setColour(KhensuTheme::accentTeal.withAlpha(0.08f));
            const float y0 = py(window_->second), y1 = py(window_->first);
            g.fillRect(juce::Rectangle<float>(plot.getX(), y0, plot.getWidth(), std::max(1.0f, y1 - y0)));
        }
        for (size_t b = 0; b < width_bands::kBands; ++b) {
            const float fc = width_bands::kCentres[b];
            const float x0 = px(fc / std::sqrt(2.0f)) + 2.0f, x1 = px(fc * std::sqrt(2.0f)) - 2.0f;
            if (b < live_.size() && std::isfinite(live_[b])) {
                g.setColour(KhensuTheme::vizCategory(0).withAlpha(0.8f));
                g.fillRect(juce::Rectangle<float>(x0, py(live_[b]), std::max(1.0f, x1 - x0), plot.getBottom() - py(live_[b])));
            }
            if (b < analysed_.size() && std::isfinite(analysed_[b])) {
                g.setColour(KhensuTheme::vizTrack.withAlpha(0.85f));
                g.drawRect(juce::Rectangle<float>(x0, py(analysed_[b]), std::max(1.0f, x1 - x0), plot.getBottom() - py(analysed_[b])), 1.2f);
            }
        }
    }

    ui::chart::Readout readoutAt(double xv, double) override
    {
        ui::chart::Readout r;
        size_t best = 0;
        for (size_t b = 1; b < width_bands::kBands; ++b)
            if (std::abs(std::log(width_bands::kCentres[b] / xv)) < std::abs(std::log(width_bands::kCentres[best] / xv))) best = b;
        r.title = x.label(width_bands::kCentres[best], true) + " " + TRANS("octave");
        auto pct = [](float w) { return juce::String(juce::roundToInt(w * 100.0f)) + " %"; };
        if (best < analysed_.size() && std::isfinite(analysed_[best]))
            r.lines.push_back({TRANS("Analysed width") + " " + pct(analysed_[best]), KhensuTheme::vizTrack});
        if (best < live_.size() && std::isfinite(live_[best]))
            r.lines.push_back({TRANS("Playing now") + " " + pct(live_[best]), KhensuTheme::vizCategory(0)});
        if (window_)
            r.lines.push_back({TRANS("Genre window") + " " + pct(window_->first) + " - " + pct(window_->second),
                               KhensuTheme::accentTeal});
        return r;
    }

  private:
    std::vector<float> analysed_, live_;
    std::optional<std::pair<float, float>> window_;
};

} // namespace khensu
