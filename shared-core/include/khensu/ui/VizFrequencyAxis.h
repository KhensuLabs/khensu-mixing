// shared-core/include/khensu/ui/VizFrequencyAxis.h
// VIS-3: one frequency axis for every spectrum, spectrogram and heat map in
// the desktop app and the plugin - 20 Hz to 20 kHz on a log scale, with the
// same 1-2-5 ticks and the same labels ChartKit writes ("20", "50", "100",
// "1k", "20k"). A view whose data covers less (a measurement of 30 Hz -
// 16 kHz) keeps this axis and leaves the unmeasured strip visibly empty,
// so two views of the same song line up.
//
//   const float y = plot.getBottom() - viz::freqNorm(hz) * plot.getHeight();
//   for (float hz : viz::kFreqTicksHz) g.drawText(viz::freqTickLabel(hz), ...);
//
// THREAD_CONTEXT(Any)
#pragma once
#include <khensu/ui/ChartKit.h>
#include <array>
#include <cmath>

namespace khensu::ui::viz {

inline constexpr float kFreqMinHz = 20.0f;
inline constexpr float kFreqMaxHz = 20000.0f;

/// The labelled ticks (1-2-5 per decade, as ChartKit's log axis draws them).
inline constexpr std::array<float, 10> kFreqTicksHz{20.0f,   50.0f,   100.0f,  200.0f,   500.0f,
                                                    1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f};

/// The standard axis as a ChartKit Axis (a ChartView sets `x = viz::frequencyAxis();`).
inline chart::Axis frequencyAxis()
{
    chart::Axis a;
    a.lo = kFreqMinHz;
    a.hi = kFreqMaxHz;
    a.scale = chart::Scale::Log;
    a.format = chart::Format::Hz;
    return a;
}

/// 0 at kFreqMinHz, 1 at kFreqMaxHz (log); values outside clamp.
inline float freqNorm(float hz) noexcept
{
    const float h = hz > kFreqMinHz ? (hz < kFreqMaxHz ? hz : kFreqMaxHz) : kFreqMinHz;
    return std::log(h / kFreqMinHz) / std::log(kFreqMaxHz / kFreqMinHz);
}

/// The frequency at t in [0, 1] along the axis.
inline float freqAt(float t) noexcept
{
    const float c = t > 0.0f ? (t < 1.0f ? t : 1.0f) : 0.0f;
    return kFreqMinHz * std::pow(kFreqMaxHz / kFreqMinHz, c);
}

/// A tick label, written exactly as ChartKit writes its Hz ticks.
inline juce::String freqTickLabel(float hz) { return frequencyAxis().label(static_cast<double>(hz), false); }

/// A readout ("1.25 kHz", "62.5 Hz"), as ChartKit's tooltip writes it.
inline juce::String freqReadout(float hz) { return frequencyAxis().label(static_cast<double>(hz), true); }

} // namespace khensu::ui::viz
