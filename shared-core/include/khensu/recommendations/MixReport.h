#pragma once
#include <khensu/recommendations/Recommendation.h>
#include <map>
#include <string>
#include <vector>

namespace khensu {

struct ConflictPair {
    std::string id_a;
    std::string id_b;
    std::string reason;
};

/// One term of the mastering-readiness penalty formula (see
/// MixReportBuilder's computeReadiness), exposed so the UI can show which
/// single factor is dominating a low score instead of just the final number.
struct ReadinessPenaltyTerm {
    std::string name;    ///< "Headroom", "Crest Factor", "Low-End Balance", "Stereo Correlation", "Critical Issues"
    float points = 0.0f; ///< points deducted from 100 by this term
};

/// The complete output of the recommendation engine for one analysis session.
/// Contains all recommendations, contradiction pairs, readiness score, and no-action evidence.
struct MixReport {
    // --- Identity ---
    std::string file_id;
    std::string genre_id;

    // --- Core output ---
    std::vector<Recommendation> recommendations; ///< Sorted by severity desc, then chain_position asc
    std::vector<std::string> no_action_needed;   ///< Measurement + evidence for dimensions that passed

    // --- Contradiction detection ---
    std::vector<ConflictPair> conflicts; ///< Pairs of recommendations that contradict

    // --- Readiness ---
    float mastering_readiness_score = 0.0f;              ///< 0-100
    std::string readiness_label;                         ///< One of the four kLabel* constants below
    std::vector<ReadinessPenaltyTerm> penalty_breakdown; ///< Which factor(s) are dragging the score down

    // --- Translation risk (per environment, 0-1 where 1 = high risk) ---
    std::map<std::string, float> translation_risk; ///< "earbuds", "phone", "car", "club", "studio"

    // --- Section summary ---
    std::vector<std::string> sections_found; ///< Section labels from Python analysis

    // Exact label strings - used by builder and tests
    static constexpr const char* kLabelReady = "Ready for mastering";
    static constexpr const char* kLabelNearReady = "Near-ready - address critical issues first";
    static constexpr const char* kLabelMixFixes = "Mix fixes needed before mastering";
    static constexpr const char* kLabelMajorIssues = "Major issues - mastering will not fix these";
};

} // namespace khensu
