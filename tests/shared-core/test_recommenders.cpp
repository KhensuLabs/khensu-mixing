#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <algorithm>
#include <khensu/recommendations/LoudnessRecommender.h>
#include <khensu/recommendations/SpectralRecommender.h>
#include <khensu/recommendations/DynamicsRecommender.h>
#include <khensu/recommendations/StereoRecommender.h>
#include <khensu/recommendations/ArtifactRecommender.h>
#include <khensu/recommendations/HookRecommender.h>
#include <khensu/recommendations/MemorabilityRecommender.h>
#include <khensu/recommendations/StructuralFlowRecommender.h>
#include <khensu/recommendations/HeavinessRecommender.h>
#include <khensu/recommendations/MaskingRecommender.h>
#include <khensu/recommendations/ProcessingStage.h>

using namespace khensu;
using Catch::Matchers::WithinAbs;

static GenreEntry makeGenre()
{
    GenreEntry g;
    g.id = "trap";
    g.spectral_centroid_hz = 3200.0f;
    g.low_end_weight = 0.65f;
    g.stereo_width_min = 0.55f;
    g.stereo_width_max = 0.90f;
    g.lra_range_max = 9.0f;
    return g;
}

static AnalysisResult makeResult()
{
    AnalysisResult r;
    r.file_id = "550e8400-e29b-41d4-a716-446655440000";
    return r;
}

// ── Spectral ──────────────────────────────────────────────────────────────

TEST_CASE("SpectralRecommender", "[spectral]")
{
    SpectralRecommender rec;
    GenreEntry genre = makeGenre(); // centroid_hz=3200, low_end_weight=0.65

    // Neutralises the new spectral-tilt-vs-genre-target check (added
    // alongside the centroid/balance checks below) so these pre-existing
    // sections keep testing exactly one behaviour each; tilt-vs-target has
    // its own dedicated section further down.
    const float neutralTilt = genre.spectral_tilt_target_db_per_octave;

    SECTION("no findings when in range")
    {
        auto result = makeResult();
        result.spectral.centroid_hz = 3200.0f; // exactly at norm
        result.spectral.balance_low = 0.65f;   // exactly at norm
        result.spectral.tilt_db_per_octave = neutralTilt;
        CHECK(rec.recommend(result, genre, CreativeTarget{}).empty());
    }

    SECTION("centroid_too_bright fires when > norm x 1.25")
    {
        auto result = makeResult();
        result.spectral.centroid_hz = 4200.0f; // 3200 x 1.25 = 4000; 4200 > 4000
        result.spectral.balance_low = 0.65f;
        result.spectral.tilt_db_per_octave = neutralTilt;
        auto findings = rec.recommend(result, genre, CreativeTarget{});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].id.rfind("centroid_too_bright_", 0) == 0);
        CHECK_THAT(findings[0].confidence, WithinAbs(0.80f, 0.001f));
    }

    SECTION("centroid_too_dark fires when < norm x 0.75")
    {
        auto result = makeResult();
        result.spectral.centroid_hz = 2000.0f; // 3200 x 0.75 = 2400; 2000 < 2400
        result.spectral.balance_low = 0.65f;
        result.spectral.tilt_db_per_octave = neutralTilt;
        auto findings = rec.recommend(result, genre, CreativeTarget{});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].id.rfind("centroid_too_dark_", 0) == 0);
    }

    SECTION("low_end_heavy fires when balance_low > norm + 0.20")
    {
        auto result = makeResult();
        result.spectral.centroid_hz = 3200.0f;
        result.spectral.balance_low = 0.90f; // 0.65 + 0.20 = 0.85; 0.90 > 0.85
        result.spectral.tilt_db_per_octave = neutralTilt;
        auto findings = rec.recommend(result, genre, CreativeTarget{});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].id.rfind("low_end_heavy_", 0) == 0);
    }

    SECTION("low_end_light fires when balance_low < norm - 0.20")
    {
        auto result = makeResult();
        result.spectral.centroid_hz = 3200.0f;
        result.spectral.balance_low = 0.40f; // 0.65 - 0.20 = 0.45; 0.40 < 0.45
        result.spectral.tilt_db_per_octave = neutralTilt;
        auto findings = rec.recommend(result, genre, CreativeTarget{});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].id.rfind("low_end_light_", 0) == 0);
    }

    SECTION("tilt_too_bright fires when tilt exceeds genre target by more than tolerance")
    {
        auto result = makeResult();
        result.spectral.centroid_hz = 3200.0f;
        result.spectral.balance_low = 0.65f;
        result.spectral.tilt_db_per_octave = neutralTilt + 3.0f; // tolerance is 2.0
        auto findings = rec.recommend(result, genre, CreativeTarget{});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].id.rfind("tilt_too_bright_", 0) == 0);
    }

    SECTION("tilt_too_dark fires when tilt falls below genre target by more than tolerance")
    {
        auto result = makeResult();
        result.spectral.centroid_hz = 3200.0f;
        result.spectral.balance_low = 0.65f;
        result.spectral.tilt_db_per_octave = neutralTilt - 3.0f;
        auto findings = rec.recommend(result, genre, CreativeTarget{});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].id.rfind("tilt_too_dark_", 0) == 0);
    }
}

// ── Dynamics ──────────────────────────────────────────────────────────────

TEST_CASE("DynamicsRecommender", "[dynamics]")
{
    DynamicsRecommender rec;
    GenreEntry genre = makeGenre(); // lra_range_max = 9.0

    SECTION("no findings when in range")
    {
        auto result = makeResult();
        result.dynamics.crest_factor_db = 10.0f; // > 6.0 - ok
        result.dynamics.dynamic_range_db = 8.0f; // < 9.0 x 1.5 = 13.5 - ok
        CHECK(rec.recommend(result, genre, CreativeTarget{}).empty());
    }

    SECTION("over_compressed fires below the genre floor (target - 3 dB)")
    {
        // Genre crest target 10 dB -> floor 7 dB. The genre floor replaced
        // both the fixed 6 dB floor and crest_below_genre_target, which fired
        // at the same point and only duplicated the card (AUDIT-070).
        auto result = makeResult();
        result.dynamics.crest_factor_db = 4.0f;
        result.dynamics.dynamic_range_db = 8.0f;
        auto findings = rec.recommend(result, genre, CreativeTarget{});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].id.rfind("over_compressed_", 0) == 0);
        CHECK_THAT(findings[0].confidence, WithinAbs(0.80f, 0.001f));
    }

    SECTION("an on-target hyperpop master is not over_compressed (AUDIT-070)")
    {
        GenreEntry hyperpop = genre;
        hyperpop.name = "Hyperpop";
        hyperpop.crest_factor_target_db = 6.0f; // floor 3 dB
        auto result = makeResult();
        result.dynamics.crest_factor_db = 5.0f; // fired under the old fixed 6 dB floor
        CHECK(rec.recommend(result, hyperpop, CreativeTarget{}).empty());
        result.dynamics.crest_factor_db = 2.5f;
        auto findings = rec.recommend(result, hyperpop, CreativeTarget{});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].id.rfind("over_compressed_", 0) == 0);
    }

    SECTION("under_compressed fires when the gated LRA > lra_range_max x 1.5")
    {
        auto result = makeResult();
        result.dynamics.crest_factor_db = 10.0f; // matches genre crest target -> no co-fire
        result.loudness.lra = 16.0f;             // 9.0 x 1.5 = 13.5; 16.0 > 13.5
        auto findings = rec.recommend(result, genre, CreativeTarget{});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].id.rfind("under_compressed_", 0) == 0);
    }

    SECTION("under_compressed ignores the ungated block spread and unmeasured LRA")
    {
        auto result = makeResult();
        result.dynamics.crest_factor_db = 10.0f;
        result.dynamics.dynamic_range_db = 30.0f; // a song with a break: big ungated spread
        result.loudness.lra = 6.0f;               // but the gated LRA is normal
        CHECK(rec.recommend(result, genre, CreativeTarget{}).empty());
        result.loudness.lra = 16.0f;
        result.loudness.lra_measured = false; // too short to measure
        CHECK(rec.recommend(result, genre, CreativeTarget{}).empty());
    }

    SECTION("under_compressed never fires together with over_compressed")
    {
        auto result = makeResult();
        result.dynamics.crest_factor_db = 4.0f; // over-compressed
        result.loudness.lra = 20.0f;            // and a wide LRA (e.g. loud/quiet sections)
        auto findings = rec.recommend(result, genre, CreativeTarget{});
        CHECK(std::none_of(findings.begin(), findings.end(),
                           [](const Recommendation& r) { return r.id.rfind("under_compressed_", 0) == 0; }));
    }

    SECTION("crest_above_genre_target fires when crest exceeds genre target by more than tolerance")
    {
        auto result = makeResult();
        result.dynamics.crest_factor_db = genre.crest_factor_target_db + 5.0f; // tolerance is 3.0
        result.dynamics.dynamic_range_db = 8.0f;
        auto findings = rec.recommend(result, genre, CreativeTarget{});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].id.rfind("crest_above_genre_target_", 0) == 0);
    }
}

// ── Stereo ────────────────────────────────────────────────────────────────

TEST_CASE("StereoRecommender", "[stereo]")
{
    StereoRecommender rec;
    GenreEntry genre = makeGenre(); // stereo_width_min=0.55, stereo_width_max=0.90

    SECTION("no findings when in range")
    {
        auto result = makeResult();
        result.stereo.width = 0.70f;       // in range [0.55, 0.90]
        result.stereo.correlation = 0.50f; // > 0
        CHECK(rec.recommend(result, genre, CreativeTarget{}).empty());
    }

    SECTION("stereo_too_narrow fires when width < stereo_width_min")
    {
        auto result = makeResult();
        result.stereo.width = 0.30f;
        result.stereo.correlation = 0.80f;
        auto findings = rec.recommend(result, genre, CreativeTarget{});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].id.rfind("stereo_too_narrow_", 0) == 0);
        CHECK_THAT(findings[0].confidence, WithinAbs(0.80f, 0.001f));
    }

    SECTION("stereo_too_wide fires when width > stereo_width_max")
    {
        auto result = makeResult();
        result.stereo.width = 0.98f;
        result.stereo.correlation = 0.80f;
        auto findings = rec.recommend(result, genre, CreativeTarget{});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].id.rfind("stereo_too_wide_", 0) == 0);
    }

    SECTION("phase_issue fires when correlation < 0.0")
    {
        auto result = makeResult();
        result.stereo.width = 0.70f;
        result.stereo.correlation = -0.30f;
        auto findings = rec.recommend(result, genre, CreativeTarget{});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].id.rfind("phase_issue_", 0) == 0);
    }
}

// ── Artifacts ─────────────────────────────────────────────────────────────

TEST_CASE("ArtifactRecommender", "[artifacts]")
{
    ArtifactRecommender rec;
    GenreEntry genre = makeGenre();

    SECTION("no findings when clean")
    {
        auto result = makeResult();
        result.artifacts.has_clipping = false;
        result.artifacts.noise_floor_db = -65.0f;
        CHECK(rec.recommend(result, genre, CreativeTarget{}).empty());
    }

    SECTION("clipping_detected fires with fixed severity 95 and confidence 0.90")
    {
        auto result = makeResult();
        result.artifacts.has_clipping = true;
        result.artifacts.clipping_frame_count = ArtifactRecommender::kMinClippedFrames - 1; // inaudible
        CHECK(rec.recommend(result, genre, CreativeTarget{}).empty());
        result.artifacts.clipping_frame_count = ArtifactRecommender::kMinClippedFrames;
        auto findings = rec.recommend(result, genre, CreativeTarget{});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].id.rfind("clipping_detected_", 0) == 0);
        CHECK_THAT(findings[0].severity, WithinAbs(95.0f, 0.001f));
        CHECK_THAT(findings[0].confidence, WithinAbs(0.90f, 0.001f));
    }

    SECTION("noise_floor_high fires for a floor between -50 and -40 dBFS (MEAS-4)")
    {
        auto result = makeResult();
        result.artifacts.noise_floor_db = -38.0f; // quiet music, not a floor
        CHECK(rec.recommend(result, genre, CreativeTarget{}).empty());
        result.artifacts.noise_floor_db = -45.0f;
        auto findings = rec.recommend(result, genre, CreativeTarget{});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].id.rfind("noise_floor_high_", 0) == 0);
        CHECK(findings[0].severity > 0.0f);
    }

    SECTION("dc_offset fires at the service's 0.002 mean and grows with the offset (MEAS-4)")
    {
        auto result = makeResult();
        result.artifacts.dc_offset = 0.0015f; // under the threshold
        CHECK(rec.recommend(result, genre, CreativeTarget{}).empty());

        result.artifacts.dc_offset = -0.01f; // either sign
        auto small = rec.recommend(result, genre, CreativeTarget{});
        REQUIRE(small.size() == 1);
        CHECK(small[0].id.rfind("dc_offset_", 0) == 0);

        result.artifacts.dc_offset = 0.04f;
        auto large = rec.recommend(result, genre, CreativeTarget{});
        REQUIRE(large.size() == 1);
        CHECK(large[0].severity > small[0].severity);

        // A mean that does not hold through the file is not an offset.
        result.artifacts.dc_offset_steady = false;
        CHECK(rec.recommend(result, genre, CreativeTarget{}).empty());
    }

    SECTION("never an ai_artifacts finding (FIX-13)")
    {
        // The pass has no calibrated AI detector, so the rule that read its
        // flag is gone; even a file with every artifact reports only those.
        auto result = makeResult();
        result.artifacts.has_clipping = true;
        result.artifacts.clipping_frame_count = ArtifactRecommender::kMinClippedFrames;
        result.artifacts.noise_floor_db = -45.0f;
        result.artifacts.sample_reversal_rate = 0.67f; // white noise reads this high
        const auto findings = rec.recommend(result, genre, CreativeTarget{});
        CHECK(findings.size() == 2);
        for (const auto& f : findings)
            CHECK(f.id.rfind("ai_artifacts_", 0) != 0);
    }
}

// ── Memorability ──────────────────────────────────────────────────────────

TEST_CASE("MemorabilityRecommender", "[memorability]")
{
    MemorabilityRecommender rec;
    GenreEntry genre = makeGenre();

    SECTION("quick-mode guard: returns empty without python data")
    {
        auto result = makeResult();
        // bpm_detected stays 0 → no python analysis
        CHECK(rec.recommend(result, genre, CreativeTarget{}).empty());
    }

    SECTION("no findings when memorability in range")
    {
        auto result = makeResult();
        result.python_data.bpm_detected = 120.0f;
        result.python_data.hooks.hook_count = 2;
        result.python_data.memorability.score = 0.75f;              // > 0.65 threshold at t=50
        result.python_data.memorability.earworm_likelihood = 0.60f; // > 0.50 threshold at t=50
        CHECK(rec.recommend(result, genre, CreativeTarget{}).empty());
    }

    SECTION("TUNE-8: silent without a detected hook (no_hooks_detected speaks)")
    {
        auto result = makeResult();
        result.python_data.bpm_detected = 120.0f;
        result.python_data.hooks.hook_count = 0;
        result.python_data.memorability.score = 0.25f;
        result.python_data.memorability.earworm_likelihood = 0.0f;
        CHECK(rec.recommend(result, genre, CreativeTarget{}).empty());
    }

    SECTION("memorability_low fires when score below threshold")
    {
        auto result = makeResult();
        result.python_data.bpm_detected = 120.0f;
        result.python_data.hooks.hook_count = 2;
        result.python_data.memorability.score = 0.30f;              // < 0.65
        result.python_data.memorability.earworm_likelihood = 0.60f; // ok
        auto findings = rec.recommend(result, genre, CreativeTarget{});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].id.rfind("memorability_low_", 0) == 0);
        CHECK_THAT(findings[0].confidence, WithinAbs(0.70f, 0.001f));
    }

    SECTION("earworm_likelihood_low fires when earworm below threshold")
    {
        auto result = makeResult();
        result.python_data.bpm_detected = 120.0f;
        result.python_data.hooks.hook_count = 2;
        result.python_data.memorability.score = 0.75f;              // ok
        result.python_data.memorability.earworm_likelihood = 0.20f; // < 0.50
        auto findings = rec.recommend(result, genre, CreativeTarget{});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].id.rfind("earworm_likelihood_low_", 0) == 0);
    }
}

// ── StructuralFlow ────────────────────────────────────────────────────────

TEST_CASE("StructuralFlowRecommender", "[structural]")
{
    StructuralFlowRecommender rec;
    GenreEntry genre = makeGenre();

    SECTION("quick-mode guard: returns empty without python data")
    {
        auto result = makeResult();
        CHECK(rec.recommend(result, genre, CreativeTarget{}).empty());
    }

    SECTION("no findings when arc_quality >= 0.50")
    {
        auto result = makeResult();
        result.python_data.bpm_detected = 120.0f;
        result.python_data.structural_flow.arc_quality = 0.70f;
        CHECK(rec.recommend(result, genre, CreativeTarget{}).empty());
    }

    SECTION("structural_flow_poor fires when arc_quality < 0.50")
    {
        auto result = makeResult();
        result.python_data.bpm_detected = 120.0f;
        result.python_data.structural_flow.arc_quality = 0.30f;
        auto findings = rec.recommend(result, genre, CreativeTarget{});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].id.rfind("structural_flow_poor_", 0) == 0);
        CHECK_THAT(findings[0].confidence, WithinAbs(0.70f, 0.001f));
    }
}

// ── Heaviness ─────────────────────────────────────────────────────────────

TEST_CASE("HeavinessRecommender", "[heaviness]")
{
    HeavinessRecommender rec;
    GenreEntry genre = makeGenre();
    genre.heaviness_extremity_norm = 0.50f;
    genre.heaviness_weight_norm = 0.60f;
    genre.heaviness_distortion_norm = 0.40f;

    SECTION("quick-mode guard: returns empty without python data")
    {
        auto result = makeResult();
        CHECK(rec.recommend(result, genre, CreativeTarget{}).empty());
    }

    SECTION("no findings when all heaviness values in range")
    {
        auto result = makeResult();
        result.python_data.bpm_detected = 120.0f;
        result.python_data.heaviness.extremity = 0.50f;  // matches threshold at t=50
        result.python_data.heaviness.weight = 0.65f;     // > 0.60 threshold (won't fire since heaviness_target=50)
        result.python_data.heaviness.distortion = 0.40f; // matches norm
        // heaviness_extremity_mismatch: on the threshold → ok
        // heaviness_weight_low: weight > threshold but also heaviness_target=50, not > 50 → no fire
        // heaviness_distortion_mismatch: on the norm → ok
        CHECK(rec.recommend(result, genre, CreativeTarget{}).empty());
    }

    SECTION("heaviness_extremity_mismatch fires outside the odds-10 band (0.09..0.91 at 0.50)")
    {
        auto result = makeResult();
        result.python_data.bpm_detected = 120.0f;
        result.python_data.heaviness.extremity = 0.95f; // above the 0.91 ceiling
        result.python_data.heaviness.weight = 0.65f;
        result.python_data.heaviness.distortion = 0.40f;
        auto findings = rec.recommend(result, genre, CreativeTarget{});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].id.rfind("heaviness_extremity_mismatch_", 0) == 0);
        CHECK(findings[0].stem_or_bus == "Low End Bus");
    }

    SECTION("heaviness_weight_low fires only when weight < threshold AND heaviness_target > 50")
    {
        auto result = makeResult();
        result.python_data.bpm_detected = 120.0f;
        result.python_data.heaviness.extremity = 0.50f;
        result.python_data.heaviness.weight = 0.30f; // below threshold
        result.python_data.heaviness.distortion = 0.40f;
        CreativeTarget tgt;
        tgt.heaviness_target = 75.0f; // > 50 - required condition
        auto findings = rec.recommend(result, genre, tgt);
        bool found_weight = false;
        for (const auto& f : findings)
            if (f.id.rfind("heaviness_weight_low_", 0) == 0) found_weight = true;
        CHECK(found_weight);
    }

    SECTION("heaviness_distortion_mismatch fires outside the odds-8 band (ceiling 0.84 at 0.40)")
    {
        auto result = makeResult();
        result.python_data.bpm_detected = 120.0f;
        result.python_data.heaviness.extremity = 0.50f;
        result.python_data.heaviness.weight = 0.65f;
        result.python_data.heaviness.distortion = 0.90f; // above the 0.84 ceiling
        auto findings = rec.recommend(result, genre, CreativeTarget{});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].id.rfind("heaviness_distortion_mismatch_", 0) == 0);
    }
}

// ── Stage Attribution Tests ───────────────────────────────────────────────────

TEST_CASE("LoudnessRecommender sets stage on every recommendation", "[recommenders][stage]")
{
    AnalysisResult r;
    r.file_id = "f_stage";
    r.loudness.lufs_integrated = -6.0f;
    r.loudness.true_peak_dbtp = 0.5f;
    r.loudness.lra = 1.0f;

    GenreEntry genre;
    genre.id = "pop";
    genre.lufs_range_max = -8.0f;
    genre.lufs_range_min = -16.0f;
    genre.true_peak_max = -1.0f;
    genre.lra_range_min = 4.0f;
    genre.lra_range_max = 10.0f;

    auto recs = LoudnessRecommender{}.recommend(r, genre, {});
    REQUIRE_FALSE(recs.empty());
    for (const auto& rec : recs) {
        CHECK(rec.stage != ProcessingStage::Unknown);
        CHECK(rec.chain_position != 99);
    }
}

TEST_CASE("DynamicsRecommender sets stage Master or MixBus", "[recommenders][stage]")
{
    AnalysisResult r;
    r.file_id = "f_dyn_stage";
    r.dynamics.crest_factor_db = 2.0f; // triggers over_compressed
    r.dynamics.dynamic_range_db = 20.0f;

    GenreEntry genre;
    genre.id = "pop";
    genre.lra_range_max = 10.0f;

    auto recs = DynamicsRecommender{}.recommend(r, genre, {});
    REQUIRE_FALSE(recs.empty());
    for (const auto& rec : recs) {
        CHECK((rec.stage == ProcessingStage::MixBus || rec.stage == ProcessingStage::Master));
    }
}

TEST_CASE("HookRecommender marks taste decisions", "[recommenders][stage]")
{
    AnalysisResult r;
    r.file_id = "f_hook_stage";
    r.python_data.bpm_detected = 120.0f;       // enable python analysis
    r.python_data.hooks.hook_strength = 0.10f; // very low - triggers hook_strength_low
    r.python_data.hooks.hook_count = 1;
    r.python_data.hooks.hook_density = 0.05f;

    auto recs = HookRecommender{}.recommend(r, {}, {});
    for (const auto& rec : recs)
        CHECK(rec.is_taste_decision == true);
}

// ── Masking ───────────────────────────────────────────────────────────────

TEST_CASE("MaskingRecommender ignores collisions below the moderate threshold", "[masking]")
{
    AnalysisResult r = makeResult();
    MaskingSummary m;
    m.bands_hz = {100.0f, 200.0f, 400.0f};
    MaskingPairSummary pair;
    pair.stem_a = "vocals";
    pair.stem_b = "guitar";
    pair.collision_score = 0.3f; // below kModerateThreshold (0.5)
    pair.band_scores = {0.2f, 0.3f};
    m.pairs.push_back(pair);
    r.masking = m;

    auto recs = MaskingRecommender{}.recommend(r, {}, {});
    CHECK(recs.empty());
}

TEST_CASE("MaskingRecommender emits a concrete EQ-cut recommendation", "[masking]")
{
    AnalysisResult r = makeResult();
    MaskingSummary m;
    m.bands_hz = {100.0f, 200.0f, 400.0f};
    MaskingPairSummary pair;
    pair.stem_a = "vocals";
    pair.stem_b = "guitar";
    pair.collision_score = 0.8f;
    pair.band_scores = {0.2f, 0.9f}; // worst band is index 1 -> 200-400 Hz
    m.pairs.push_back(pair);
    r.masking = m;

    auto recs = MaskingRecommender{}.recommend(r, {}, {});
    REQUIRE(recs.size() == 1);
    const auto& rec = recs[0];
    CHECK(rec.is_complete());
    REQUIRE(rec.freq_range.has_value());
    CHECK(rec.freq_range->low_hz == 200.0f);
    CHECK(rec.freq_range->high_hz == 400.0f);
    REQUIRE(rec.suggested_parameters.count("eq_freq_hz") == 1);
    REQUIRE(rec.suggested_parameters.count("eq_gain_db") == 1);
    CHECK_THAT(rec.suggested_parameters.at("eq_freq_hz"), WithinAbs(300.0f, 0.01f));
    CHECK(rec.suggested_parameters.at("eq_gain_db") < 0.0f); // always a cut, never a boost
}

TEST_CASE("MaskingRecommender scales the cut depth with collision severity", "[masking]")
{
    auto makePair = [](float score) {
        MaskingSummary m;
        m.bands_hz = {100.0f, 200.0f};
        MaskingPairSummary pair;
        pair.stem_a = "vocals";
        pair.stem_b = "synth";
        pair.collision_score = score;
        pair.band_scores = {score};
        m.pairs.push_back(pair);
        return m;
    };

    AnalysisResult mild = makeResult();
    mild.masking = makePair(0.5f);
    AnalysisResult severe = makeResult();
    severe.masking = makePair(1.0f);

    auto mildRecs = MaskingRecommender{}.recommend(mild, {}, {});
    auto severeRecs = MaskingRecommender{}.recommend(severe, {}, {});
    REQUIRE(mildRecs.size() == 1);
    REQUIRE(severeRecs.size() == 1);

    const float mildCut = mildRecs[0].suggested_parameters.at("eq_gain_db");
    const float severeCut = severeRecs[0].suggested_parameters.at("eq_gain_db");
    CHECK(severeCut < mildCut); // more collision -> deeper cut
}

TEST_CASE("MaskingRecommender holds each stem pair to its own bar (FULL-4)", "[masking]")
{
    auto withPair = [](const char* a, const char* b, float score) {
        AnalysisResult r = makeResult();
        MaskingSummary m;
        m.bands_hz = {60.0f, 120.0f};
        MaskingPairSummary pair;
        pair.stem_a = a;
        pair.stem_b = b;
        pair.collision_score = score;
        pair.band_scores = {score};
        m.pairs.push_back(pair);
        r.masking = m;
        return MaskingRecommender{}.recommend(r, {}, {}).size();
    };
    // Kick and bass overlap in most released music: 0.8 is ordinary there.
    CHECK(withPair("drums", "bass", 0.80f) == 0);
    CHECK(withPair("drums", "bass", 0.92f) == 1);
    CHECK(withPair("bass", "drums", 0.92f) == 1); // either order
    CHECK(withPair("vocals", "other", 0.70f) == 0);
    CHECK(withPair("vocals", "other", 0.75f) == 1);
    // A pair the corpus has not measured keeps the general bar.
    CHECK(withPair("vocals", "guitar", 0.55f) == 1);
    CHECK(MaskingRecommender::fireThreshold("vocals", "bass") >= MaskingRecommender::kModerateThreshold);
}

// -- Audit 2026-10-15 regressions ------------------------------------------

#include <khensu/recommendations/MixScorecard.h>
#include <khensu/recommendations/RecommendationEngine.h>
#include <khensu/recommendations/StrengthRecommender.h>

TEST_CASE("StereoRecommender skips width advice on mono sources (AUDIT-055)", "[stereo][audit]")
{
    StereoRecommender rec;
    GenreEntry genre = makeGenre(); // width min 0.55
    auto narrow = [](const std::vector<Recommendation>& v) {
        return std::any_of(v.begin(), v.end(),
                           [](const Recommendation& r) { return r.id.rfind("stereo_too_narrow_", 0) == 0; });
    };

    auto mono = makeResult();
    mono.channel_layout.channel_count = 1;
    mono.stereo.width = 0.0f;
    mono.stereo.correlation = 1.0f;
    CHECK_FALSE(narrow(rec.recommend(mono, genre, CreativeTarget{})));

    auto dualMono = makeResult(); // stereo file, identical channels
    dualMono.channel_layout.channel_count = 2;
    dualMono.stereo.width = 0.0f;
    dualMono.stereo.correlation = 1.0f;
    CHECK_FALSE(narrow(rec.recommend(dualMono, genre, CreativeTarget{})));

    auto realNarrow = dualMono; // a genuinely narrow stereo mix still fires
    realNarrow.stereo.width = 0.2f;
    realNarrow.stereo.correlation = 0.9f;
    CHECK(narrow(rec.recommend(realNarrow, genre, CreativeTarget{})));

    // The scorecard mirrors it: no width penalty on the mono source.
    const auto sc = buildMixScorecard(mono, genre, {}, ScoreWeightProfile{});
    for (const auto& c : sc.categories)
        if (c.id == "stereo_width_depth") CHECK(c.score == 100.0f);
}

TEST_CASE("LoudnessRecommender abstains on a silent file (AUDIT-069)", "[loudness][audit]")
{
    LoudnessRecommender rec;
    GenreEntry genre = makeGenre();
    auto silent = makeResult();
    silent.loudness.lufs_integrated = -70.0f;  // absolute-gate sentinel
    silent.loudness.true_peak_dbtp = -200.0f; // digital-silence sentinel
    CHECK(rec.recommend(silent, genre, CreativeTarget{}).empty());
    silent.loudness.lufs_integrated = -99.0f; // never measured
    CHECK(rec.recommend(silent, genre, CreativeTarget{}).empty());

    // No "true peak is safe" strength is claimed for silence either.
    silent.loudness.lufs_integrated = -70.0f;
    const auto strengths = StrengthRecommender{}.recommend(silent, genre, CreativeTarget{});
    CHECK(std::none_of(strengths.begin(), strengths.end(), [](const Recommendation& r) {
        return r.id.rfind("strength_true_peak_safe_", 0) == 0 || r.id.rfind("strength_lufs_on_target_", 0) == 0;
    }));

    // A quiet but real file still gets "too quiet" advice.
    auto quiet = makeResult();
    quiet.loudness.lufs_integrated = -30.0f;
    quiet.loudness.true_peak_dbtp = -15.0f;
    const auto recs = rec.recommend(quiet, genre, CreativeTarget{});
    CHECK(std::any_of(recs.begin(), recs.end(),
                      [](const Recommendation& r) { return r.id.rfind("lufs_too_quiet_", 0) == 0; }));
}

TEST_CASE("Crest trust binding follows the measurement that drives each card (AUDIT-070)", "[recommend][audit]")
{
    RecommendationEngine engine;
    GenreEntry genre = makeGenre(); // crest target 10, lra max 9
    genre.lufs_range_min = -60.0f;  // keep loudness cards out of this test
    genre.lufs_range_max = 0.0f;

    auto findSev = [](const std::vector<Recommendation>& v, const char* prefix) {
        for (const auto& r : v)
            if (r.id.rfind(prefix, 0) == 0) return r.severity;
        return -1.0f;
    };

    // under_compressed is driven by LRA: an unreliable CREST must not touch it.
    auto loose = makeResult();
    loose.loudness.lufs_integrated = -14.0f;
    loose.dynamics.crest_factor_db = 10.0f;
    loose.loudness.lra = 20.0f;
    const float base = findSev(engine.generate(loose, genre, CreativeTarget{}), "under_compressed_");
    REQUIRE(base > 0.0f);
    loose.measurement_trust["crest_factor_db"] = {0.2f, "unreliable", ""};
    CHECK(findSev(engine.generate(loose, genre, CreativeTarget{}), "under_compressed_") == base);

    // crest_above_genre_target IS crest-driven: it is down-weighted.
    auto dynamic = makeResult();
    dynamic.loudness.lufs_integrated = -14.0f;
    dynamic.dynamics.crest_factor_db = 16.0f;
    dynamic.loudness.lra = 6.0f;
    const float aboveBase = findSev(engine.generate(dynamic, genre, CreativeTarget{}), "crest_above_genre_target_");
    REQUIRE(aboveBase > 0.0f);
    dynamic.measurement_trust["crest_factor_db"] = {0.2f, "unreliable", ""};
    CHECK(findSev(engine.generate(dynamic, genre, CreativeTarget{}), "crest_above_genre_target_") ==
          aboveBase * 0.5f);
}
