// shared-core/include/khensu/analysis/MaskingSummary.h
#pragma once
#include <string>
#include <vector>

namespace khensu {

/// Compact, IPC-agnostic mirror of ipc::StemMaskingPair/StemMaskingResult -
/// duplicated here rather than reused directly so AnalysisResult (a plain
/// analysis::* data struct) doesn't take a dependency on the ipc:: layer.
/// Populated on AppController's current AnalysisResult once the user runs
/// "Analyze Masking" in StemMaskingPanel, so MaskingRecommender can turn
/// collision pairs into first-class Recommendations instead of leaving the
/// detector display-only.
struct MaskingPairSummary {
    std::string stem_a;
    std::string stem_b;
    float collision_score = 0.0f;
    std::vector<float> band_scores;
};

struct MaskingSummary {
    std::vector<MaskingPairSummary> pairs;
    std::vector<float> bands_hz; // lower edge of each band, Hz - same order as band_scores
};

} // namespace khensu
