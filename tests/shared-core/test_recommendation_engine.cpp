// tests/shared-core/test_recommendation_engine.cpp
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <khensu/recommendations/RecommendationEngine.h>
#include <khensu/recommendations/MixReport.h>
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/targets/GenreEntry.h>
#include <khensu/targets/CreativeTarget.h>

using namespace khensu;

TEST_CASE("RecommendationEngine flags loudness over genre max", "[recengine]")
{
    AnalysisResult r;
    r.file_id = "f1";
    r.loudness.lufs_integrated = -6.0f; // very hot
    r.loudness.true_peak_dbtp = 0.5f;   // over 0 dBTP

    GenreEntry genre;
    genre.id = "trap";
    genre.lufs_range_max = -8.0f;
    genre.lufs_range_min = -16.0f;
    genre.true_peak_max = -1.0f;

    CreativeTarget target;

    RecommendationEngine engine;
    auto recs = engine.generate(r, genre, target);

    REQUIRE_FALSE(recs.empty());
    // Sorted by severity descending
    for (size_t i = 1; i < recs.size(); ++i)
        CHECK(recs[i - 1].severity >= recs[i].severity);
}

TEST_CASE("RecommendationEngine on a clean result yields few/no recommendations", "[recengine]")
{
    AnalysisResult r;
    r.file_id = "f2";
    r.loudness.lufs_integrated = -12.0f;
    r.loudness.true_peak_dbtp = -1.5f;
    r.loudness.lra = 7.0f;

    GenreEntry genre;
    genre.id = "pop";
    genre.lufs_range_max = -8.0f;
    genre.lufs_range_min = -16.0f;
    genre.true_peak_max = -1.0f;
    genre.lra_range_min = 4.0f;
    genre.lra_range_max = 10.0f;

    CreativeTarget target;

    RecommendationEngine engine;
    auto recs = engine.generate(r, genre, target);
    // No assertion on exact count (depends on all recommenders); just must not crash
    // and must be sorted.
    for (size_t i = 1; i < recs.size(); ++i)
        CHECK(recs[i - 1].severity >= recs[i].severity);
}

TEST_CASE("RecommendationEngine::generateReport returns MixReport", "[engine][report]")
{
    AnalysisResult result;
    result.file_id = "test_gen_report";
    result.loudness.lufs_integrated = -6.0f; // too loud for pop → triggers rec
    result.loudness.true_peak_dbtp = -1.5f;
    result.loudness.lra = 7.0f;
    result.dynamics.crest_factor_db = 10.0f;
    result.dynamics.dynamic_range_db = 12.0f;
    result.spectral.centroid_hz = 3000.0f;
    result.spectral.balance_low = 0.35f;
    result.stereo.correlation = 0.7f;

    GenreEntry genre;
    genre.id = "pop";
    genre.lufs_range_max = -8.0f;
    genre.lufs_range_min = -16.0f;
    genre.true_peak_max = -1.0f;
    genre.lra_range_min = 4.0f;
    genre.lra_range_max = 10.0f;
    genre.spectral_centroid_hz = 3000.0f;
    genre.low_end_weight = 0.35f;

    RecommendationEngine engine;
    auto report = engine.generateReport(result, genre, {});

    CHECK_FALSE(report.file_id.empty());
    CHECK_FALSE(report.readiness_label.empty());
    CHECK(report.mastering_readiness_score >= 0.0f);
    CHECK(report.mastering_readiness_score <= 100.0f);
    CHECK_FALSE(report.recommendations.empty()); // lufs_too_loud should fire
}

TEST_CASE("RecommendationEngine::generate still works after generateReport added", "[engine][backwards]")
{
    AnalysisResult result;
    result.file_id = "test_backwards";
    result.loudness.lufs_integrated = -6.0f;
    result.loudness.true_peak_dbtp = -1.5f;
    result.loudness.lra = 7.0f;
    result.dynamics.crest_factor_db = 10.0f;
    result.dynamics.dynamic_range_db = 12.0f;
    result.spectral.centroid_hz = 3000.0f;
    result.spectral.balance_low = 0.35f;
    result.stereo.correlation = 0.7f;

    GenreEntry genre;
    genre.id = "pop";
    genre.lufs_range_max = -8.0f;
    genre.lufs_range_min = -16.0f;
    genre.true_peak_max = -1.0f;
    genre.lra_range_min = 4.0f;
    genre.lra_range_max = 10.0f;
    genre.spectral_centroid_hz = 3000.0f;
    genre.low_end_weight = 0.35f;

    RecommendationEngine engine;
    auto recs = engine.generate(result, genre, {});
    CHECK_FALSE(recs.empty());
}

TEST_CASE("RecommendationEngine fires harshness rec when harshness data present", "[engine][extended]")
{
    AnalysisResult result;
    result.file_id = "f_engine_harsh";
    // Populate enough to not fire most existing recommenders
    result.loudness.lufs_integrated = -12.0f;
    result.loudness.true_peak_dbtp = -1.5f;
    result.loudness.lra = 7.0f;
    result.dynamics.crest_factor_db = 10.0f;
    result.dynamics.dynamic_range_db = 12.0f;
    result.spectral.centroid_hz = 3000.0f;
    result.spectral.balance_low = 0.35f;
    result.stereo.correlation = 0.7f;

    // Set harshness data (optional)
    AnalysisResult::HarshnessData harsh_data;
    harsh_data.score = 0.7f; // high harshness - should fire
    result.harshness = harsh_data;

    GenreEntry genre;
    genre.id = "pop";
    genre.lufs_range_max = -8.0f;
    genre.lufs_range_min = -16.0f;
    genre.true_peak_max = -1.0f;
    genre.lra_range_min = 4.0f;
    genre.lra_range_max = 10.0f;
    genre.spectral_centroid_hz = 3000.0f;
    genre.low_end_weight = 0.35f;

    RecommendationEngine engine;
    auto recs = engine.generate(result, genre, {});
    bool found_harsh = false;
    for (const auto& r : recs)
        if (r.id.find("harshness_detected") != std::string::npos) found_harsh = true;
    CHECK(found_harsh);
}

// --- Measurement-trust down-weighting (pipeline #5 trust map) ---------------

namespace {

AnalysisResult hotResult(const char* fileId)
{
    AnalysisResult r;
    r.file_id = fileId;
    r.loudness.lufs_integrated = -6.0f; // hot vs the genre below -> lufs_too_loud
    r.loudness.true_peak_dbtp = -1.5f;
    return r;
}

GenreEntry popGenre()
{
    GenreEntry g;
    g.id = "pop";
    g.lufs_range_max = -8.0f;
    g.lufs_range_min = -16.0f;
    g.true_peak_max = -1.0f;
    return g;
}

const Recommendation* findByPrefix(const std::vector<Recommendation>& recs, const std::string& prefix)
{
    for (const auto& r : recs)
        if (r.id.rfind(prefix, 0) == 0) return &r;
    return nullptr;
}

} // namespace

TEST_CASE("Measurement trust down-weights recommendations built on shaky measurements", "[recengine][trust]")
{
    const GenreEntry genre = popGenre();
    const CreativeTarget target;
    RecommendationEngine engine;

    // Baseline: no trust map -> untouched severity.
    auto baseline = engine.generate(hotResult("t0"), genre, target);
    const auto* baseRec = findByPrefix(baseline, "lufs_too_loud");
    REQUIRE(baseRec != nullptr);
    REQUIRE(baseRec->severity > 0.0f);
    CHECK(baseRec->abstained_reason.empty());

    SECTION("unreliable halves severity and sets an abstention note")
    {
        auto r = hotResult("t1");
        r.measurement_trust["loudness_lufs"] = {0.3f, "unreliable", "irregular content"};
        auto recs = engine.generate(r, genre, target);
        const auto* rec = findByPrefix(recs, "lufs_too_loud");
        REQUIRE(rec != nullptr);
        CHECK(rec->severity == baseRec->severity * 0.5f);
        CHECK(rec->measurement_confidence == 0.3f);
        CHECK(rec->abstained_reason.find("unreliable") != std::string::npos);
        CHECK(rec->confidence_reason.find("Down-weighted") != std::string::npos);
    }

    SECTION("tentative scales severity by 0.8 and does NOT abstain")
    {
        auto r = hotResult("t2");
        r.measurement_trust["loudness_lufs"] = {0.6f, "tentative", "(short excerpt)"};
        auto recs = engine.generate(r, genre, target);
        const auto* rec = findByPrefix(recs, "lufs_too_loud");
        REQUIRE(rec != nullptr);
        CHECK(rec->severity == baseRec->severity * 0.8f);
        CHECK(rec->measurement_confidence == 0.6f);
        CHECK(rec->abstained_reason.empty());
    }

    SECTION("reliable leaves the recommendation untouched")
    {
        auto r = hotResult("t3");
        r.measurement_trust["loudness_lufs"] = {0.95f, "reliable", ""};
        auto recs = engine.generate(r, genre, target);
        const auto* rec = findByPrefix(recs, "lufs_too_loud");
        REQUIRE(rec != nullptr);
        CHECK(rec->severity == baseRec->severity);
        CHECK(rec->measurement_confidence == baseRec->measurement_confidence);
        CHECK(rec->abstained_reason.empty());
    }

    SECTION("trust never RAISES measurement_confidence above the recommender's own claim")
    {
        auto r = hotResult("t4");
        r.measurement_trust["loudness_lufs"] = {0.99f, "tentative", ""};
        auto recs = engine.generate(r, genre, target);
        const auto* rec = findByPrefix(recs, "lufs_too_loud");
        REQUIRE(rec != nullptr);
        CHECK(rec->measurement_confidence <= baseRec->measurement_confidence);
    }

    SECTION("metrics without a binding pass through untouched")
    {
        auto r = hotResult("t5");
        r.measurement_trust["bpm"] = {0.2f, "unreliable", "rubato"};
        auto recs = engine.generate(r, genre, target);
        const auto* rec = findByPrefix(recs, "lufs_too_loud");
        REQUIRE(rec != nullptr);
        CHECK(rec->severity == baseRec->severity);
    }

    SECTION("down-weighting happens before the severity sort")
    {
        auto r = hotResult("t6");
        r.measurement_trust["loudness_lufs"] = {0.3f, "unreliable", ""};
        auto recs = engine.generate(r, genre, target);
        for (size_t i = 1; i < recs.size(); ++i)
            CHECK(recs[i - 1].severity >= recs[i].severity);
    }
}

// ---- REF-1: every finding names the optional inputs it was derived from ----
namespace {
// A result that has every optional input and trips a finding on most of them.
AnalysisResult richResult()
{
    AnalysisResult r;
    r.file_id = "rich";
    r.loudness.lufs_integrated = -4.0f; // far over any target: fast-pass findings too
    r.loudness.true_peak_dbtp = 0.8f;
    r.dynamics.crest_factor_db = 3.0f;

    r.python_analysis_ran = true;
    r.python_data.bpm_detected = 120.0f;
    r.python_data.hooks.hook_count = 0;
    r.python_data.hooks.hook_density = 0.05f;
    r.python_data.hooks.hook_strength = 0.10f;

    r.harshness = AnalysisResult::HarshnessData{};
    r.harshness->score = 0.8f;
    r.harshness->peak_hz = 3500.0f;

    r.mono_compat = AnalysisResult::MonoCompatData{};
    r.mono_compat->low_end_stereo_pct = 50.0f;
    r.mono_compat->per_band_correlation["midrange"] = 0.0f; // comb-filtered mids and highs (TUNE-3)
    r.mono_compat->per_band_correlation["upper_mid"] = 0.0f;
    r.mono_compat->per_band_correlation["air"] = 0.0f;

    r.translation_risk = AnalysisResult::TranslationRiskData{};
    r.translation_risk->per_environment["earbuds"] = 0.8f;

    r.resonance = AnalysisResult::ResonanceData{};
    r.resonance->hotspots.push_back({800.0f, 0.0f, 14.0f});
    r.resonance->hotspots.push_back({2000.0f, 0.0f, 12.0f}); // in the vocal presence range

    r.mid_side_imbalance = AnalysisResult::MidSideImbalanceData{};

    r.drums = AnalysisResult::DrumData{};
    r.drums->kick_bass_clash = true;
    r.drums->kick_bass_proximity_hz = 3.0f; // inside the 4 Hz collision edge (TUNE-8)

    r.section_analysis = SectionAnalysis{};
    r.section_analysis->loudness_arc.section_count = 4;
    r.section_analysis->loudness_arc.chorus_verse_delta_lu = 0.5f; // a flat arc
    for (const float lufs : {-10.0f, -10.4f, -10.2f}) { // within 1 LU: section_arc_too_flat
        SectionData s;
        s.label = "S";
        s.lufs_integrated = lufs;
        r.section_analysis->sections.push_back(s);
    }

    r.masking = MaskingSummary{};
    return r;
}

std::map<std::string, std::uint32_t> tagsById(const std::vector<Recommendation>& recs)
{
    std::map<std::string, std::uint32_t> out;
    for (const auto& r : recs)
        if (r.type != RecommendationType::Strength) out[r.id] |= r.requires_inputs;
    return out;
}
} // namespace

TEST_CASE("RecommendationEngine: a finding that needs an optional input says so (REF-1)", "[recengine][ref1]")
{
    namespace in = analysis_inputs;
    GenreEntry genre;
    genre.id = "test";
    CreativeTarget target;
    RecommendationEngine engine;

    const auto rich = richResult();
    const auto all = tagsById(engine.generate(rich, genre, target));
    REQUIRE_FALSE(all.empty());

    struct Strip {
        std::uint32_t bit;
        const char* name;
        bool mustRemoveSomething; // the fixture trips a finding on this input
        void (*apply)(AnalysisResult&);
    };
    const Strip strips[] = {
        {in::kDeep, "deep", true,
         [](AnalysisResult& r) {
             r.python_analysis_ran = false;
             r.python_data = AnalysisResult::PythonData{};
         }},
        {in::kHarshness, "harshness", true, [](AnalysisResult& r) { r.harshness.reset(); }},
        {in::kMonoCompat, "mono_compat", true, [](AnalysisResult& r) { r.mono_compat.reset(); }},
        {in::kTranslationRisk, "translation_risk", true, [](AnalysisResult& r) { r.translation_risk.reset(); }},
        {in::kResonance, "resonance", true, [](AnalysisResult& r) { r.resonance.reset(); }},
        {in::kMidSide, "mid_side", false, [](AnalysisResult& r) { r.mid_side_imbalance.reset(); }},
        {in::kSections, "sections", true, [](AnalysisResult& r) { r.section_analysis.reset(); }},
        {in::kDrums, "drums", true, [](AnalysisResult& r) { r.drums.reset(); }},
        {in::kMasking, "masking", false, [](AnalysisResult& r) { r.masking.reset(); }},
    };

    for (const auto& strip : strips) {
        INFO("without " << strip.name);
        AnalysisResult less = rich;
        strip.apply(less);
        REQUIRE((less.inputs_available() & strip.bit) == 0);
        REQUIRE((rich.inputs_available() & strip.bit) != 0);
        const auto remaining = tagsById(engine.generate(less, genre, target));
        int removed = 0;
        for (const auto& [id, tags] : all) {
            if (remaining.count(id) != 0) continue;
            ++removed;
            INFO("finding " << id << " disappeared");
            // The engine must have said this finding needs the stripped input;
            // otherwise the fix check would call it resolved.
            CHECK((tags & strip.bit) != 0);
        }
        if (strip.mustRemoveSomething) CHECK(removed > 0);
        // And nothing that stayed claims to need what is no longer there.
        for (const auto& [id, tags] : remaining) {
            INFO("finding " << id << " stayed");
            CHECK((tags & strip.bit) == 0);
        }
    }

    // Fast-pass findings need nothing.
    AnalysisResult fast;
    fast.file_id = "fast";
    fast.loudness.lufs_integrated = -4.0f;
    fast.loudness.true_peak_dbtp = 0.8f;
    for (const auto& [id, tags] : tagsById(engine.generate(fast, genre, target))) {
        INFO("finding " << id);
        CHECK(tags == 0u);
    }
}


TEST_CASE("Unvalidated song-level findings are capped at a minor note (FULL-3)", "[recengine]")
{
    AnalysisResult r;
    r.file_id = "f_heavy";
    r.python_analysis_ran = true;
    r.loudness.lufs_integrated = -10.0f;
    r.python_data.heaviness.extremity = 0.0f;  // far under a heavy genre's norm
    r.python_data.heaviness.distortion = 0.0f;
    GenreEntry genre;
    genre.id = "deathcore";
    genre.heaviness_extremity_norm = 0.95f;
    genre.heaviness_distortion_norm = 0.9f;
    const auto recs = RecommendationEngine{}.generate(r, genre, CreativeTarget{});
    bool sawHeaviness = false;
    for (const auto& rec : recs)
        if (rec.id.rfind("heaviness_", 0) == 0) {
            sawHeaviness = true;
            CHECK(rec.severity <= RecommendationEngine::kUnvalidatedSeverityCap);
            CHECK(rec.confidence_reason.find("Experimental") != std::string::npos);
        }
    CHECK(sawHeaviness);
}
