#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace khensu::analysis {

/// The one percentile definition shared by the C++ analyzers (audit §8.3):
/// LRA (LoudnessAnalyzer), macro dynamic range (DynamicsAnalyzer) and the
/// noise floor (ArtifactDetector) used three slightly different index rules
/// (one rounded, two truncated).
///
/// Nearest rank on the (n - 1) basis, rounding half away from zero:
/// `sorted[lround(q * (n - 1))]`. The Python service's LRA uses the identical
/// rule (`loudness._nearest_rank`, CORE-12) so both meters agree - keep the
/// two in lock-step.
///
/// `sorted` must be sorted ascending; `q` is in [0, 1]. Returns 0 for an empty
/// input.
inline float percentileSorted(const std::vector<float>& sorted, double q) noexcept
{
    if (sorted.empty()) return 0.0f;
    const double pos = std::clamp(q, 0.0, 1.0) * static_cast<double>(sorted.size() - 1);
    const auto idx = static_cast<std::size_t>(std::lround(pos));
    return sorted[std::min(idx, sorted.size() - 1)];
}

} // namespace khensu::analysis
