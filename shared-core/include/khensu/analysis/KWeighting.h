#pragma once
#include <array>

namespace khensu::analysis {

/// The two ITU-R BS.1770-4 K-weighting biquads at one sample rate, each as
/// {b0, b1, b2, a1, a2} with a0 normalised to 1.
struct KWeightingCoefficients {
    std::array<double, 5> preFilter; ///< stage 1: high shelf
    std::array<double, 5> rlbFilter; ///< stage 2: RLB high-pass
};

/// The K-weighting used by every C++ loudness meter that links shared-core:
/// the desktop LoudnessAnalyzer, the native meters and the plugin's live
/// telemetry (FIX-11, MEAS-5). At 48 kHz it is the ITU table verbatim, like
/// the Python service's loudness.py.
/// Change the filter here and nowhere else, so the two cannot read the same
/// audio differently.
///
/// Plain arithmetic: no allocation and no locks. Call it when the sample rate
/// is set (reset / prepareToPlay), not per block.
KWeightingCoefficients kWeightingCoefficients(double sampleRate) noexcept;

} // namespace khensu::analysis
