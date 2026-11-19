#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <khensu/recommendations/LoudnessRecommender.h>

using namespace khensu;
using Catch::Matchers::WithinAbs;

static GenreEntry makeGenre()
{
    GenreEntry g;
    g.id = "trap";
    g.lufs_range_min = -12.0f;
    g.lufs_range_max = -7.0f;
    g.lra_range_min = 4.0f;
    g.lra_range_max = 9.0f;
    g.true_peak_max = -1.0f;
    return g;
}

static AnalysisResult makeResult(float lufs, float lra, float tp)
{
    AnalysisResult r;
    r.file_id = "550e8400-e29b-41d4-a716-446655440000";
    r.loudness.lufs_integrated = lufs;
    r.loudness.lra = lra;
    r.loudness.true_peak_dbtp = tp;
    return r;
}

static LoudnessRecommender rec;

TEST_CASE("LoudnessRecommender: no findings when in range", "[loudness]")
{
    auto result = makeResult(-9.0f, 6.5f, -1.5f);
    auto findings = rec.recommend(result, makeGenre(), CreativeTarget{});
    CHECK(findings.empty());
}

TEST_CASE("LoudnessRecommender: lufs_too_loud fires and has correct id", "[loudness]")
{
    auto result = makeResult(-5.0f, 6.5f, -1.5f); // -5 > -7 (max)
    auto findings = rec.recommend(result, makeGenre(), CreativeTarget{});
    REQUIRE(findings.size() == 1);
    CHECK(findings[0].id == "lufs_too_loud_550e8400-e29b-41d4-a716-446655440000");
    CHECK(findings[0].confidence == Catch::Approx(0.90f));
    CHECK(findings[0].severity > 0.0f);
    CHECK(findings[0].is_target_relative == false);
    CHECK(findings[0].suggested_parameters.count("reduce_gain_db") == 1);
}

TEST_CASE("LoudnessRecommender: lufs_too_quiet fires", "[loudness]")
{
    auto result = makeResult(-15.0f, 6.5f, -1.5f); // -15 < -12 (min)
    auto findings = rec.recommend(result, makeGenre(), CreativeTarget{});
    REQUIRE(findings.size() == 1);
    CHECK(findings[0].id.rfind("lufs_too_quiet_", 0) == 0);
    CHECK(findings[0].suggested_parameters.count("increase_gain_db") == 1);
}

TEST_CASE("LoudnessRecommender: a quiet but normalisable master is a minor note (MEAS-4)", "[loudness]")
{
    // Genre floor -12: -15 LUFS is under it but above the -16 LUFS reference.
    auto findings = rec.recommend(makeResult(-15.0f, 6.5f, -1.5f), makeGenre(), CreativeTarget{});
    REQUIRE(findings.size() == 1);
    CHECK_THAT(findings[0].severity, WithinAbs(LoudnessRecommender::kQuietChoiceSeverity, 0.001f));
    // Under -16 LUFS it rises from the minor note, graded from -16 (12 dB = full).
    findings = rec.recommend(makeResult(-22.0f, 6.5f, -7.0f), makeGenre(), CreativeTarget{});
    REQUIRE(findings.size() == 1);
    CHECK_THAT(findings[0].severity, WithinAbs(20.0f + 0.8f * 50.0f, 0.01f));
    // A chosen playback intent is the user's own window: graded in full.
    CreativeTarget headphone;
    headphone.playback_intent = "headphone"; // -16 +/- 1 LUFS
    findings = rec.recommend(makeResult(-19.0f, 6.5f, -4.0f), makeGenre(), headphone);
    REQUIRE(!findings.empty());
    CHECK_THAT(findings[0].severity, WithinAbs(100.0f * 2.0f / 12.0f, 0.01f));
    // A genre whose floor is already under -16 keeps its own grading.
    auto quietGenre = makeGenre();
    quietGenre.lufs_range_min = -20.0f;
    findings = rec.recommend(makeResult(-23.0f, 6.5f, -8.0f), quietGenre, CreativeTarget{});
    REQUIRE(findings.size() == 1);
    CHECK_THAT(findings[0].severity, WithinAbs(25.0f, 0.01f));
}

TEST_CASE("LoudnessRecommender: lra_too_compressed fires", "[loudness]")
{
    auto result = makeResult(-9.0f, 2.0f, -1.5f); // 2.0 < 4.0 (min)
    auto findings = rec.recommend(result, makeGenre(), CreativeTarget{});
    REQUIRE(findings.size() == 1);
    CHECK(findings[0].id.rfind("lra_too_compressed_", 0) == 0);
}

TEST_CASE("LoudnessRecommender: lra_too_dynamic fires", "[loudness]")
{
    auto result = makeResult(-9.0f, 14.0f, -1.5f); // 14.0 > 9.0 (max)
    auto findings = rec.recommend(result, makeGenre(), CreativeTarget{});
    REQUIRE(findings.size() == 1);
    CHECK(findings[0].id.rfind("lra_too_dynamic_", 0) == 0);
}

TEST_CASE("LoudnessRecommender: true_peak_over is minor up to 0 dBTP and grows past it (MEAS-4)", "[loudness]")
{
    // Over the -1 dBTP ceiling but legal: encoder headroom, a minor note.
    auto findings = rec.recommend(makeResult(-9.0f, 6.5f, -0.5f), makeGenre(), CreativeTarget{});
    REQUIRE(findings.size() == 1);
    CHECK(findings[0].id.rfind("true_peak_over_", 0) == 0);
    CHECK_THAT(findings[0].severity, WithinAbs(LoudnessRecommender::kTruePeakHeadroomSeverity, 0.001f));
    // Above 0 dBTP: an over, 50 + 15 per dB, capped at 95.
    findings = rec.recommend(makeResult(-9.0f, 6.5f, 0.5f), makeGenre(), CreativeTarget{});
    REQUIRE(findings.size() == 1);
    CHECK_THAT(findings[0].severity, WithinAbs(57.5f, 0.001f));
    findings = rec.recommend(makeResult(-9.0f, 6.5f, 4.0f), makeGenre(), CreativeTarget{});
    CHECK_THAT(findings[0].severity, WithinAbs(95.0f, 0.001f));
}

TEST_CASE("LoudnessRecommender: is_target_relative is always false", "[loudness]")
{
    // Set a non-default CreativeTarget - should make no difference for loudness
    CreativeTarget tgt;
    tgt.hook_emphasis = 100.0f;
    auto result = makeResult(-5.0f, 6.5f, -1.5f);
    auto findings = rec.recommend(result, makeGenre(), tgt);
    REQUIRE(!findings.empty());
    for (const auto& f : findings)
        CHECK(f.is_target_relative == false);
}

TEST_CASE("LoudnessRecommender: multiple findings can fire simultaneously", "[loudness]")
{
    // Too loud AND true peak over
    auto result = makeResult(-5.0f, 6.5f, 0.5f);
    auto findings = rec.recommend(result, makeGenre(), CreativeTarget{});
    CHECK(findings.size() == 2);
}

TEST_CASE("LoudnessRecommender: the suggested boost leaves a margin under the -1 dBTP ceiling", "[loudness][truepeak]")
{
    // The app's --screenshot-audio loop (VER-3) found the loudness fix
    // rejected by the post-render guardrail: the boost put true peak exactly
    // on -1.0 dBTP by this (native) meter, and the service's oversampled
    // meter read the render at -0.94 dBTP. Two true-peak filters (MEAS-5)
    // differ by a few hundredths of a dB, so the boost must not aim at the
    // ceiling itself.
    auto result = makeResult(-27.9f, 6.5f, -16.6f);
    auto findings = rec.recommend(result, makeGenre(), CreativeTarget{});
    REQUIRE(findings.size() == 1);
    const float boost = findings[0].suggested_parameters.at("increase_gain_db");
    CHECK(boost > 0.0f);
    CHECK(-16.6f + boost <= -1.0f - LoudnessRecommender::kTruePeakMeasurementMarginDb + 1e-4f);
    CHECK(LoudnessRecommender::kTruePeakMeasurementMarginDb >= 0.2f);
}

// --- Playback intent -> loudness target (wiring M7) -------------------------

TEST_CASE("playbackIntentTarget maps the documented intents", "[loudness][intent]")
{
    auto s = playbackIntentTarget("streaming");
    REQUIRE(s.has_value());
    CHECK(s->lufs == Catch::Approx(-14.0f));
    CHECK(s->true_peak_dbtp == Catch::Approx(-1.0f));

    auto c = playbackIntentTarget("club");
    REQUIRE(c.has_value());
    CHECK(c->lufs == Catch::Approx(-9.0f));
    CHECK(c->true_peak_dbtp == Catch::Approx(-0.3f));

    auto b = playbackIntentTarget("broadcast");
    REQUIRE(b.has_value());
    CHECK(b->lufs == Catch::Approx(-23.0f)); // EBU R128
    CHECK(b->true_peak_dbtp == Catch::Approx(-1.0f));

    auto h = playbackIntentTarget("headphone");
    REQUIRE(h.has_value());
    CHECK(h->lufs == Catch::Approx(-16.0f));
    CHECK(h->true_peak_dbtp == Catch::Approx(-1.0f));

    CHECK_FALSE(playbackIntentTarget("reference").has_value());
    CHECK_FALSE(playbackIntentTarget("").has_value());
    CHECK_FALSE(playbackIntentTarget("Streaming").has_value()); // ids are data, not display text
}

TEST_CASE("LoudnessRecommender: playback intent overrides the genre loudness window", "[loudness][intent]")
{
    // -9 LUFS sits inside the trap genre window (-12..-7) ...
    auto result = makeResult(-9.0f, 6.5f, -1.5f);
    CHECK(rec.recommend(result, makeGenre(), CreativeTarget{}).empty());

    // ... and "streaming", Intake's default, keeps the genre window: the
    // intent is opt-in, so a loud genre is not judged against -14 LUFS.
    CreativeTarget streaming;
    streaming.playback_intent = "streaming";
    CHECK(rec.recommend(result, makeGenre(), streaming).empty());
    CHECK_FALSE(loudnessOverrideForIntent("streaming").has_value());
    CHECK_FALSE(loudnessOverrideForIntent("reference").has_value());

    // A club master at -9 LUFS is on target.
    CreativeTarget club;
    club.playback_intent = "club";
    CHECK(rec.recommend(result, makeGenre(), club).empty());

    // Broadcast wants -23: far too loud.
    CreativeTarget broadcast;
    broadcast.playback_intent = "broadcast";
    auto bf = rec.recommend(result, makeGenre(), broadcast);
    REQUIRE(bf.size() == 1);
    CHECK_THAT(bf[0].suggested_parameters.at("reduce_gain_db"), WithinAbs(13.0, 0.01));
}

TEST_CASE("LoudnessRecommender: club intent relaxes the true-peak ceiling to -0.3 dBTP", "[loudness][intent]")
{
    auto result = makeResult(-9.0f, 6.5f, -0.5f); // over the genre's -1.0 ceiling
    auto genreFindings = rec.recommend(result, makeGenre(), CreativeTarget{});
    REQUIRE(genreFindings.size() == 1);
    CHECK(genreFindings[0].id.rfind("true_peak_over_", 0) == 0);

    CreativeTarget club;
    club.playback_intent = "club";
    CHECK(rec.recommend(result, makeGenre(), club).empty());
}
