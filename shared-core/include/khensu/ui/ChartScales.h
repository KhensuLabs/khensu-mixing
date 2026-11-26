// shared-core/include/khensu/ui/ChartScales.h
// VIS-3: the standard axes every chart uses, in one place - so a spectrum on
// the Dashboard, the Issues hero, the plugin and a fix's difference view read
// the same: one frequency axis (20 Hz - 20 kHz, log, the same 1-2-5 tick
// labels), and one dB range per kind of measure:
//   spectrum level     -60 .. 0 dB      (absolute band level)
//   relative level     -18 .. +18 dB    (against a target, mean-normalised)
//   loudness           -36 .. 0 LUFS    (integrated / short-term / momentary)
//   gain reduction     -24 .. 0 dB
//   true peak          -12 .. +3 dBTP
// A chart may narrow a range to its data (autoFit) but never invents its own.
// THREAD_CONTEXT(Any)
#pragma once
#include "ChartKit.h"
#include "VizFrequencyAxis.h" // the frequency axis' one definition (shared with the spectrograms)
#include <algorithm>
#include <cmath>

namespace khensu::ui::chart::scales {

inline constexpr double kFreqLoHz = static_cast<double>(viz::kFreqMinHz);
inline constexpr double kFreqHiHz = static_cast<double>(viz::kFreqMaxHz);
inline constexpr double kSpectrumDbLo = -60.0;
inline constexpr double kSpectrumDbHi = 0.0;
inline constexpr double kRelativeDb = 18.0;
inline constexpr double kLufsLo = -36.0;
inline constexpr double kLufsHi = 0.0;
inline constexpr double kGainReductionLo = -24.0;
inline constexpr double kGainReductionHi = 0.0;
inline constexpr double kTruePeakLo = -12.0;
inline constexpr double kTruePeakHi = 3.0;

/// The one frequency axis: 20 Hz - 20 kHz, log, Hz labels ("50", "1k") -
/// the same axis the spectrograms use (viz::frequencyAxis, VizFrequencyAxis.h).
inline Axis frequency() { return viz::frequencyAxis(); }

/// Absolute spectrum level, -60 .. 0 dB.
inline Axis spectrumDb()
{
    Axis a;
    a.lo = kSpectrumDbLo;
    a.hi = kSpectrumDbHi;
    a.unit = "dB";
    return a;
}

/// Level against a target, symmetric: -range .. +range dB (18 by default).
inline Axis relativeDb(double range = kRelativeDb)
{
    Axis a;
    a.lo = -range;
    a.hi = range;
    a.unit = "dB";
    return a;
}

/// Loudness, -36 .. 0 LUFS.
inline Axis lufs()
{
    Axis a;
    a.lo = kLufsLo;
    a.hi = kLufsHi;
    a.unit = "LUFS";
    return a;
}

/// Gain reduction, -24 .. 0 dB.
inline Axis gainReduction()
{
    Axis a;
    a.lo = kGainReductionLo;
    a.hi = kGainReductionHi;
    a.unit = "dB";
    return a;
}

/// True peak, -12 .. +3 dBTP.
inline Axis truePeak()
{
    Axis a;
    a.lo = kTruePeakLo;
    a.hi = kTruePeakHi;
    a.unit = "dBTP";
    return a;
}

/// Time, 0 .. duration (m:ss labels).
inline Axis time(double durationSec)
{
    Axis a;
    a.lo = 0.0;
    a.hi = std::max(1e-3, durationSec);
    a.format = Format::Time;
    return a;
}

/// A share, 0 .. hi (0..1 = 0..100 %).
inline Axis percent(double hi = 1.0)
{
    Axis a;
    a.lo = 0.0;
    a.hi = hi;
    a.format = Format::Percent;
    return a;
}

/// Categories at 0 .. n-1 (bars, sections); `inverted` lists them top-down.
inline Axis categories(std::vector<juce::String> names, bool inverted = false)
{
    Axis a;
    a.format = Format::Category;
    a.lo = -0.5;
    a.hi = static_cast<double>(names.size()) - 0.5;
    a.categories = std::move(names);
    a.inverted = inverted;
    return a;
}

/// Narrows a standard range to the data: the standard range clipped to
/// [dataLo - step/2, dataHi + step/2] rounded out to `step`, never narrower
/// than `minSpan`; widened past the standard bounds only by data beyond them.
inline Axis autoFit(Axis standard, double dataLo, double dataHi, double step = 6.0, double minSpan = 18.0)
{
    if (!(std::isfinite(dataLo) && std::isfinite(dataHi)) || dataLo > dataHi) return standard;
    double lo = std::floor((dataLo - step * 0.5) / step) * step;
    double hi = std::ceil((dataHi + step * 0.5) / step) * step;
    if (hi - lo < minSpan) {
        const double c = 0.5 * (lo + hi);
        lo = std::floor((c - minSpan * 0.5) / step) * step;
        hi = lo + std::ceil(minSpan / step) * step;
    }
    // Inside the standard range: narrow to the data. Data past it (a -45 LUFS
    // intro) widens the axis rather than being clipped off the plot.
    standard.lo = dataLo < standard.lo ? lo : std::max(standard.lo, lo);
    standard.hi = dataHi > standard.hi ? hi : std::min(standard.hi, hi);
    if (standard.hi - standard.lo < step) standard.hi = standard.lo + step;
    return standard;
}

} // namespace khensu::ui::chart::scales
