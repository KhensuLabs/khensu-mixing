#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <khensu/recommendations/TargetInterpolator.h>

using namespace khensu;
using Catch::Matchers::WithinAbs;

static GenreEntry makeGenre()
{
    GenreEntry g;
    g.id = "trap";
    g.name = "Trap";
    g.hook_density_norm = 0.70f;
    g.heaviness_extremity_norm = 0.50f;
    g.heaviness_weight_norm = 0.60f;
    return g;
}

static CreativeTarget makeTarget(float hook = 50.0f, float heaviness = 50.0f, float memorability = 50.0f)
{
    CreativeTarget t;
    t.hook_emphasis = hook;
    t.heaviness_target = heaviness;
    t.memorability_target = memorability;
    return t;
}

// ── interpolate core ──────────────────────────────────────────────────────

TEST_CASE("TargetInterpolator::interpolate boundary values", "[interpolator]")
{
    SECTION("t=0 returns floor")
    {
        CHECK_THAT(TargetInterpolator::interpolate(0.0f, 0.2f, 0.5f, 0.8f), WithinAbs(0.2f, 0.001f));
    }
    SECTION("t=50 returns norm")
    {
        CHECK_THAT(TargetInterpolator::interpolate(50.0f, 0.2f, 0.5f, 0.8f), WithinAbs(0.5f, 0.001f));
    }
    SECTION("t=100 returns ceil")
    {
        CHECK_THAT(TargetInterpolator::interpolate(100.0f, 0.2f, 0.5f, 0.8f), WithinAbs(0.8f, 0.001f));
    }
    SECTION("t=25 is midpoint of floor..norm")
    {
        // t=25: floor + (norm - floor) * 0.5 = 0.2 + 0.3*0.5 = 0.35
        CHECK_THAT(TargetInterpolator::interpolate(25.0f, 0.2f, 0.5f, 0.8f), WithinAbs(0.35f, 0.001f));
    }
    SECTION("t=75 is midpoint of norm..ceil")
    {
        // t=75: norm + (ceil - norm) * 0.5 = 0.5 + 0.3*0.5 = 0.65
        CHECK_THAT(TargetInterpolator::interpolate(75.0f, 0.2f, 0.5f, 0.8f), WithinAbs(0.65f, 0.001f));
    }
}

// ── hookDensityThreshold ──────────────────────────────────────────────────

TEST_CASE("TargetInterpolator::hookDensityThreshold", "[interpolator]")
{
    GenreEntry g = makeGenre(); // hook_density_norm = 0.70

    SECTION("t=50 returns genre norm")
    {
        // floor = 0.70 - 0.30 = 0.40, norm = 0.70, ceil = 0.80
        // t=50 → norm = 0.70
        auto t = makeTarget(50.0f);
        CHECK_THAT(TargetInterpolator::hookDensityThreshold(g, t), WithinAbs(0.70f, 0.001f));
    }
    SECTION("t=0 returns floor (most lenient)")
    {
        // t=0 → 0.40
        auto t = makeTarget(0.0f);
        CHECK_THAT(TargetInterpolator::hookDensityThreshold(g, t), WithinAbs(0.40f, 0.001f));
    }
    SECTION("t=100 returns ceil (strictest)")
    {
        // t=100 → 0.80
        auto t = makeTarget(100.0f);
        CHECK_THAT(TargetInterpolator::hookDensityThreshold(g, t), WithinAbs(0.80f, 0.001f));
    }
}

// ── hookStrengthThreshold ─────────────────────────────────────────────────

TEST_CASE("TargetInterpolator::hookStrengthThreshold", "[interpolator]")
{
    SECTION("t=50 returns norm 0.55")
    {
        auto t = makeTarget(50.0f);
        CHECK_THAT(TargetInterpolator::hookStrengthThreshold(t), WithinAbs(0.55f, 0.001f));
    }
    SECTION("t=0 returns floor 0.35")
    {
        auto t = makeTarget(0.0f);
        CHECK_THAT(TargetInterpolator::hookStrengthThreshold(t), WithinAbs(0.35f, 0.001f));
    }
    SECTION("t=100 returns ceil 0.70")
    {
        auto t = makeTarget(100.0f);
        CHECK_THAT(TargetInterpolator::hookStrengthThreshold(t), WithinAbs(0.70f, 0.001f));
    }
}

// ── memorabilityThreshold ─────────────────────────────────────────────────

TEST_CASE("TargetInterpolator::memorabilityThreshold", "[interpolator]")
{
    SECTION("t=50 returns norm 0.50")
    {
        auto t = makeTarget(50.0f, 50.0f, 50.0f);
        CHECK_THAT(TargetInterpolator::memorabilityThreshold(t), WithinAbs(0.65f, 0.001f));
    }
    SECTION("t=0 returns floor 0.30")
    {
        auto t = makeTarget(50.0f, 50.0f, 0.0f);
        CHECK_THAT(TargetInterpolator::memorabilityThreshold(t), WithinAbs(0.45f, 0.001f));
    }
    SECTION("t=100 returns ceil 0.70")
    {
        auto t = makeTarget(50.0f, 50.0f, 100.0f);
        CHECK_THAT(TargetInterpolator::memorabilityThreshold(t), WithinAbs(0.85f, 0.001f));
    }
}

// ── earwormThreshold ──────────────────────────────────────────────────────

TEST_CASE("TargetInterpolator::earwormThreshold", "[interpolator]")
{
    SECTION("t=50 returns norm 0.40")
    {
        auto t = makeTarget(50.0f, 50.0f, 50.0f);
        CHECK_THAT(TargetInterpolator::earwormThreshold(t), WithinAbs(0.50f, 0.001f));
    }
    SECTION("t=0 returns floor 0.25")
    {
        auto t = makeTarget(50.0f, 50.0f, 0.0f);
        CHECK_THAT(TargetInterpolator::earwormThreshold(t), WithinAbs(0.35f, 0.001f));
    }
    SECTION("t=100 returns ceil 0.60")
    {
        auto t = makeTarget(50.0f, 50.0f, 100.0f);
        CHECK_THAT(TargetInterpolator::earwormThreshold(t), WithinAbs(0.70f, 0.001f));
    }
}

// ── heavinessExtremityThreshold ───────────────────────────────────────────

TEST_CASE("TargetInterpolator::heavinessExtremityThreshold", "[interpolator]")
{
    GenreEntry g = makeGenre(); // heaviness_extremity_norm = 0.50

    SECTION("t=50 returns genre norm")
    {
        // floor = 0.30, norm = 0.50, ceil = 0.70
        auto t = makeTarget(50.0f, 50.0f);
        CHECK_THAT(TargetInterpolator::heavinessExtremityThreshold(g, t), WithinAbs(0.50f, 0.001f));
    }
    SECTION("t=0 returns floor (norm - 0.20)")
    {
        auto t = makeTarget(50.0f, 0.0f);
        CHECK_THAT(TargetInterpolator::heavinessExtremityThreshold(g, t), WithinAbs(0.30f, 0.001f));
    }
    SECTION("t=100 returns ceil (norm + 0.20)")
    {
        auto t = makeTarget(50.0f, 100.0f);
        CHECK_THAT(TargetInterpolator::heavinessExtremityThreshold(g, t), WithinAbs(0.70f, 0.001f));
    }
}

// ── heavinessWeightThreshold ──────────────────────────────────────────────

TEST_CASE("TargetInterpolator::heavinessWeightThreshold", "[interpolator]")
{
    GenreEntry g = makeGenre(); // heaviness_weight_norm = 0.60

    SECTION("t=50 returns genre norm")
    {
        // floor = 0.35, norm = 0.60, ceil = 0.75
        auto t = makeTarget(50.0f, 50.0f);
        CHECK_THAT(TargetInterpolator::heavinessWeightThreshold(g, t), WithinAbs(0.60f, 0.001f));
    }
    SECTION("t=0 returns floor (norm - 0.25)")
    {
        auto t = makeTarget(50.0f, 0.0f);
        CHECK_THAT(TargetInterpolator::heavinessWeightThreshold(g, t), WithinAbs(0.35f, 0.001f));
    }
    SECTION("t=100 returns ceil (norm + 0.15)")
    {
        auto t = makeTarget(50.0f, 100.0f);
        CHECK_THAT(TargetInterpolator::heavinessWeightThreshold(g, t), WithinAbs(0.75f, 0.001f));
    }
}

TEST_CASE("CreativeTarget new session-control fields have safe defaults", "[target][sp16a]")
{
    CreativeTarget t;
    CHECK(t.confidence_threshold == 0.0f);
    CHECK(t.run_deep_analysis);
    CHECK(t.reference_track_paths.empty());
}


TEST_CASE("TargetInterpolator::arcQualityThreshold follows chorus payoff", "[target_interpolator][intent]")
{
    CreativeTarget t;
    t.chorus_payoff = 50.0f;
    CHECK_THAT(TargetInterpolator::arcQualityThreshold(t), WithinAbs(0.50, 1e-5)); // historical default
    t.chorus_payoff = 0.0f;
    CHECK_THAT(TargetInterpolator::arcQualityThreshold(t), WithinAbs(0.35, 1e-5));
    t.chorus_payoff = 100.0f;
    CHECK_THAT(TargetInterpolator::arcQualityThreshold(t), WithinAbs(0.65, 1e-5));
}
