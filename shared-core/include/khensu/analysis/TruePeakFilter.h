#pragma once
// MEAS-5: the one true-peak interpolator. The desktop LoudnessAnalyzer and the
// native meters (NativeMeters.h) both use it, and it is the filter the Python
// service's analysis/loudness.py uses: scipy.signal.resample_poly at 4x with
// firwin(257, 0.25, window=("kaiser", 9.0)). Change it here and in loudness.py
// together; tests/shared-core/test_native_meters_parity.cpp pins the two.
// THREAD_CONTEXT(Any) - immutable after first use.

#include <cmath>

namespace khensu::analysis {

inline constexpr int kTruePeakOversample = 4;
/// Input samples either side of the output position.
inline constexpr int kTruePeakHalf = 32;
/// Taps per polyphase phase (2 * kTruePeakHalf + 1).
inline constexpr int kTruePeakPhaseTaps = 2 * kTruePeakHalf + 1;

/// The silence floor every meter reports, in dBTP / dBFS: 20 log10(1e-12),
/// the same epsilon as the Python service (MEAS-5).
inline constexpr double kPeakFloorDb = -240.0;

/// Linear peak to dB with the shared floor.
inline double peakToDb(double linear) noexcept
{
    return 20.0 * std::log10(linear + 1e-12);
}

struct TruePeakPhases {
    /// w[p][j]: y[4n + p] = sum_j w[p][j] * x[n - kTruePeakHalf + j], so a
    /// window of x read oldest..newest gives the output kTruePeakHalf samples
    /// behind the newest one.
    double w[kTruePeakOversample][kTruePeakPhaseTaps];
};

/// Built once on first use; safe to call from any thread after that.
const TruePeakPhases& truePeakPhases() noexcept;

} // namespace khensu::analysis
