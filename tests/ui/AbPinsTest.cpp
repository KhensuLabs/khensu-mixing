// tests/ui/AbPinsTest.cpp
// STU-8: "Pin as A" / "Pin as B" in the render tree - which versions the
// loudness-matched A/B compares, how a pinned A is level-matched, and that a
// session change forgets the pins.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "ui/MixMasterEngine/AbPins.h"
#include "session/PlaybackSourceController.h"
#include <cmath>

using namespace khensu;
using Catch::Matchers::WithinAbs;

namespace {
std::vector<RenderHistory::TreeNode> threeVersions()
{
    // R1 <- R2, and R3 a second root (a render of the original).
    return {{"C:/mix_fix1.wav", -1}, {"C:/mix_fix2.wav", 0}, {"C:/mix_alt.wav", -1}};
}
float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }
} // namespace

TEST_CASE("A/B pins: default is the original against the current version", "[abpins][stu8]")
{
    const auto tree = threeVersions();
    const auto s = ab_pins::resolve(tree, {}, "C:/mix_fix2.wav", "C:/mix_fix2.wav");
    CHECK(s.aIsOriginal);
    CHECK(s.aNode == -1);
    CHECK(s.bNode == 1);
    CHECK(s.hasB);
    CHECK(s.isDefault);
    CHECK(ab_pins::badgeFor(s, 1) == "B");
    CHECK(ab_pins::badgeFor(s, 0).isEmpty());

    // No render accepted and no B: still the default (nothing to reset).
    const auto none = ab_pins::resolve(tree, {}, {}, {});
    CHECK(none.isDefault);
    CHECK_FALSE(none.hasB);
}

TEST_CASE("A/B pins: any two versions, and Reset goes back", "[abpins][stu8]")
{
    const auto tree = threeVersions();
    // A pinned to R1, B pinned to R3 while R2 stays the current version.
    const auto s = ab_pins::resolve(tree, "C:/mix_fix1.wav", "C:/mix_alt.wav", "C:/mix_fix2.wav");
    CHECK_FALSE(s.aIsOriginal);
    CHECK(s.aNode == 0);
    CHECK(s.bNode == 2);
    CHECK_FALSE(s.isDefault);
    CHECK(ab_pins::badgeFor(s, 0) == "A");
    CHECK(ab_pins::badgeFor(s, 2) == "B");
    CHECK(ab_pins::badgeFor(s, 1).isEmpty());

    // Only B pinned away from the current version is not the default either.
    CHECK_FALSE(ab_pins::resolve(tree, {}, "C:/mix_fix1.wav", "C:/mix_fix2.wav").isDefault);
    // A preview B (not a tree version) is shown as no node.
    const auto preview = ab_pins::resolve(tree, {}, "C:/preview.wav", "C:/mix_fix2.wav");
    CHECK(preview.bNode == -1);
    CHECK_FALSE(preview.isDefault);

    const auto reset = ab_pins::resetTo("C:/mix_fix2.wav");
    CHECK(reset.aPath.isEmpty());
    CHECK(reset.bPath == "C:/mix_fix2.wav");
    CHECK(ab_pins::resolve(tree, reset.aPath, reset.bPath, "C:/mix_fix2.wav").isDefault);

    // The same version on both sides reads A=B.
    const auto same = ab_pins::resolve(tree, "C:/mix_fix1.wav", "C:/mix_fix1.wav", "C:/mix_fix2.wav");
    CHECK(ab_pins::badgeFor(same, 0) == "A=B");
}

TEST_CASE("A/B pins: a pinned A is the level B is matched to", "[abpins][stu8]")
{
    PlaybackSourceController src;
    src.userVolume = 1.0f;
    src.processedPath = "C:/b.wav";
    src.processedLufs = -10.0f;

    PlaybackSourceController::GainContext ctx;
    ctx.originalLufs = -14.0f; // the original, from the analysis
    ctx.originalPath = "C:/orig.wav";

    // Default: B (4 dB louder than the original) is turned down 4 dB.
    ctx.loadedPath = "C:/b.wav";
    CHECK_THAT(src.gainFor(ctx), WithinAbs(dbToGain(-4.0f), 1e-4));

    // Pin a version as A whose level is not yet known: A and B are held muted.
    CHECK(src.assignReference("C:/a.wav", std::nullopt)); // needs measuring
    CHECK(src.awaitingReferenceLufs());
    CHECK(src.gainFor(ctx) == 0.0f);
    ctx.loadedPath = "C:/a.wav";
    CHECK(src.gainFor(ctx) == 0.0f);
    CHECK(src.levelOffset(ctx.originalLufs).measuring);

    // Measured at -12: B is 2 dB louder than A, so B drops 2 dB, A plays at unity.
    src.referenceLufs = -12.0f;
    CHECK_THAT(src.gainFor(ctx), WithinAbs(1.0f, 1e-4));
    ctx.loadedPath = "C:/b.wav";
    CHECK_THAT(src.gainFor(ctx), WithinAbs(dbToGain(-2.0f), 1e-4));
    const auto offset = src.levelOffset(ctx.originalLufs);
    CHECK(offset.matched);
    REQUIRE(offset.bMinusADb.has_value());
    CHECK_THAT(*offset.bMinusADb, WithinAbs(2.0f, 1e-4));
    CHECK(src.aSidePath("C:/orig.wav") == "C:/a.wav");

    // A louder pinned A is the one turned down.
    src.referenceLufs = -8.0f;
    ctx.loadedPath = "C:/a.wav";
    CHECK_THAT(src.gainFor(ctx), WithinAbs(dbToGain(-2.0f), 1e-4));

    // A cached level needs no measuring.
    CHECK_FALSE(src.assignReference("C:/a2.wav", -11.0f));
    CHECK(src.referenceLufs == -11.0f);

    // Unpinning: back to the original as A.
    CHECK_FALSE(src.assignReference({}, -11.0f));
    CHECK(src.referencePath.isEmpty());
    CHECK_FALSE(src.referenceLufs.has_value());
    CHECK(src.aSidePath("C:/orig.wav") == "C:/orig.wav");
    ctx.loadedPath = "C:/b.wav";
    CHECK_THAT(src.gainFor(ctx), WithinAbs(dbToGain(-4.0f), 1e-4));
}

TEST_CASE("A/B pins: an unmeasurable pinned A plays unmatched, not muted", "[abpins][stu8]")
{
    PlaybackSourceController src;
    src.userVolume = 1.0f;
    src.processedPath = "C:/b.wav";
    src.processedLufs = -10.0f;
    src.assignReference("C:/a.wav", std::nullopt);
    src.referenceLufsUnavailable = true;
    PlaybackSourceController::GainContext ctx;
    ctx.originalLufs = -14.0f;
    ctx.originalPath = "C:/orig.wav";
    ctx.loadedPath = "C:/a.wav";
    CHECK_THAT(src.gainFor(ctx), WithinAbs(1.0f, 1e-4));
    ctx.loadedPath = "C:/b.wav";
    CHECK_THAT(src.gainFor(ctx), WithinAbs(1.0f, 1e-4));
}

TEST_CASE("A/B pins: a session change forgets them", "[abpins][stu8]")
{
    PlaybackSourceController src;
    src.processedPath = "C:/b.wav";
    src.assignReference("C:/a.wav", -12.0f);
    src.resetForSession();
    CHECK(src.referencePath.isEmpty());
    CHECK_FALSE(src.referenceLufs.has_value());
    CHECK_FALSE(src.referenceLufsUnavailable);
    CHECK(src.processedPath.isEmpty());
}
