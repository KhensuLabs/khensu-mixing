#include <catch2/catch_test_macros.hpp>
#include <khensu/recommendations/MixReport.h>

using namespace khensu;

TEST_CASE("MixReport default construction is safe", "[mix_report]")
{
    MixReport r;
    CHECK(r.file_id.empty());
    CHECK(r.genre_id.empty());
    CHECK(r.recommendations.empty());
    CHECK(r.no_action_needed.empty());
    CHECK(r.conflicts.empty());
    CHECK(r.mastering_readiness_score == 0.0f);
    CHECK(r.readiness_label.empty());
    CHECK(r.translation_risk.empty());
    CHECK(r.sections_found.empty());
}

TEST_CASE("ConflictPair fields accessible", "[mix_report]")
{
    ConflictPair cp;
    cp.id_a = "lufs_too_loud_file1";
    cp.id_b = "lufs_too_quiet_file1";
    cp.reason = "Mutually exclusive loudness direction";
    CHECK(cp.id_a == "lufs_too_loud_file1");
    CHECK(cp.id_b == "lufs_too_quiet_file1");
    CHECK(cp.reason == "Mutually exclusive loudness direction");
}

TEST_CASE("MixReport readiness_label enum exhaustiveness", "[mix_report]")
{
    // Verify all four label constants match the exact strings the builder will use
    CHECK(std::string(MixReport::kLabelReady) == "Ready for mastering");
    CHECK(std::string(MixReport::kLabelNearReady) == "Near-ready - address critical issues first");
    CHECK(std::string(MixReport::kLabelMixFixes) == "Mix fixes needed before mastering");
    CHECK(std::string(MixReport::kLabelMajorIssues) == "Major issues - mastering will not fix these");
}

// ---------------------------------------------------------------------------
// Builder tests (Task 2 - SP-16B)
// ---------------------------------------------------------------------------
#include <khensu/recommendations/MixReportBuilder.h>
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/targets/GenreEntry.h>
#include <khensu/targets/CreativeTarget.h>
#include <khensu/recommendations/ProcessingStage.h>

// Helper: build a minimal GenreEntry for pop
static GenreEntry makePopGenre()
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
    return g;
}

// Helper: build a clean analysis result (all within genre targets)
static AnalysisResult makeCleanResult()
{
    AnalysisResult r;
    r.file_id = "test_file";
    r.loudness.lufs_integrated = -14.0f;
    r.loudness.true_peak_dbtp = -1.5f;
    r.loudness.lra = 7.0f;
    r.dynamics.crest_factor_db = 12.0f;
    r.dynamics.dynamic_range_db = 12.0f;
    r.spectral.centroid_hz = 3000.0f;
    r.spectral.balance_low = 0.35f;
    r.stereo.correlation = 0.7f;
    return r;
}

TEST_CASE("buildMixReport returns high readiness for clean mix", "[mix_report][builder]")
{
    auto genre = makePopGenre();
    auto result = makeCleanResult();
    auto report = khensu::buildMixReport({}, result, genre, {});
    CHECK(report.mastering_readiness_score >= 80.0f);
    CHECK(report.readiness_label == std::string(MixReport::kLabelReady));
}

TEST_CASE("buildMixReport marks clipping as low readiness", "[mix_report][builder]")
{
    auto genre = makePopGenre();
    auto result = makeCleanResult();
    result.loudness.true_peak_dbtp = 2.0f;  // clipping
    result.dynamics.crest_factor_db = 2.0f; // crushed
    result.stereo.correlation = 0.1f;       // phase trouble

    // Make some critical recs to push score down
    std::vector<Recommendation> recs;
    Recommendation r;
    r.id = "clipping_detected_test_file";
    r.stage = ProcessingStage::MixChannel;
    r.severity = 90.0f;
    r.confidence = 0.95f;
    r.what = r.why_helps = r.suggested_action = r.engineer_note = "x";
    recs.push_back(r);

    auto report = khensu::buildMixReport(recs, result, genre, {});
    CHECK(report.mastering_readiness_score < 60.0f);
    CHECK(report.readiness_label == std::string(MixReport::kLabelMajorIssues));
}

TEST_CASE("buildMixReport detects mutually exclusive conflicts", "[mix_report][builder]")
{
    // Inject two recs whose codes are known to conflict
    std::vector<Recommendation> recs;

    Recommendation a;
    a.id = "lra_too_compressed_test_file";
    a.stage = ProcessingStage::MixBus;
    a.chain_position = 5;
    a.severity = 60.0f;
    a.confidence = 0.8f;
    a.what = a.why_helps = a.suggested_action = a.engineer_note = "x";
    recs.push_back(a);

    Recommendation b;
    b.id = "over_compressed_test_file";
    b.stage = ProcessingStage::MixBus;
    b.chain_position = 5;
    b.severity = 70.0f;
    b.confidence = 0.8f;
    b.what = b.why_helps = b.suggested_action = b.engineer_note = "x";
    recs.push_back(b);

    auto report = khensu::buildMixReport(recs, makeCleanResult(), makePopGenre(), {});
    // lra_too_compressed and over_compressed conflict (addressing one affects the other)
    REQUIRE_FALSE(report.conflicts.empty());
    bool found = false;
    for (const auto& cp : report.conflicts)
        if ((cp.id_a == a.id && cp.id_b == b.id) || (cp.id_a == b.id && cp.id_b == a.id)) found = true;
    CHECK(found);
    // Also check that conflicts_with is populated on the recs
    bool a_has_conflict = false, b_has_conflict = false;
    for (const auto& rec : report.recommendations) {
        if (rec.id == a.id && !rec.conflicts_with.empty()) a_has_conflict = true;
        if (rec.id == b.id && !rec.conflicts_with.empty()) b_has_conflict = true;
    }
    CHECK(a_has_conflict);
    CHECK(b_has_conflict);
}

TEST_CASE("buildMixReport applies confidence_threshold filtering", "[mix_report][builder]")
{
    std::vector<Recommendation> recs;
    Recommendation r;
    r.id = "low_confidence_rec";
    r.severity = 80.0f;
    r.confidence = 0.40f; // below threshold
    r.stage = ProcessingStage::Master;
    r.what = r.why_helps = r.suggested_action = r.engineer_note = "x";
    recs.push_back(r);

    CreativeTarget target;
    target.confidence_threshold = 0.50f; // filter out anything below 0.5

    auto report = khensu::buildMixReport(recs, makeCleanResult(), makePopGenre(), target);
    for (const auto& rec : report.recommendations)
        CHECK(rec.confidence >= 0.50f);
}

TEST_CASE("buildMixReport sorts by severity desc then chain_position asc", "[mix_report][builder]")
{
    std::vector<Recommendation> recs;
    auto mkRec = [](const char* id, float sev, int chain, ProcessingStage stg) {
        Recommendation r;
        r.id = id;
        r.severity = sev;
        r.chain_position = chain;
        r.stage = stg;
        r.confidence = 0.8f;
        r.what = r.why_helps = r.suggested_action = r.engineer_note = "x";
        return r;
    };
    recs.push_back(mkRec("a", 50.0f, 3, ProcessingStage::MixBus));
    recs.push_back(mkRec("b", 80.0f, 1, ProcessingStage::MixChannel));
    recs.push_back(mkRec("c", 80.0f, 5, ProcessingStage::Master));

    auto report = khensu::buildMixReport(recs, makeCleanResult(), makePopGenre(), {});
    REQUIRE(report.recommendations.size() == 3);
    // b and c both have severity 80; b has chain_position 1, c has 5 - b should come first
    CHECK(report.recommendations[0].id == "b");
    CHECK(report.recommendations[1].id == "c");
    CHECK(report.recommendations[2].id == "a");
}

TEST_CASE("buildMixReport populates no_action_needed for passing dimensions", "[mix_report][builder]")
{
    auto report = khensu::buildMixReport({}, makeCleanResult(), makePopGenre(), {});
    REQUIRE_FALSE(report.no_action_needed.empty());
    // Should contain evidence for at least LUFS (since clean result is within range)
    bool found_lufs = false;
    for (const auto& s : report.no_action_needed)
        if (s.find("LUFS") != std::string::npos) found_lufs = true;
    CHECK(found_lufs);
}

TEST_CASE("buildMixReport NearReady label for score in range 70 to 85", "[mix_report][builder]")
{
    // Penalty budget (all fixes applied):
    //   Headroom    : min(25, (-0.3+0.5)*25)                    =  5.0
    //   Crest       : 20 - min(20, (13.5-6)*2)                  =  5.0
    //   Low-end     : min(20, |0.35-0.35|*50)                    =  0.0
    //   Stereo      : 0  (correlation 0.7 >= 0.5)                =  0.0
    //   Crit recs   : 2 x MixChannel sev70 (>= threshold 60)    = 10.0
    //   Total = 20 → score = 80 → NearReady
    auto genre = makePopGenre();
    auto result = makeCleanResult();
    result.loudness.true_peak_dbtp = -0.3f;
    result.dynamics.crest_factor_db = 13.5f;
    result.spectral.balance_low = 0.35f; // matches genre.low_end_weight
    result.stereo.correlation = 0.7f;

    auto mkRec = [](const char* id) {
        Recommendation r;
        r.id = id;
        r.stage = ProcessingStage::MixChannel;
        r.chain_position = 7;
        r.severity = 70.0f;
        r.confidence = 0.80f;
        r.is_taste_decision = false;
        r.what = r.why_helps = r.suggested_action = r.engineer_note = "x";
        return r;
    };
    std::vector<Recommendation> recs;
    recs.push_back(mkRec("hook_strength_low_file_1"));
    recs.push_back(mkRec("hook_strength_low_file_2"));

    auto report = khensu::buildMixReport(recs, result, genre, {});
    CHECK(report.mastering_readiness_score >= 70.0f);
    CHECK(report.mastering_readiness_score < 85.0f);
    CHECK(report.readiness_label == std::string(MixReport::kLabelNearReady));
}

TEST_CASE("buildMixReport MixFixes label for score in range 50 to 70", "[mix_report][builder]")
{
    // Penalty budget (all fixes applied):
    //   Headroom    : min(25, (0.0+0.5)*25)           = 12.5
    //   Crest       : 20 - min(20, (12.0-6)*2)        =  8.0
    //   Low-end     : min(20, |0.47-0.35|*50)          =  6.0
    //   Stereo      : min(15, (0.5-0.3)*30)            =  6.0
    //   Crit recs   : none                             =  0.0
    //   Total = 32.5 → score = 67.5 → MixFixes
    auto genre = makePopGenre();
    auto result = makeCleanResult();
    result.loudness.true_peak_dbtp = 0.0f;
    result.dynamics.crest_factor_db = 12.0f;
    result.spectral.balance_low = 0.47f;
    result.stereo.correlation = 0.3f;

    auto report = khensu::buildMixReport({}, result, genre, {});
    CHECK(report.mastering_readiness_score >= 50.0f);
    CHECK(report.mastering_readiness_score < 70.0f);
    CHECK(report.readiness_label == std::string(MixReport::kLabelMixFixes));
}
