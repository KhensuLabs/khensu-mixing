// tests/shared-core/test_learned_preferences.cpp
// "What Khensu has learned": the evidence log behind UserPreferenceModel's
// offsets, the plain-language description built from it, and reset - which
// must forget only the four learned offsets, never UI state, score weights
// or other keys that happen to share recommendation_prefs.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <khensu/feedback/LearnedPreferences.h>
#include <khensu/targets/UserPreferenceModel.h>

using namespace khensu;
using Catch::Matchers::WithinAbs;

namespace {
const LearnedPreference* find(const std::vector<LearnedPreference>& v, const std::string& key)
{
    for (const auto& p : v)
        if (p.pref_key == key) return &p;
    return nullptr;
}
} // namespace

TEST_CASE("Learned preferences: nothing learned yet describes nothing", "[learnedprefs]")
{
    FeedbackManager fm(":memory:");
    fm.setPreference("sort_by_severity", 1.0f); // UI state, not learned
    CHECK(learned_preferences::describe(fm).empty());
}

TEST_CASE("Learned preferences: rejects are counted and explained", "[learnedprefs]")
{
    FeedbackManager fm(":memory:");
    for (int i = 0; i < 4; ++i)
        CHECK(learned_preferences::learnFromRecommendation(fm, "lufs_too_loud_" + std::to_string(i), false));
    CHECK(learned_preferences::learnFromRecommendation(fm, "lufs_too_quiet_x", true));
    // A type the model doesn't learn from changes nothing and logs nothing.
    CHECK_FALSE(learned_preferences::learnFromRecommendation(fm, "sibilance_harsh_1", false));

    const auto prefs = learned_preferences::describe(fm);
    REQUIRE(prefs.size() == 1);
    const auto& p = prefs.front();
    CHECK(p.pref_key == "pref_lufs_offset");
    CHECK(p.title == "Loudness");
    CHECK(p.rejects == 4);
    CHECK(p.accepts == 1);
    CHECK(p.ab_picks == 0);
    CHECK_THAT(p.offset, WithinAbs(1.0, 1e-4)); // 4 x 0.2 + 0.2
    CHECK(p.offset_text == "1.0 dB louder");
    CHECK_FALSE(p.last_updated.empty());
    CHECK(p.explanation ==
          "You rejected 4 'too loud' warnings and accepted 1 'too quiet' warning, so loudness targets now "
          "sit 1.0 dB louder than the genre default.");

    // The learned value is what recommendations actually use.
    UserPreferenceModel model;
    model.loadFromDatabase(fm);
    CHECK_THAT(model.getLufsOffset(), WithinAbs(1.0, 1e-4));
}

TEST_CASE("Learned preferences: A/B picks are evidence too", "[learnedprefs]")
{
    FeedbackManager fm(":memory:");
    fm.setPreference("pref_lufs_offset", 0.5f);
    learned_preferences::recordAbPick(fm, true, 0.0f, 0.5f);
    learned_preferences::recordAbPick(fm, true, 0.5f, 0.5f); // no movement: not logged

    const auto prefs = learned_preferences::describe(fm);
    const auto* p = find(prefs, "pref_lufs_offset");
    REQUIRE(p != nullptr);
    CHECK(p->ab_picks == 1);
    CHECK(p->explanation.find("picked the processed render in 1 A/B comparison") != std::string::npos);
}

TEST_CASE("Learned preferences: an offset learned before the log existed is still shown", "[learnedprefs]")
{
    FeedbackManager fm(":memory:");
    fm.setPreference("pref_stereo_width_offset", -0.1f);
    const auto prefs = learned_preferences::describe(fm);
    REQUIRE(prefs.size() == 1);
    CHECK(prefs.front().offset_text == "0.10 narrower");
    CHECK(prefs.front().explanation.rfind("Learned from earlier choices", 0) == 0);
}

TEST_CASE("Learned preferences: reset forgets one offset and only learned keys", "[learnedprefs]")
{
    FeedbackManager fm(":memory:");
    learned_preferences::learnFromRecommendation(fm, "lufs_too_loud", false);
    learned_preferences::learnFromRecommendation(fm, "stereo_too_wide", false);
    // Neighbours in recommendation_prefs that are NOT learned taste.
    fm.setPreference("sort_by_severity", 0.0f);
    fm.setPreference("stem_pan_vocals", 0.25f);
    fm.setPreference("score_weight_scorecard_fx_weight_trap", 0.3f);
    REQUIRE(learned_preferences::describe(fm).size() == 2);

    REQUIRE(learned_preferences::reset(fm, "pref_lufs_offset").is_ok());
    auto prefs = learned_preferences::describe(fm);
    REQUIRE(prefs.size() == 1);
    CHECK(prefs.front().pref_key == "pref_stereo_width_offset");
    CHECK(prefs.front().rejects == 1); // its evidence survived
    CHECK(fm.getPreference("pref_lufs_offset", 7.0f).value() == 7.0f); // row gone -> default

    CHECK_FALSE(learned_preferences::reset(fm, "sort_by_severity").is_ok()); // not a learned key

    REQUIRE(learned_preferences::resetAll(fm).is_ok());
    CHECK(learned_preferences::describe(fm).empty());
    auto events = fm.listLearningEvents();
    REQUIRE(events.is_ok());
    CHECK(events.value().empty());

    CHECK(fm.getPreference("sort_by_severity", 1.0f).value() == 0.0f);
    CHECK(fm.getPreference("stem_pan_vocals", 0.0f).value() == 0.25f);
    CHECK_THAT(fm.getPreference("score_weight_scorecard_fx_weight_trap", 0.0f).value(), WithinAbs(0.3, 1e-6));
}

TEST_CASE("Learned preferences: deleteAll also erases the evidence log", "[learnedprefs]")
{
    FeedbackManager fm(":memory:");
    learned_preferences::learnFromRecommendation(fm, "low_end_heavy", false);
    REQUIRE(fm.listLearningEvents().value().size() == 1);
    REQUIRE(fm.deleteAll().is_ok());
    CHECK(fm.listLearningEvents().value().empty());
}
