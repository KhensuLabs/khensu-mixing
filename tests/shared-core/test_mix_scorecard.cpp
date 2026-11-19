#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <khensu/recommendations/MixScorecard.h>
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/targets/GenreEntry.h>

using namespace khensu;

namespace {

GenreEntry makePopGenre()
{
    GenreEntry g;
    g.id = "pop";
    g.lufs_range_max = -8.0f;
    g.lufs_range_min = -16.0f;
    g.true_peak_max = -1.0f;
    g.lra_range_min = 4.0f;
    g.lra_range_max = 10.0f;
    g.spectral_centroid_hz = 3000.0f;
    g.low_end_weight = 0.35f;
    g.stereo_width_min = 0.4f;
    g.stereo_width_max = 0.8f;
    return g;
}

AnalysisResult makeCleanResult()
{
    AnalysisResult r;
    r.file_id = "test_file";
    r.loudness.lufs_integrated = -12.0f;
    r.loudness.true_peak_dbtp = -1.5f;
    r.loudness.lra = 7.0f;
    r.dynamics.crest_factor_db = 10.0f;
    r.dynamics.dynamic_range_db = 8.0f;
    r.spectral.centroid_hz = 3000.0f;
    r.spectral.balance_low = 0.35f;
    r.stereo.width = 0.6f;
    r.stereo.correlation = 0.7f;
    r.artifacts.has_clipping = false;
    r.artifacts.noise_floor_db = -65.0f;
    return r;
}

} // namespace

TEST_CASE("buildMixScorecard scores a clean mix highly across all fast-analysis categories", "[mix_scorecard]")
{
    auto genre = makePopGenre();
    auto result = makeCleanResult();
    auto sc = buildMixScorecard(result, genre, {});

    CHECK(sc.overall_score > 80.0f);
    CHECK((sc.tier_label == std::string(MixScorecard::kTierHighlySkilled) ||
           sc.tier_label == std::string(MixScorecard::kTierExceptional)));

    // Extended-analysis-only categories (FX & Processing, Frequency Masking)
    // should be marked unavailable when harshness/resonance/mid-side data
    // wasn't computed, and completeness should reflect that.
    bool foundFx = false, foundMasking = false;
    for (const auto& c : sc.categories) {
        if (c.id == "fx_processing") {
            foundFx = true;
            CHECK_FALSE(c.available);
        }
        if (c.id == "frequency_masking") {
            foundMasking = true;
            CHECK_FALSE(c.available);
        }
    }
    CHECK(foundFx);
    CHECK(foundMasking);
    CHECK(sc.completeness < 1.0f);
}

TEST_CASE("buildMixScorecard penalizes clipping and low crest factor in Technical Integrity / Dynamics",
          "[mix_scorecard]")
{
    auto genre = makePopGenre();
    auto result = makeCleanResult();
    result.artifacts.has_clipping = true;
    result.artifacts.clipping_frame_count = 5000;
    result.dynamics.crest_factor_db = 2.0f; // heavily squashed

    auto sc = buildMixScorecard(result, genre, {});

    const ScorecardCategory* technical = nullptr;
    const ScorecardCategory* dynamics = nullptr;
    for (const auto& c : sc.categories) {
        if (c.id == "technical_integrity") technical = &c;
        if (c.id == "dynamics_compression") dynamics = &c;
    }
    REQUIRE(technical != nullptr);
    REQUIRE(dynamics != nullptr);
    CHECK(technical->score <= 50.0f);
    CHECK(dynamics->score <= 60.0f);

    // Sub-category breakdown for Technical Integrity should be present and low on clipping.
    REQUIRE(technical->sub_categories.size() == 2);
    CHECK(technical->sub_categories[0].id == "clipping");
    CHECK(technical->sub_categories[0].score < 30.0f);
}

TEST_CASE("buildMixScorecard tags exactly one Strongest and one Improve category", "[mix_scorecard]")
{
    auto genre = makePopGenre();
    auto result = makeCleanResult();
    result.stereo.correlation = -0.5f; // clear phase issue -> should be the weakest category

    auto sc = buildMixScorecard(result, genre, {});

    int strongestCount = 0, improveCount = 0;
    for (const auto& c : sc.categories) {
        if (c.tag == ScorecardCategory::kTagStrongest) ++strongestCount;
        if (c.tag == ScorecardCategory::kTagImprove) ++improveCount;
    }
    CHECK(strongestCount == 1);
    CHECK(improveCount == 1);
}

TEST_CASE("buildMixScorecard tier bands match the documented ranges", "[mix_scorecard]")
{
    // Bounded readiness labels (safety audit P0, commit 3a6619b) - the tiers
    // describe distance-to-target, not a quality/skill judgement.
    CHECK(std::string(MixScorecard::kTierEarlyStage) == "Rough");
    CHECK(std::string(MixScorecard::kTierDeveloping) == "Early Draft");
    CHECK(std::string(MixScorecard::kTierDemoLevel) == "Needs Work");
    CHECK(std::string(MixScorecard::kTierSkilled) == "Minor Issues");
    CHECK(std::string(MixScorecard::kTierHighlySkilled) == "Near Target");
    CHECK(std::string(MixScorecard::kTierExceptional) == "Meets Targets");
}
