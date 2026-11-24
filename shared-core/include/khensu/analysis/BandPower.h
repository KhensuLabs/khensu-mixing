// shared-core/include/khensu/analysis/BandPower.h
// The long-term spectrum as BAND POWER per 1/6 octave (the tilt convention:
// pink noise is flat, AUDIT-009), normalised so its least-squares line passes
// 0 dB at 1 kHz. One definition for the hero spectrum (desktop), the corpus
// measurement behind the genre spread (VIZ-1a, tests/shared-core/
// test_corpus_measure.cpp) and the plugin, so a track is drawn against
// released tracks measured the same way.
// THREAD_CONTEXT(Any) - plain functions.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace khensu::band_power {

struct Point {
    float hz = 0.0f;
    float db = 0.0f;
};

/// 1/6-octave band centres, 25 Hz - 18 kHz (the spread's grid: index i is
/// 25 * 2^(i/6) Hz).
inline std::vector<float> bandCentres(int perOctave = 6)
{
    std::vector<float> out;
    for (float hz = 25.0f; hz <= 18000.0f; hz *= std::pow(2.0f, 1.0f / static_cast<float>(perOctave)))
        out.push_back(hz);
    return out;
}

/// Band power (dB, un-normalised) per 1/6 octave from the analyser's per-bin
/// power spectrum (`fftDb`: dB power per bin, bins 0..N-1 of a 2N-point FFT).
/// A band too narrow to hold a bin takes its nearest bin's density.
inline std::vector<Point> fromFft(const std::vector<float>& fftDb, double sampleRate)
{
    std::vector<Point> out;
    const int n = static_cast<int>(fftDb.size());
    if (n < 16 || sampleRate <= 0.0) return out;
    const double binHz = sampleRate / (2.0 * n);
    const double half = std::pow(2.0, 1.0 / 12.0);
    for (const float fc : bandCentres()) {
        if (fc >= sampleRate * 0.5) break;
        const int lo = std::clamp(static_cast<int>(std::ceil(fc / half / binHz)), 1, n - 1);
        const int hi = std::clamp(static_cast<int>(std::floor(fc * half / binHz)), 1, n - 1);
        double sum = 0.0;
        int count = 0;
        for (int k = lo; k <= hi; ++k) {
            sum += std::pow(10.0, fftDb[static_cast<size_t>(k)] / 10.0);
            ++count;
        }
        if (count == 0) { // narrower than a bin: the nearest bin's density
            const int k = std::clamp(static_cast<int>(std::lround(fc / binHz)), 1, n - 1);
            sum = std::pow(10.0, fftDb[static_cast<size_t>(k)] / 10.0);
            count = 1;
        }
        const double density = sum / count;
        if (density <= 1e-12) continue;
        // density x bandwidth: a proportional band's width grows with fc.
        out.push_back({fc, static_cast<float>(10.0 * std::log10(density) + 10.0 * std::log10(static_cast<double>(fc)))});
    }
    return out;
}

/// The same from the ten octave levels (SpectralData::rms_per_octave_db, a
/// density per octave), when the full spectrum is missing.
inline std::vector<Point> fromOctaves(const std::vector<float>& octaveDb)
{
    static constexpr float kCentres[10] = {31.5f, 63.0f, 125.0f, 250.0f, 500.0f,
                                           1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f};
    std::vector<Point> out;
    if (octaveDb.size() < 10) return out;
    for (size_t i = 0; i < 10; ++i)
        if (octaveDb[i] > -89.0f) out.push_back({kCentres[i], octaveDb[i] + 10.0f * std::log10(kCentres[i])});
    return out;
}

/// Shifts `points` so their least-squares line in log2(hz / 1 kHz) passes
/// 0 dB at 1 kHz; returns the shift added.
inline float normaliseAt1k(std::vector<Point>& points)
{
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int n = 0;
    for (const auto& p : points) {
        if (p.hz < 31.0f || p.hz > 16500.0f) continue;
        const double x = std::log2(p.hz / 1000.0);
        sx += x;
        sy += p.db;
        sxx += x * x;
        sxy += x * p.db;
        ++n;
    }
    if (n < 2) return 0.0f;
    const double denom = n * sxx - sx * sx;
    const double slope = std::abs(denom) > 1e-9 ? (n * sxy - sx * sy) / denom : 0.0;
    const double intercept = (sy - slope * sx) / n;
    const float shift = static_cast<float>(-intercept);
    for (auto& p : points)
        p.db += shift;
    return shift;
}

/// Linear interpolation of `points` (sorted by hz) at `hz` in log frequency;
/// clamps at the ends. Empty -> 0.
inline float at(const std::vector<Point>& points, float hz)
{
    if (points.empty()) return 0.0f;
    if (hz <= points.front().hz) return points.front().db;
    if (hz >= points.back().hz) return points.back().db;
    for (size_t i = 1; i < points.size(); ++i)
        if (hz <= points[i].hz) {
            const float a = std::log2(points[i - 1].hz), b = std::log2(points[i].hz);
            const float t = b > a ? (std::log2(hz) - a) / (b - a) : 0.0f;
            return points[i - 1].db + t * (points[i].db - points[i - 1].db);
        }
    return points.back().db;
}

} // namespace khensu::band_power
