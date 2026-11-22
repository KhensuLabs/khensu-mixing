#include <catch2/catch_test_macros.hpp>
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/recommendations/HarshnessRecommender.h>
#include <khensu/recommendations/MonoCompatRecommender.h>
#include <khensu/recommendations/TranslationRiskRecommender.h>
#include <khensu/recommendations/ResonanceRecommender.h>
#include <khensu/recommendations/MidSideImbalanceRecommender.h>
#include <khensu/recommendations/ProcessingStage.h>
#include <khensu/recommendations/DynamicsRecommender.h>
#include <khensu/recommendations/StrengthRecommender.h>
#include <algorithm>
#include <cmath>

using namespace khensu;

// ---------------------------------------------------------------------------
// HarshnessRecommender
// ---------------------------------------------------------------------------

TEST_CASE("HarshnessRecommender fires above threshold", "[recommenders][extended][harshness]")
{
    AnalysisResult r;
    r.file_id = "f_harsh";
    r.harshness = AnalysisResult::HarshnessData{};
    r.harshness->score = 0.6f;
    r.harshness->peak_hz = 3500.0f;
    r.harshness->peak_db = -10.0f;

    auto recs = HarshnessRecommender{}.recommend(r, {}, {});
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].stage == ProcessingStage::Master);
    CHECK(recs[0].chain_position == 7);
    CHECK(recs[0].is_taste_decision == false);
}

TEST_CASE("HarshnessRecommender silent below threshold", "[recommenders][extended][harshness]")
{
    AnalysisResult r;
    r.file_id = "f_clean";
    r.harshness = AnalysisResult::HarshnessData{};
    r.harshness->score = 0.20f;
    auto recs = HarshnessRecommender{}.recommend(r, {}, {});
    CHECK(recs.empty());
}

TEST_CASE("HarshnessRecommender silent when harshness absent", "[recommenders][extended][harshness]")
{
    AnalysisResult r;
    r.file_id = "f_no_harshness";
    // harshness is nullopt by default
    auto recs = HarshnessRecommender{}.recommend(r, {}, {});
    CHECK(recs.empty());
}

// ---------------------------------------------------------------------------
// MonoCompatRecommender
// ---------------------------------------------------------------------------

TEST_CASE("MonoCompatRecommender fires on wide low end", "[recommenders][extended][mono_compat]")
{
    AnalysisResult r;
    r.file_id = "f_mono";
    r.mono_compat = AnalysisResult::MonoCompatData{};
    r.mono_compat->low_end_stereo_pct = 45.0f; // > 30.0 threshold
    r.mono_compat->per_band_correlation["sub"] = 0.3f;
    r.mono_compat->per_band_correlation["bass"] = 0.5f;
    r.mono_compat->per_band_correlation["midrange"] = 0.8f;
    r.mono_compat->per_band_correlation["upper_mid"] = 0.9f;
    r.mono_compat->per_band_correlation["air"] = 0.95f;

    auto recs = MonoCompatRecommender{}.recommend(r, {}, {});
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].stage == ProcessingStage::MixBus);
}

TEST_CASE("MonoCompatRecommender fires on low band correlation", "[recommenders][extended][mono_compat]")
{
    AnalysisResult r;
    r.file_id = "f_mono_corr";
    r.mono_compat = AnalysisResult::MonoCompatData{};
    r.mono_compat->low_end_stereo_pct = 10.0f;         // below 30 - won't fire on this
    r.mono_compat->per_band_correlation["sub"] = 0.2f; // < 0.4 threshold
    r.mono_compat->per_band_correlation["bass"] = 0.5f;
    r.mono_compat->per_band_correlation["midrange"] = 0.8f;
    r.mono_compat->per_band_correlation["upper_mid"] = 0.9f;
    r.mono_compat->per_band_correlation["air"] = 0.95f;

    auto recs = MonoCompatRecommender{}.recommend(r, {}, {});
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].stage == ProcessingStage::MixBus);
    CHECK(recs[0].chain_position == 6);
}

TEST_CASE("MonoCompatRecommender silent when absent", "[recommenders][extended][mono_compat]")
{
    AnalysisResult r;
    r.file_id = "f_no_mono";
    auto recs = MonoCompatRecommender{}.recommend(r, {}, {});
    CHECK(recs.empty());
}

// ---------------------------------------------------------------------------
// TranslationRiskRecommender
// ---------------------------------------------------------------------------

TEST_CASE("TranslationRiskRecommender fires on high earbuds risk", "[recommenders][extended][translation]")
{
    AnalysisResult r;
    r.file_id = "f_trans";
    r.translation_risk = AnalysisResult::TranslationRiskData{};
    r.translation_risk->per_environment["studio"] = 0.1f;
    r.translation_risk->per_environment["earbuds"] = 0.75f; // > 0.6 threshold
    r.translation_risk->per_environment["phone"] = 0.2f;
    r.translation_risk->per_environment["car"] = 0.3f;
    r.translation_risk->per_environment["club"] = 0.2f;

    auto recs = TranslationRiskRecommender{}.recommend(r, {}, {});
    REQUIRE_FALSE(recs.empty());
    // ID should include the environment name
    bool found_earbuds = false;
    for (const auto& rec : recs)
        if (rec.id.find("earbuds") != std::string::npos) found_earbuds = true;
    CHECK(found_earbuds);
}

TEST_CASE("TranslationRiskRecommender silent when all risks low", "[recommenders][extended][translation]")
{
    AnalysisResult r;
    r.file_id = "f_trans_ok";
    r.translation_risk = AnalysisResult::TranslationRiskData{};
    r.translation_risk->per_environment["studio"] = 0.1f;
    r.translation_risk->per_environment["earbuds"] = 0.5f; // <= 0.6
    r.translation_risk->per_environment["phone"] = 0.2f;
    r.translation_risk->per_environment["car"] = 0.3f;
    r.translation_risk->per_environment["club"] = 0.2f;

    auto recs = TranslationRiskRecommender{}.recommend(r, {}, {});
    CHECK(recs.empty());
}

TEST_CASE("TranslationRiskRecommender silent when absent", "[recommenders][extended][translation]")
{
    AnalysisResult r;
    r.file_id = "f_no_trans";
    auto recs = TranslationRiskRecommender{}.recommend(r, {}, {});
    CHECK(recs.empty());
}

// ---------------------------------------------------------------------------
// ResonanceRecommender
// ---------------------------------------------------------------------------

TEST_CASE("ResonanceRecommender fires on strong peak", "[recommenders][extended][resonance]")
{
    AnalysisResult r;
    r.file_id = "f_res";
    r.resonance = AnalysisResult::ResonanceData{};
    // ResonancePeak has {hz, db, prominence} - set prominence=14.0 (third field)
    r.resonance->hotspots.push_back({350.0f, 0.0f, 8.0f}); // 350 Hz, 8 dB prominence - above the 5 dB edge

    auto recs = ResonanceRecommender{}.recommend(r, {}, {});
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].stage == ProcessingStage::MixBus);
    CHECK(recs[0].chain_position == 4);
}

TEST_CASE("ResonanceRecommender silent when no strong peaks", "[recommenders][extended][resonance]")
{
    AnalysisResult r;
    r.file_id = "f_clean_res";
    r.resonance = AnalysisResult::ResonanceData{};
    r.resonance->hotspots.push_back({350.0f, 0.0f, 4.0f}); // only 4 dB prominence - below the 5 dB edge

    auto recs = ResonanceRecommender{}.recommend(r, {}, {});
    CHECK(recs.empty());
}

TEST_CASE("ResonanceRecommender silent when absent", "[recommenders][extended][resonance]")
{
    AnalysisResult r;
    r.file_id = "f_no_res";
    auto recs = ResonanceRecommender{}.recommend(r, {}, {});
    CHECK(recs.empty());
}

TEST_CASE("ResonanceRecommender groups a harmonic series into one pitched finding", "[recommenders][extended][resonance]")
{
    // The real song of 2026-11-14: 219/440/880 Hz (one A3) plus two unrelated peaks.
    AnalysisResult r;
    r.file_id = "f_song";
    r.resonance = AnalysisResult::ResonanceData{};
    r.resonance->hotspots = {{219.0f, 0.0f, 33.0f}, {440.0f, 0.0f, 17.0f}, {880.0f, 0.0f, 16.0f},
                             {154.0f, 0.0f, 6.0f}, {698.0f, 0.0f, 14.0f}};

    const auto recs = ResonanceRecommender{}.recommend(r, {}, {});
    REQUIRE(recs.size() == 3); // A3 series, 154 Hz, 698 Hz
    const auto a3 = std::find_if(recs.begin(), recs.end(), [](const Recommendation& x) {
        return x.id.rfind("resonance_hotspot_219hz_", 0) == 0;
    });
    REQUIRE(a3 != recs.end());
    CHECK(a3->what.find("A3") != std::string::npos);
    CHECK(a3->what.find("440, 880") != std::string::npos);
    CHECK(std::abs(a3->severity - 60.0f) < 0.01f); // 100 x the pitched scale
    CHECK(a3->confidence == 0.5f);
    CHECK(a3->is_complete());
    for (const auto& x : recs) {
        CHECK(x.id.find("440hz") == std::string::npos);
        CHECK(x.id.find("880hz") == std::string::npos);
    }
    // An unrelated peak keeps the plain resonance wording and severity.
    const auto p154 = std::find_if(recs.begin(), recs.end(), [](const Recommendation& x) {
        return x.id.rfind("resonance_hotspot_154hz_", 0) == 0;
    });
    REQUIRE(p154 != recs.end());
    CHECK(p154->confidence == 0.72f);
    CHECK(p154->severity == 36.0f); // 20 at the 5 dB edge + 16 per dB
    CHECK(p154->suggested_parameters.at("eq_cut_db") == 9.0f);
    CHECK(a3->suggested_parameters.count("eq_cut_db") == 0); // a note: no static notch
}

TEST_CASE("ResonanceRecommender note names", "[recommenders][extended][resonance]")
{
    CHECK(ResonanceRecommender::noteName(219.0f) == "A3");
    CHECK(ResonanceRecommender::noteName(440.0f) == "A4");
    CHECK(ResonanceRecommender::noteName(261.6f) == "C4");
    CHECK(ResonanceRecommender::noteName(0.0f).empty());
}

TEST_CASE("Crest above the genre band is not also a transient-headroom strength", "[recommenders][strength]")
{
    GenreEntry genre;
    genre.crest_factor_target_db = 9.8f;
    AnalysisResult r;
    r.file_id = "f_crest";
    r.loudness.lufs_integrated = -13.4f;

    r.dynamics.crest_factor_db = 15.7f; // the real song: 5.9 dB above target
    auto has = [](const std::vector<Recommendation>& v, const char* code) {
        return std::any_of(v.begin(), v.end(), [code](const Recommendation& x) { return x.id.rfind(code, 0) == 0; });
    };
    CHECK(has(DynamicsRecommender{}.recommend(r, genre, {}), "crest_above_genre_target"));
    CHECK_FALSE(has(StrengthRecommender{}.recommend(r, genre, {}), "strength_transient_headroom"));

    r.dynamics.crest_factor_db = 10.5f; // inside the band
    CHECK(has(StrengthRecommender{}.recommend(r, genre, {}), "strength_transient_headroom"));
}

TEST_CASE("Crest advice controls peaks instead of adding bus compression", "[recommenders][dynamics]")
{
    GenreEntry genre;
    genre.crest_factor_target_db = 9.8f;
    AnalysisResult r;
    r.file_id = "f_crest_adv";
    r.dynamics.crest_factor_db = 15.7f;
    const auto recs = DynamicsRecommender{}.recommend(r, genre, {});
    const auto it = std::find_if(recs.begin(), recs.end(), [](const Recommendation& x) {
        return x.id.rfind("crest_above_genre_target", 0) == 0;
    });
    REQUIRE(it != recs.end());
    CHECK(it->suggested_action.find("limiter or clipper") != std::string::npos);
    CHECK(it->suggested_action.find("bus compression") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Cross-recommender: completeness check
// ---------------------------------------------------------------------------

TEST_CASE("All extended recommenders return complete recommendations", "[recommenders][extended]")
{
    // Verify is_complete() returns true on all fired recommendations
    AnalysisResult r;
    r.file_id = "f_complete_check";

    r.harshness = AnalysisResult::HarshnessData{};
    r.harshness->score = 0.7f;

    r.mono_compat = AnalysisResult::MonoCompatData{};
    r.mono_compat->low_end_stereo_pct = 50.0f;
    r.mono_compat->per_band_correlation["sub"] = 0.2f;
    r.mono_compat->per_band_correlation["bass"] = 0.6f;
    r.mono_compat->per_band_correlation["midrange"] = 0.8f;
    r.mono_compat->per_band_correlation["upper_mid"] = 0.85f;
    r.mono_compat->per_band_correlation["air"] = 0.9f;

    r.translation_risk = AnalysisResult::TranslationRiskData{};
    r.translation_risk->per_environment["studio"] = 0.1f;
    r.translation_risk->per_environment["earbuds"] = 0.8f;
    r.translation_risk->per_environment["phone"] = 0.3f;
    r.translation_risk->per_environment["car"] = 0.2f;
    r.translation_risk->per_environment["club"] = 0.2f;

    r.resonance = AnalysisResult::ResonanceData{};
    r.resonance->hotspots.push_back({800.0f, 0.0f, 12.0f});

    for (const auto& rec : HarshnessRecommender{}.recommend(r, {}, {}))
        CHECK(rec.is_complete());
    for (const auto& rec : MonoCompatRecommender{}.recommend(r, {}, {}))
        CHECK(rec.is_complete());
    for (const auto& rec : TranslationRiskRecommender{}.recommend(r, {}, {}))
        CHECK(rec.is_complete());
    for (const auto& rec : ResonanceRecommender{}.recommend(r, {}, {}))
        CHECK(rec.is_complete());
}

TEST_CASE("M/S brightness fires once the side is brighter than the mid (FULL-3)", "[recommenders][extended][midside]")
{
    AnalysisResult r;
    r.file_id = "f_ms";
    r.mid_side_imbalance = AnalysisResult::MidSideImbalanceData{};
    CHECK(MidSideImbalanceRecommender{}.recommend(r, {}, {}).empty()); // the default 1.0: no finding
    r.mid_side_imbalance->side_bright_ratio = 1.25f;
    r.mid_side_imbalance->imbalance_severity = 0.1f; // the old composite gate no longer applies
    const auto recs = MidSideImbalanceRecommender{}.recommend(r, {}, {});
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].id.rfind("ms_brightness_imbalance", 0) == 0);
    CHECK(recs[0].severity == 40.0f);
}
