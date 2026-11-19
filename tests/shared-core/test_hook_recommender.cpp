#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <khensu/recommendations/HookRecommender.h>

using namespace khensu;
using Catch::Matchers::WithinAbs;

static GenreEntry makeGenre()
{
    GenreEntry g;
    g.id = "trap";
    g.hook_density_norm = 0.70f;
    return g;
}

static AnalysisResult makeResult(bool withPython = true)
{
    AnalysisResult r;
    r.file_id = "550e8400-e29b-41d4-a716-446655440000";
    if (withPython) {
        r.python_data.bpm_detected = 140.0f; // mark as python-analyzed
        r.python_data.hooks.hook_count = 3;
        r.python_data.hooks.hook_density = 0.75f;
        r.python_data.hooks.hook_strength = 0.60f;
    }
    return r;
}

static HookRecommender rec;

TEST_CASE("HookRecommender: quick-mode guard returns empty when no python data", "[hook]")
{
    auto result = makeResult(false); // no python_data
    auto findings = rec.recommend(result, makeGenre(), CreativeTarget{});
    CHECK(findings.empty());
}

TEST_CASE("HookRecommender: no findings when hooks in range at default target", "[hook]")
{
    // hook_density=0.75 vs threshold=0.40 (hook_density_norm-0.30=0.40 at t=50) → no firing
    // hook_strength=0.60 vs threshold=0.55 (norm at t=50) → no firing
    auto result = makeResult();
    result.python_data.hooks.hook_density = 0.75f;
    result.python_data.hooks.hook_strength = 0.60f;
    auto findings = rec.recommend(result, makeGenre(), CreativeTarget{});
    CHECK(findings.empty());
}

TEST_CASE("HookRecommender: no_hooks_detected fires when hook_count == 0", "[hook]")
{
    auto result = makeResult();
    result.python_data.hooks.hook_count = 0;
    result.python_data.hooks.hook_density = 0.0f;
    result.python_data.hooks.hook_strength = 0.0f;
    auto findings = rec.recommend(result, makeGenre(), CreativeTarget{});
    REQUIRE(findings.size() == 1);
    CHECK(findings[0].id.rfind("no_hooks_detected_", 0) == 0);
    CHECK_THAT(findings[0].severity, WithinAbs(85.0f, 0.001f));
    CHECK_THAT(findings[0].confidence, WithinAbs(0.70f, 0.001f));
    CHECK(findings[0].stem_or_bus == "Hook");
}

TEST_CASE("HookRecommender: hook_density_low fires when density below threshold", "[hook]")
{
    auto result = makeResult();
    result.python_data.hooks.hook_density = 0.30f;  // below 0.40 (threshold at t=50)
    result.python_data.hooks.hook_strength = 0.60f; // ok
    auto findings = rec.recommend(result, makeGenre(), CreativeTarget{});
    REQUIRE(findings.size() == 1);
    CHECK(findings[0].id.rfind("hook_density_low_", 0) == 0);
    CHECK(findings[0].is_target_relative == false); // default hook_emphasis=50
}

TEST_CASE("HookRecommender: is_target_relative set when hook_emphasis != 50", "[hook]")
{
    auto result = makeResult();
    result.python_data.hooks.hook_density = 0.30f;
    result.python_data.hooks.hook_strength = 0.60f;
    CreativeTarget tgt;
    tgt.hook_emphasis = 80.0f; // non-default
    auto findings = rec.recommend(result, makeGenre(), tgt);
    REQUIRE(!findings.empty());
    CHECK(findings[0].is_target_relative == true);
}

TEST_CASE("HookRecommender: hook_strength_low fires and sets time_range from weakest instance", "[hook]")
{
    auto result = makeResult();
    result.python_data.hooks.hook_density = 0.75f;  // ok
    result.python_data.hooks.hook_strength = 0.40f; // below 0.55 threshold
    // Add instances so time_range can be populated
    HookInstance strong, weak;
    strong.start_sec = 10.0f;
    strong.end_sec = 15.0f;
    strong.strength = 0.80f;
    weak.start_sec = 30.0f;
    weak.end_sec = 35.0f;
    weak.strength = 0.30f;
    result.python_data.hooks.instances = {strong, weak};
    auto findings = rec.recommend(result, makeGenre(), CreativeTarget{});
    REQUIRE(findings.size() == 1);
    CHECK(findings[0].id.rfind("hook_strength_low_", 0) == 0);
    REQUIRE(findings[0].time_range.has_value());
    CHECK_THAT(findings[0].time_range->start_sec, WithinAbs(30.0f, 0.001f));
    CHECK_THAT(findings[0].time_range->end_sec, WithinAbs(35.0f, 0.001f));
}

// 2026-11-18: these three recommenders built findings with no text, so the
// Mix / Master card drew an empty row and Issues showed no advice. Every
// finding they raise must say what was found and what to do.
#include <khensu/recommendations/MemorabilityRecommender.h>
#include <khensu/recommendations/StructuralFlowRecommender.h>

TEST_CASE("Song-level findings all carry their text", "[hook][memorability][structure]")
{
    std::vector<Recommendation> all;
    auto noHooks = makeResult();
    noHooks.python_data.hooks.hook_count = 0;
    for (auto& r : rec.recommend(noHooks, makeGenre(), CreativeTarget{})) all.push_back(r);

    auto weakHooks = makeResult();
    weakHooks.python_data.hooks.hook_density = 0.05f;
    weakHooks.python_data.hooks.hook_strength = 0.05f;
    for (auto& r : rec.recommend(weakHooks, makeGenre(), CreativeTarget{})) all.push_back(r);

    auto low = makeResult();
    low.python_data.memorability.score = 0.0f;
    low.python_data.memorability.earworm_likelihood = 0.0f;
    low.python_data.structural_flow.arc_quality = 0.0f;
    for (auto& r : MemorabilityRecommender{}.recommend(low, makeGenre(), CreativeTarget{})) all.push_back(r);
    for (auto& r : StructuralFlowRecommender{}.recommend(low, makeGenre(), CreativeTarget{})) all.push_back(r);

    REQUIRE(all.size() == 6); // no_hooks, density, strength, memorability, earworm, arc
    for (const auto& r : all) {
        INFO(r.id);
        CHECK_FALSE(r.what.empty());
        CHECK_FALSE(r.suggested_action.empty());
        CHECK_FALSE(r.why_helps.empty());
        CHECK_FALSE(r.how_to_verify.empty());
    }
}
