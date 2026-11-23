// shared-core/include/khensu/recommendations/TonalFixModel.h
// MEAS-4: how much shelf a tonal finding's Fix needs. The low-end and
// centroid findings used to suggest a gain from a fixed slope (share
// difference x 8 dB, centroid difference x 0.0015 dB/Hz) that had no relation
// to the measured spectrum: on released tracks with a +10 dB low shelf added,
// the app's own fix cut 0.5 dB and cleared nothing. Here the shelf's response
// (RBJ, Q 0.707 - what the renderer's Low/HighShelfFilter is) is applied to
// the track's own ten octave bands and the gain that lands the metric on its
// aim is solved for.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace khensu::tonal_fix {

/// Octave-band centres of SpectralData::rms_per_octave_db.
inline constexpr double kBandCentersHz[10] = {31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0};
/// Bands summed into balance_low (SpectralAnalyzer: bands 0-2, below ~177 Hz).
inline constexpr int kLowBands = 3;
inline constexpr double kRefSampleRate = 48000.0;

/// Power gain (linear) of an RBJ shelf (Q 0.707) at `hz`.
inline double shelfPowerGain(double hz, double cornerHz, double gainDb, bool high,
                             double sampleRate = kRefSampleRate)
{
    if (hz >= sampleRate * 0.5) hz = sampleRate * 0.499;
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * 3.141592653589793 * cornerHz / sampleRate;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * 0.7071067811865476);
    const double sq = 2.0 * std::sqrt(A) * alpha;
    double b0, b1, b2, a0, a1, a2;
    if (!high) {
        b0 = A * ((A + 1) - (A - 1) * cw + sq);
        b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - sq);
        a0 = (A + 1) + (A - 1) * cw + sq;
        a1 = -2 * ((A - 1) + (A + 1) * cw);
        a2 = (A + 1) + (A - 1) * cw - sq;
    } else {
        b0 = A * ((A + 1) + (A - 1) * cw + sq);
        b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - sq);
        a0 = (A + 1) - (A - 1) * cw + sq;
        a1 = 2 * ((A - 1) - (A + 1) * cw);
        a2 = (A + 1) - (A - 1) * cw - sq;
    }
    const double w = 2.0 * 3.141592653589793 * hz / sampleRate;
    const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
    const auto h = (b0 + b1 * z1 + b2 * z2) / (a0 + a1 * z1 + a2 * z2);
    return std::norm(h);
}

/// Band powers (linear) from the octave levels; empty when the spectrum was
/// not measured. SpectralAnalyzer's rms_per_octave_db is the MEAN power per
/// FFT bin in each octave (a density), so a band's total power is that times
/// its width - proportional to its centre frequency for octave bands.
inline std::vector<double> bandPowers(const std::vector<float>& octaveDb)
{
    std::vector<double> p;
    if (octaveDb.size() < 10) return p;
    double total = 0.0;
    for (int b = 0; b < 10; ++b) {
        const double v = std::pow(10.0, static_cast<double>(octaveDb[static_cast<size_t>(b)]) / 10.0) *
                         kBandCentersHz[b];
        p.push_back(v);
        total += v;
    }
    if (total <= 1e-12) p.clear();
    return p;
}

inline double lowShare(const std::vector<double>& p)
{
    double low = 0.0, total = 0.0;
    for (size_t b = 0; b < p.size(); ++b) {
        total += p[b];
        if (static_cast<int>(b) < kLowBands) low += p[b];
    }
    return total > 0.0 ? low / total : 0.0;
}

inline double bandCentroid(const std::vector<double>& p)
{
    double num = 0.0, den = 0.0;
    for (size_t b = 0; b < p.size(); ++b) {
        num += kBandCentersHz[b] * p[b];
        den += p[b];
    }
    return den > 0.0 ? num / den : 0.0;
}

inline std::vector<double> withShelf(const std::vector<double>& p, double cornerHz, double gainDb, bool high)
{
    std::vector<double> out(p.size());
    for (size_t b = 0; b < p.size(); ++b)
        out[b] = p[b] * shelfPowerGain(kBandCentersHz[b], cornerHz, gainDb, high);
    return out;
}

/// Bisection for the gain in [lo, hi] dB whose predicted metric reaches
/// `aim`; `metric` must be monotonic in the gain (true for these shelves).
template <typename Metric>
double solveGain(Metric metric, double aim, double lo, double hi)
{
    const double mLo = metric(lo), mHi = metric(hi);
    const bool rising = mHi > mLo;
    if (rising ? aim <= mLo : aim >= mLo) return lo;
    if (rising ? aim >= mHi : aim <= mHi) return hi;
    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (lo + hi);
        const double m = metric(mid);
        if ((m < aim) == rising)
            lo = mid;
        else
            hi = mid;
    }
    return 0.5 * (lo + hi);
}

/// Low-shelf gain (dB) that moves balance_low from `measuredShare` to
/// `aimShare`, or NaN without a measured spectrum. The shelf's effect is
/// predicted from the octave bands as a change in the low:rest odds and
/// applied to the measured share (balance_low comes from the analyzer's
/// filter bank, whose edges differ slightly from the FFT octaves).
inline float lowShelfGainForShare(const std::vector<float>& octaveDb, double measuredShare, double aimShare,
                                  double cornerHz, double minDb, double maxDb)
{
    const auto p = bandPowers(octaveDb);
    if (p.empty() || measuredShare <= 0.0 || measuredShare >= 1.0) return std::nanf("");
    const double base = lowShare(p);
    if (base <= 0.0 || base >= 1.0) return std::nanf("");
    const double baseOdds = base / (1.0 - base), measuredOdds = measuredShare / (1.0 - measuredShare);
    auto predicted = [&](double g) {
        const double m = lowShare(withShelf(p, cornerHz, g, false));
        const double odds = measuredOdds * (m / (1.0 - m)) / baseOdds;
        return odds / (1.0 + odds);
    };
    return static_cast<float>(solveGain(predicted, aimShare, minDb, maxDb));
}

/// High-shelf gain (dB) that moves the spectral centroid to `aimHz`. The
/// octave-band centroid is a coarse stand-in for the measured (per-frame FFT)
/// one, so the change is predicted as a ratio of the band centroid and
/// applied to the measured value. NaN without a measured spectrum.
inline float highShelfGainForCentroid(const std::vector<float>& octaveDb, double measuredHz, double aimHz,
                                      double cornerHz, double minDb, double maxDb)
{
    const auto p = bandPowers(octaveDb);
    if (p.empty() || measuredHz <= 0.0) return std::nanf("");
    const double base = bandCentroid(p);
    if (base <= 0.0) return std::nanf("");
    return static_cast<float>(solveGain(
        [&](double g) { return measuredHz * bandCentroid(withShelf(p, cornerHz, g, true)) / base; }, aimHz, minDb,
        maxDb));
}

/// High-shelf gain (dB) that moves the spectral centroid to `aimHz`, from the
/// averaged FFT bins (SpectralData::fft_mid_magnitudes, dB power per bin) -
/// the same bins, and the same MAGNITUDE weighting, as the measured centroid
/// (SpectralAnalyzer averages each frame's sum(k |X|) / sum(|X|)). The change
/// is predicted as a ratio and applied to the measured value. NaN when the
/// bins or the sample rate are missing.
inline float highShelfGainForCentroidFft(const std::vector<float>& binDb, double sampleRate, double measuredHz,
                                         double aimHz, double cornerHz, double minDb, double maxDb)
{
    if (binDb.size() < 64 || sampleRate <= 0.0 || measuredHz <= 0.0) return std::nanf("");
    const double binHz = sampleRate * 0.5 / static_cast<double>(binDb.size());
    std::vector<double> mag(binDb.size());
    for (size_t k = 0; k < binDb.size(); ++k)
        mag[k] = binDb[k] <= -89.9f ? 0.0 : std::pow(10.0, static_cast<double>(binDb[k]) / 20.0);
    auto centroid = [&](double g) {
        double num = 0.0, den = 0.0;
        for (size_t k = 1; k < mag.size(); ++k) {
            if (mag[k] <= 0.0) continue;
            const double f = static_cast<double>(k) * binHz;
            const double m = mag[k] * std::sqrt(shelfPowerGain(f, cornerHz, g, true, sampleRate));
            num += f * m;
            den += m;
        }
        return den > 0.0 ? num / den : 0.0;
    };
    const double base = centroid(0.0);
    if (base <= 0.0) return std::nanf("");
    return static_cast<float>(
        solveGain([&](double g) { return measuredHz * centroid(g) / base; }, aimHz, minDb, maxDb));
}

/// The spectral tilt of averaged FFT bins (dB power per bin) after a high
/// shelf of `gainDb` at `cornerHz` - SpectralAnalyzer's definition: the
/// least-squares slope of octave-band POWER (dB) against log2 of the band
/// centre, over the ten nominal octaves capped at min(Nyquist, 20 kHz), a
/// partly covered top octave rescaled by its covered fraction. NaN when fewer
/// than two octaves hold energy.
inline double tiltWithShelfFft(const std::vector<double>& binPower, double binHz, double sampleRate,
                               double cornerHz, double gainDb)
{
    const double cap = std::min(sampleRate * 0.5, 20000.0);
    double sumX = 0.0, sumY = 0.0, sumXY = 0.0, sumXX = 0.0;
    int n = 0;
    for (double fc : kBandCentersHz) {
        const double lo = fc / std::sqrt(2.0);
        const double hi = std::min(fc * std::sqrt(2.0), cap);
        if (hi <= lo) continue;
        const double covered = std::log2(hi / lo);
        if (covered < 0.5) continue;
        double energy = 0.0;
        for (size_t k = 0; k < binPower.size(); ++k) {
            const double f = static_cast<double>(k) * binHz;
            if (f < lo) continue;
            if (f >= hi) break;
            if (binPower[k] > 0.0)
                energy += binPower[k] * (gainDb == 0.0 ? 1.0 : shelfPowerGain(f, cornerHz, gainDb, true, sampleRate));
        }
        if (energy <= 1e-30) continue;
        const double x = std::log2(fc);
        const double y = 10.0 * std::log10(energy / covered);
        sumX += x;
        sumY += y;
        sumXY += x * y;
        sumXX += x * x;
        ++n;
    }
    if (n < 2) return std::nan("");
    const double denom = n * sumXX - sumX * sumX;
    if (std::abs(denom) <= 1e-9) return std::nan("");
    return (n * sumXY - sumX * sumY) / denom;
}

/// FIX-A: high-shelf gain (dB) that moves the spectral tilt from
/// `measuredTilt` to `aimTilt` (dB/octave), from the averaged FFT bins
/// (SpectralData::fft_mid_magnitudes) - the bins the measured tilt is fitted
/// on. The change is predicted and added to the measured value. NaN when the
/// bins or the sample rate are missing.
inline float highShelfGainForTiltFft(const std::vector<float>& binDb, double sampleRate, double measuredTilt,
                                     double aimTilt, double cornerHz, double minDb, double maxDb)
{
    if (binDb.size() < 64 || sampleRate <= 0.0) return std::nanf("");
    const double binHz = sampleRate * 0.5 / static_cast<double>(binDb.size());
    std::vector<double> power(binDb.size());
    for (size_t k = 0; k < binDb.size(); ++k)
        power[k] = binDb[k] <= -89.9f ? 0.0 : std::pow(10.0, static_cast<double>(binDb[k]) / 10.0);
    const double base = tiltWithShelfFft(power, binHz, sampleRate, cornerHz, 0.0);
    if (!std::isfinite(base)) return std::nanf("");
    return static_cast<float>(solveGain(
        [&](double g) { return measuredTilt + tiltWithShelfFft(power, binHz, sampleRate, cornerHz, g) - base; },
        aimTilt, minDb, maxDb));
}

/// TUNE-10: low-shelf gain (dB) that moves the low-end share (balance_low,
/// the octave bands below ~177 Hz over bands 0-9) from `measuredShare` to
/// `aimShare`, from the averaged FFT bins. The octave-band model above reads
/// the 150 Hz shelf's slope at the 125 Hz band's centre only, which is where
/// that shelf turns over: on the corpus fix check it undershot the large
/// corrections (every uncleared low_end_light fix still fired, at a median
/// 12 dB of gain). Predicted as an odds ratio applied to the measured share,
/// as lowShelfGainForShare. NaN when the bins or the sample rate are missing.
inline float lowShelfGainForShareFft(const std::vector<float>& binDb, double sampleRate, double measuredShare,
                                     double aimShare, double cornerHz, double minDb, double maxDb)
{
    if (binDb.size() < 64 || sampleRate <= 0.0 || measuredShare <= 0.0 || measuredShare >= 1.0)
        return std::nanf("");
    const double binHz = sampleRate * 0.5 / static_cast<double>(binDb.size());
    const double lowLo = kBandCentersHz[0] / std::sqrt(2.0);
    const double lowHi = kBandCentersHz[kLowBands - 1] * std::sqrt(2.0);
    const double top = std::min(kBandCentersHz[9] * std::sqrt(2.0), sampleRate * 0.5);
    auto share = [&](double g) {
        double low = 0.0, all = 0.0;
        for (size_t k = 0; k < binDb.size(); ++k) {
            const double f = static_cast<double>(k) * binHz;
            if (f < lowLo || binDb[k] <= -89.9f) continue;
            if (f >= top) break;
            const double pw = std::pow(10.0, static_cast<double>(binDb[k]) / 10.0) *
                              (g == 0.0 ? 1.0 : shelfPowerGain(f, cornerHz, g, false, sampleRate));
            all += pw;
            if (f < lowHi) low += pw;
        }
        return all > 0.0 ? low / all : std::nan("");
    };
    const double base = share(0.0);
    if (!(base > 0.0 && base < 1.0)) return std::nanf("");
    const double baseOdds = base / (1.0 - base), measuredOdds = measuredShare / (1.0 - measuredShare);
    auto predicted = [&](double g) {
        const double m = share(g);
        const double odds = measuredOdds * (m / (1.0 - m)) / baseOdds;
        return odds / (1.0 + odds);
    };
    return static_cast<float>(solveGain(predicted, aimShare, minDb, maxDb));
}

} // namespace khensu::tonal_fix
