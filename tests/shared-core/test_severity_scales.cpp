// FIX-9 / FIX-10 (docs/2026-11-09-project-review.md, section 3.2).
//
// FIX-10: a finding's severity is its error over a fixed full scale for that
// kind of metric - never the error divided by the threshold, which made the
// same miss score differently in every genre.
// FIX-9: the low-end rules stay reachable for every genre. `balance_low` is a
// share (0..1), so "target + 0.20" could not fire once the target reached 0.80.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <khensu/recommendations/HeavinessRecommender.h>
#include <khensu/recommendations/HookRecommender.h>
#include <khensu/recommendations/MemorabilityRecommender.h>
#include <khensu/recommendations/MixScorecard.h>
#include <khensu/recommendations/RecommendationEvidence.h>
#include <khensu/recommendations/SpectralRecommender.h>
#include <khensu/recommendations/StructuralFlowRecommender.h>
#include <khensu/recommendations/TargetInterpolator.h>
#include <khensu/targets/GenreDatabase.h>
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

using namespace khensu;
using Catch::Approx;

namespace {

/// Every genre the product ships (resources/genre_norms/genre_targets.json).
const std::vector<GenreEntry>& shippedGenres()
{
    static const std::vector<GenreEntry> genres = [] {
        GenreDatabase db;
        const auto loaded =
            db.loadFromFile(std::string(KHENSU_REPO_ROOT_DIR) + "/resources/genre_norms/genre_targets.json");
        return loaded.is_ok() ? db.listAll() : std::vector<GenreEntry>{};
    }();
    return genres;
}

/// A mix on the genre's centroid and tilt, so only the low-end rules can fire.
AnalysisResult mixWithLowEnd(const GenreEntry& genre, float balanceLow)
{
    AnalysisResult r;
    r.file_id = "file";
    r.spectral.centroid_hz = genre.spectral_centroid_hz;
    r.spectral.tilt_db_per_octave = genre.spectral_tilt_target_db_per_octave;
    r.spectral.balance_low = balanceLow;
    return r;
}

/// A mix on the genre's low-end share and tilt, so only the centroid rules can fire.
AnalysisResult mixWithCentroid(const GenreEntry& genre, float centroidHz)
{
    AnalysisResult r = mixWithLowEnd(genre, genre.low_end_weight);
    r.spectral.centroid_hz = centroidHz;
    return r;
}

/// A result the deep-analysis recommenders accept.
AnalysisResult deepResult()
{
    AnalysisResult r;
    r.file_id = "file";
    r.python_analysis_ran = true;
    return r;
}

std::optional<float> severityOf(const std::vector<Recommendation>& recs, const std::string& code)
{
    const std::string prefix = code + "_";
    for (const auto& r : recs)
        if (r.id.rfind(prefix, 0) == 0) return r.severity;
    return std::nullopt;
}

/// The threshold the evidence trail reports for a code - the one the card says it fired on.
float evidenceThreshold(const std::string& code, const GenreEntry& genre)
{
    const auto items = evidenceForCode(code, AnalysisResult{}, genre);
    if (items.empty() || !items.front().threshold.has_value()) return std::nanf("");
    return *items.front().threshold;
}

float tonalScore(const AnalysisResult& result, const GenreEntry& genre)
{
    const auto sc = buildMixScorecard(result, genre, {});
    for (const auto& c : sc.categories)
        if (c.id == "tonal_balance_eq") return c.score;
    return -1.0f;
}

GenreEntry genreWithLowEnd(float weight)
{
    GenreEntry g;
    g.id = "synthetic";
    g.low_end_weight = weight;
    return g;
}

} // namespace

// --- FIX-9: reachable low-end rules ---

TEST_CASE("low_end_heavy and low_end_light can fire for every shipped genre", "[spectral][fix9]")
{
    const auto& genres = shippedGenres();
    REQUIRE(genres.size() >= 100);

    SpectralRecommender rec;
    std::string heavyBlind, lightBlind, firesOnTarget, thresholdOutOfRange;
    for (const auto& g : genres) {
        // All the energy below 250 Hz / none of it: the two ends of the share.
        if (!severityOf(rec.recommend(mixWithLowEnd(g, 1.0f), g, CreativeTarget{}), "low_end_heavy"))
            heavyBlind += g.id + " ";
        if (!severityOf(rec.recommend(mixWithLowEnd(g, 0.0f), g, CreativeTarget{}), "low_end_light"))
            lightBlind += g.id + " ";
        if (!rec.recommend(mixWithLowEnd(g, g.low_end_weight), g, CreativeTarget{}).empty())
            firesOnTarget += g.id + " ";

        const float heavy = evidenceThreshold("low_end_heavy", g);
        const float light = evidenceThreshold("low_end_light", g);
        const bool heavyOk = heavy > g.low_end_weight && heavy < 1.0f;
        const bool lightOk = light < g.low_end_weight && light > 0.0f;
        if (!heavyOk || !lightOk) thresholdOutOfRange += g.id + " ";
    }

    {
        INFO("low_end_heavy cannot fire for: " << heavyBlind);
        CHECK(heavyBlind.empty());
    }
    {
        INFO("low_end_light cannot fire for: " << lightBlind);
        CHECK(lightBlind.empty());
    }
    {
        INFO("a low-end rule fires on a mix exactly on target for: " << firesOnTarget);
        CHECK(firesOnTarget.empty());
    }
    {
        INFO("low-end threshold not strictly between the target and the end of the share for: " << thresholdOutOfRange);
        CHECK(thresholdOutOfRange.empty());
    }
}

TEST_CASE("low-end rules stay reachable for any learned low-end target", "[spectral][fix9]")
{
    // UserPreferenceModel shifts low_end_weight by up to +/-0.30, so the
    // rules must hold beyond the weights the genre table ships.
    SpectralRecommender rec;
    for (int i = 1; i <= 99; ++i) {
        const float weight = static_cast<float>(i) / 100.0f;
        const GenreEntry g = genreWithLowEnd(weight);
        INFO("low_end_weight " << weight);
        CHECK(severityOf(rec.recommend(mixWithLowEnd(g, 1.0f), g, CreativeTarget{}), "low_end_heavy").has_value());
        CHECK(severityOf(rec.recommend(mixWithLowEnd(g, 0.0f), g, CreativeTarget{}), "low_end_light").has_value());
        CHECK(rec.recommend(mixWithLowEnd(g, weight), g, CreativeTarget{}).empty());
    }
}

TEST_CASE("the low-end band keeps the old 20 points at a 0.50 target and stays inside the share", "[spectral][fix9]")
{
    CHECK(evidenceThreshold("low_end_heavy", genreWithLowEnd(0.50f)) == Approx(0.70f).margin(1e-5));
    CHECK(evidenceThreshold("low_end_light", genreWithLowEnd(0.50f)) == Approx(0.30f).margin(1e-5));
    CHECK(evidenceThreshold("low_end_heavy", genreWithLowEnd(0.75f)) == Approx(0.875f).margin(1e-5));
    CHECK(evidenceThreshold("low_end_light", genreWithLowEnd(0.75f)) == Approx(0.5625f).margin(1e-5));
    CHECK(evidenceThreshold("low_end_heavy", genreWithLowEnd(0.90f)) == Approx(6.3f / 6.6f).margin(1e-5));
    CHECK(evidenceThreshold("low_end_light", genreWithLowEnd(0.10f)) == Approx(0.3f / 6.6f).margin(1e-5));

    for (const auto& g : shippedGenres()) {
        INFO(g.id << " low_end_weight " << g.low_end_weight);
        const float heavy = evidenceThreshold("low_end_heavy", g);
        const float light = evidenceThreshold("low_end_light", g);
        CHECK(heavy > g.low_end_weight);
        CHECK(heavy < 1.0f);
        CHECK(light < g.low_end_weight);
        CHECK(light > 0.0f);
    }
}

TEST_CASE("the low-end band is the same size of tilt for every genre", "[spectral][fix9]")
{
    // A share is not a level. What the band allows is how far the balance
    // between the lows and everything else may move, and that is a ratio:
    // 10*log10( (edge / (1 - edge)) / (target / (1 - target)) ) dB.
    // "target + 0.20" was 3.7 dB at a 0.50 target but 8.0 dB at 0.75 and
    // unreachable from 0.80, so bass-led genres were held to a far looser
    // rule than the rest - or to none. Every shipped genre now gets the same
    // 3.7 dB each way.
    const auto tiltDb = [](float edge, float target) {
        return 10.0f * std::log10((edge / (1.0f - edge)) / (target / (1.0f - target)));
    };
    const float bandDb = 10.0f * std::log10(7.0f / 3.0f); // 3.68 dB
    for (const auto& g : shippedGenres()) {
        INFO(g.id << " low_end_weight " << g.low_end_weight);
        CHECK(tiltDb(evidenceThreshold("low_end_heavy", g), g.low_end_weight) == Approx(bandDb).margin(0.02));
        CHECK(-tiltDb(evidenceThreshold("low_end_light", g), g.low_end_weight) == Approx(bandDb).margin(0.02));
    }
}

TEST_CASE("recommender, scorecard and evidence agree on the tonal rules for every genre", "[spectral][fix9][fix10]")
{
    const auto& genres = shippedGenres();
    REQUIRE(genres.size() >= 100);

    SpectralRecommender rec;
    int lowEndChecks = 0, centroidChecks = 0;
    std::string firstMismatch;
    auto note = [&firstMismatch](const std::string& what) {
        if (firstMismatch.empty()) firstMismatch = what;
    };

    for (const auto& g : genres) {
        const float heavyAt = evidenceThreshold("low_end_heavy", g);
        const float lightAt = evidenceThreshold("low_end_light", g);
        for (int i = 0; i <= 100; ++i) {
            const float share = static_cast<float>(i) / 100.0f;
            const auto result = mixWithLowEnd(g, share);
            const auto recs = rec.recommend(result, g, CreativeTarget{});
            const auto heavy = severityOf(recs, "low_end_heavy");
            const auto light = severityOf(recs, "low_end_light");
            if (heavy.has_value() != (share > heavyAt))
                note(g.id + ": low_end_heavy at share " + std::to_string(share));
            if (light.has_value() != (share < lightAt))
                note(g.id + ": low_end_light at share " + std::to_string(share));
            // Tonal Balance = 100 - (0.6 x centroid severity + 0.4 x low-end severity).
            const float expected = 100.0f - 0.4f * heavy.value_or(light.value_or(0.0f));
            if (std::abs(tonalScore(result, g) - expected) > 0.01f)
                note(g.id + ": scorecard low-end penalty at share " + std::to_string(share));
            ++lowEndChecks;
        }

        const float ceilingHz = evidenceThreshold("centroid_too_bright", g);
        const float floorHz = evidenceThreshold("centroid_too_dark", g);
        for (const float ratio : {0.25f, 0.5f, 0.7f, 0.76f, 1.0f, 1.24f, 1.3f, 1.6f, 2.5f, 5.0f}) {
            const float hz = g.spectral_centroid_hz * ratio;
            const auto result = mixWithCentroid(g, hz);
            const auto recs = rec.recommend(result, g, CreativeTarget{});
            const auto bright = severityOf(recs, "centroid_too_bright");
            const auto dark = severityOf(recs, "centroid_too_dark");
            if (bright.has_value() != (hz > ceilingHz))
                note(g.id + ": centroid_too_bright at x" + std::to_string(ratio));
            if (dark.has_value() != (hz < floorHz)) note(g.id + ": centroid_too_dark at x" + std::to_string(ratio));
            const float expected = 100.0f - 0.6f * bright.value_or(dark.value_or(0.0f));
            if (std::abs(tonalScore(result, g) - expected) > 0.01f)
                note(g.id + ": scorecard centroid penalty at x" + std::to_string(ratio));
            ++centroidChecks;
        }
    }

    INFO("first mismatch: " << firstMismatch);
    CHECK(firstMismatch.empty());
    CHECK(lowEndChecks > 0);
    CHECK(centroidChecks > 0);
}

// --- FIX-10: the same miss scores the same in every genre ---

TEST_CASE("a low-end miss scores the same in every shipped genre", "[spectral][fix10]")
{
    const auto& genres = shippedGenres();
    REQUIRE(genres.size() >= 100);

    // 0-1 share on a 0.5 full scale: 4 points past the edge = 8, 10 points = 20.
    SpectralRecommender rec;
    for (const auto& g : genres) {
        INFO(g.id << " low_end_weight " << g.low_end_weight);
        const float heavyAt = evidenceThreshold("low_end_heavy", g);
        const float lightAt = evidenceThreshold("low_end_light", g);

        const auto heavy =
            severityOf(rec.recommend(mixWithLowEnd(g, heavyAt + 0.04f), g, CreativeTarget{}), "low_end_heavy");
        REQUIRE(heavy.has_value());
        CHECK(*heavy == Approx(8.0f).margin(0.01));

        const auto light =
            severityOf(rec.recommend(mixWithLowEnd(g, lightAt - 0.10f), g, CreativeTarget{}), "low_end_light");
        REQUIRE(light.has_value());
        CHECK(*light == Approx(20.0f).margin(0.01));
    }
}

TEST_CASE("centroid severity is the distance past the genre window in octaves", "[spectral][fix10]")
{
    const auto& genres = shippedGenres();
    REQUIRE(genres.size() >= 100);

    // 1.5 octaves past the edge is full scale, so one octave = 66.7 and half
    // an octave = 33.3 - above the ceiling or below the floor, in any genre.
    // (Divided by the threshold, an octave over scored 100 and an octave under 50.)
    SpectralRecommender rec;
    const float halfOctave = std::sqrt(2.0f);
    for (const auto& g : genres) {
        INFO(g.id << " spectral_centroid_hz " << g.spectral_centroid_hz);
        const float ceilingHz = evidenceThreshold("centroid_too_bright", g);
        const float floorHz = evidenceThreshold("centroid_too_dark", g);
        auto bright = [&](float hz) {
            return severityOf(rec.recommend(mixWithCentroid(g, hz), g, CreativeTarget{}), "centroid_too_bright");
        };
        auto dark = [&](float hz) {
            return severityOf(rec.recommend(mixWithCentroid(g, hz), g, CreativeTarget{}), "centroid_too_dark");
        };

        REQUIRE(bright(ceilingHz * 2.0f).has_value());
        REQUIRE(dark(floorHz * 0.5f).has_value());
        CHECK(*bright(ceilingHz * 2.0f) == Approx(66.667f).margin(0.01));
        CHECK(*dark(floorHz * 0.5f) == Approx(66.667f).margin(0.01));
        CHECK(*bright(ceilingHz * halfOctave) == Approx(33.333f).margin(0.01));
        CHECK(*dark(floorHz / halfOctave) == Approx(33.333f).margin(0.01));
        CHECK(*bright(ceilingHz * 4.0f) == Approx(100.0f));
        CHECK(*dark(floorHz * 0.25f) == Approx(100.0f));
    }
}

TEST_CASE("hook findings score a shortfall the same at every threshold", "[hook][fix10]")
{
    HookRecommender rec;

    SECTION("hook_strength_low: 0.10 short = 20 at any hook emphasis")
    {
        for (const float emphasis : {0.0f, 50.0f, 100.0f}) {
            CreativeTarget target;
            target.hook_emphasis = emphasis;
            auto result = deepResult();
            result.python_data.hooks.hook_count = 3;
            result.python_data.hooks.hook_density = 1.0f;
            result.python_data.hooks.hook_strength = TargetInterpolator::hookStrengthThreshold(target) - 0.10f;
            INFO("hook_emphasis " << emphasis);
            const auto sev = severityOf(rec.recommend(result, GenreEntry{}, target), "hook_strength_low");
            REQUIRE(sev.has_value());
            CHECK(*sev == Approx(20.0f).margin(0.01));
        }
    }

    SECTION("hook_density_low: 0.15 short = 30 whatever the genre's density norm")
    {
        for (const float norm : {0.30f, 0.60f, 0.90f}) {
            GenreEntry genre;
            genre.hook_density_norm = norm;
            auto result = deepResult();
            result.python_data.hooks.hook_count = 3;
            result.python_data.hooks.hook_strength = 1.0f;
            result.python_data.hooks.hook_density = norm - 0.15f;
            INFO("hook_density_norm " << norm);
            const auto sev = severityOf(rec.recommend(result, genre, CreativeTarget{}), "hook_density_low");
            REQUIRE(sev.has_value());
            CHECK(*sev == Approx(30.0f).margin(0.01));
        }
    }
}

TEST_CASE("memorability findings score a shortfall the same at every threshold", "[memorability][fix10]")
{
    MemorabilityRecommender rec;
    for (const float t : {0.0f, 50.0f, 100.0f}) {
        CreativeTarget target;
        target.memorability_target = t;
        INFO("memorability_target " << t);

        auto result = deepResult();
        result.python_data.hooks.hook_count = 2;
        result.python_data.memorability.score = TargetInterpolator::memorabilityThreshold(target) - 0.10f;
        result.python_data.memorability.earworm_likelihood = TargetInterpolator::earwormThreshold(target) - 0.05f;
        const auto recs = rec.recommend(result, GenreEntry{}, target);

        const auto memorability = severityOf(recs, "memorability_low");
        REQUIRE(memorability.has_value());
        CHECK(*memorability == Approx(20.0f).margin(0.01));

        const auto earworm = severityOf(recs, "earworm_likelihood_low");
        REQUIRE(earworm.has_value());
        CHECK(*earworm == Approx(10.0f).margin(0.01));
    }
}

TEST_CASE("structural_flow_poor scores the arc shortfall on the 0-1 score scale", "[flow][fix10]")
{
    StructuralFlowRecommender rec;
    auto result = deepResult();
    result.python_data.structural_flow.arc_quality = 0.30f; // 0.20 under the 0.50 floor
    const auto sev = severityOf(rec.recommend(result, GenreEntry{}, CreativeTarget{}), "structural_flow_poor");
    REQUIRE(sev.has_value());
    CHECK(*sev == Approx(40.0f).margin(0.01));
}

TEST_CASE("heaviness findings score a miss the same whatever the genre norm", "[heaviness][fix10]")
{
    HeavinessRecommender rec;

    // TUNE-8: the bands are wide (odds 10 and 8), so the misses are scores
    // near the ends; the severity is still |score - norm| / 0.5.
    SECTION("heaviness_extremity_mismatch: severity is the distance on the 0-1 scale, either side")
    {
        for (const float norm : {0.30f, 0.50f, 0.70f}) {
            GenreEntry genre;
            genre.heaviness_extremity_norm = norm;
            for (const float score : {0.98f, 0.02f}) {
                const float miss = score - norm;
                auto result = deepResult();
                result.python_data.heaviness.extremity = score;
                result.python_data.heaviness.weight = 1.0f;
                result.python_data.heaviness.distortion = genre.heaviness_distortion_norm;
                INFO("heaviness_extremity_norm " << norm << " miss " << miss);
                const auto sev =
                    severityOf(rec.recommend(result, genre, CreativeTarget{}), "heaviness_extremity_mismatch");
                REQUIRE(sev.has_value());
                CHECK(*sev == Approx(std::min(100.0f, std::abs(miss) / 0.5f * 100.0f)).margin(0.01));
            }
        }
    }

    SECTION("heaviness_distortion_mismatch: severity is the distance on the 0-1 scale, either side")
    {
        for (const float norm : {0.30f, 0.50f, 0.70f}) {
            GenreEntry genre;
            genre.heaviness_distortion_norm = norm;
            for (const float score : {0.98f, 0.02f}) {
                const float miss = score - norm;
                auto result = deepResult();
                result.python_data.heaviness.extremity = genre.heaviness_extremity_norm;
                result.python_data.heaviness.weight = 1.0f;
                result.python_data.heaviness.distortion = score;
                INFO("heaviness_distortion_norm " << norm << " miss " << miss);
                const auto sev =
                    severityOf(rec.recommend(result, genre, CreativeTarget{}), "heaviness_distortion_mismatch");
                REQUIRE(sev.has_value());
                CHECK(*sev == Approx(std::min(100.0f, std::abs(miss) / 0.5f * 100.0f)).margin(0.01));
            }
        }
    }

    SECTION("heaviness_weight_low: 0.10 short = 20 whatever the genre's weight norm")
    {
        CreativeTarget target;
        target.heaviness_target = 75.0f;
        for (const float norm : {0.40f, 0.60f, 0.80f}) {
            GenreEntry genre;
            genre.heaviness_weight_norm = norm;
            auto result = deepResult();
            result.python_data.heaviness.extremity = TargetInterpolator::heavinessExtremityThreshold(genre, target);
            result.python_data.heaviness.distortion = genre.heaviness_distortion_norm;
            result.python_data.heaviness.weight = TargetInterpolator::heavinessWeightThreshold(genre, target) - 0.10f;
            INFO("heaviness_weight_norm " << norm);
            const auto sev = severityOf(rec.recommend(result, genre, target), "heaviness_weight_low");
            REQUIRE(sev.has_value());
            CHECK(*sev == Approx(20.0f).margin(0.01));
        }
    }
}

// --- FIX-9 (rest): the heaviness bands are reachable for every genre ---

TEST_CASE("heaviness extremity and distortion can fire both ways for every shipped genre", "[heaviness][fix9]")
{
    const auto& genres = shippedGenres();
    REQUIRE(genres.size() >= 100);
    HeavinessRecommender rec;
    std::string blind;
    for (const auto& g : genres) {
        auto mix = [&](float extremity, float distortion) {
            AnalysisResult r = deepResult();
            r.python_data.heaviness.extremity = extremity;
            r.python_data.heaviness.distortion = distortion;
            r.python_data.heaviness.weight = 1.0f;
            return rec.recommend(r, g, CreativeTarget{});
        };
        const float e = g.heaviness_extremity_norm, d = g.heaviness_distortion_norm;
        if (!severityOf(mix(1.0f, d), "heaviness_extremity_mismatch")) blind += g.id + ":ext-high ";
        if (!severityOf(mix(0.0f, d), "heaviness_extremity_mismatch")) blind += g.id + ":ext-low ";
        if (!severityOf(mix(e, 1.0f), "heaviness_distortion_mismatch")) blind += g.id + ":dist-high ";
        if (!severityOf(mix(e, 0.0f), "heaviness_distortion_mismatch")) blind += g.id + ":dist-low ";
        if (severityOf(mix(e, d), "heaviness_extremity_mismatch") || severityOf(mix(e, d), "heaviness_distortion_mismatch"))
            blind += g.id + ":fires-on-target ";
    }
    INFO("unreachable or always-on: " << blind);
    CHECK(blind.empty());
}

TEST_CASE("score thresholds stay where a score can sit on either side", "[interpolator][fix9]")
{
    for (const auto& g : shippedGenres()) {
        for (const float t : {0.0f, 50.0f, 100.0f}) {
            CreativeTarget target;
            target.hook_emphasis = t;
            target.heaviness_target = t;
            for (const float th : {TargetInterpolator::hookDensityThreshold(g, target),
                                   TargetInterpolator::heavinessExtremityThreshold(g, target),
                                   TargetInterpolator::heavinessWeightThreshold(g, target)}) {
                INFO(g.id << " target " << t);
                CHECK(th >= 0.05f);
                CHECK(th <= 0.95f);
            }
        }
    }
}
