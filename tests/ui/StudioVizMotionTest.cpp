// tests/ui/StudioVizMotionTest.cpp
// STU-1  the live spectrogram's frame FIFO (a fixed ring, same frames as the
//        old erase-the-front vector);
// STU-11 ValueAnimator's critically damped spring (converges, no overshoot
//        from rest, keeps velocity when retargeted, reduced motion snaps);
// STU-12 the genre constellation's pure parts (all catalog genres in eight
//        families, nearest-genre pick, the original -> versions trajectory,
//        framing) and the waterfall's contour lines.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "ui/shared/ContourLines.h"
#include "ui/shared/FrameFifo.h"
#include "ui/shared/GenreConstellation.h"
#include "ui/shared/ReducedMotion.h"
#include "ui/shared/ValueAnimator.h"
#include <khensu/targets/GenreDatabase.h>
#include <cmath>
#include <set>
#include <vector>

using namespace khensu;
using Catch::Matchers::WithinAbs;

TEST_CASE("STU-1: the frame FIFO yields the same frames as erasing the front", "[stu1][viz]")
{
    constexpr int kFrame = 16, kHop = 6;
    FrameFifo fifo(kFrame, kHop);
    std::vector<float> reference; // the old implementation
    std::vector<std::vector<float>> want, got;
    std::vector<float> frame(kFrame);
    int sample = 0;
    // Pushed in uneven blocks, like the tap delivers them.
    for (int block : {5, 1, 17, 3, 40, 2, 9, 33}) {
        for (int i = 0; i < block; ++i, ++sample) {
            const auto s = static_cast<float>(sample);
            reference.push_back(s);
            if (fifo.push(s)) {
                fifo.readFrame(frame.data());
                got.push_back(frame);
                fifo.advance();
            }
        }
        while (static_cast<int>(reference.size()) >= kFrame) {
            want.emplace_back(reference.begin(), reference.begin() + kFrame);
            reference.erase(reference.begin(), reference.begin() + kHop);
        }
    }
    REQUIRE(want.size() > 5);
    CHECK(got == want);
    CHECK(fifo.size() == static_cast<int>(reference.size()));
}

TEST_CASE("STU-1: the frame FIFO holds at a full frame until advanced", "[stu1][viz]")
{
    FrameFifo fifo(4, 4);
    CHECK_FALSE(fifo.push(1.0f));
    CHECK_FALSE(fifo.push(2.0f));
    CHECK_FALSE(fifo.push(3.0f));
    CHECK(fifo.push(4.0f));
    CHECK(fifo.push(5.0f)); // still full: the extra sample is not taken
    std::vector<float> f(4);
    fifo.readFrame(f.data());
    CHECK(f == std::vector<float>{1.0f, 2.0f, 3.0f, 4.0f});
    fifo.advance();
    CHECK(fifo.size() == 0);
}

TEST_CASE("STU-11: a critically damped spring converges without overshoot", "[stu11][motion]")
{
    for (const auto& [from, to] : {std::pair{0.0f, 100.0f}, std::pair{80.0f, -20.0f}, std::pair{3.5f, 1.8f}}) {
        const motion::Spring sp{from, to, 0.0f, 0.0, motion::springOmegaFor(450)};
        CHECK(sp.valueAt(0.0) == from);
        float prev = from;
        bool settled = false;
        for (double t = 0.0; t <= 2000.0; t += 1000.0 / 120.0) {
            const float v = sp.valueAt(t);
            // Monotone towards the target, never past it.
            if (to > from) {
                CHECK(v >= prev - 1e-4f);
                CHECK(v <= to + 1e-4f);
            } else {
                CHECK(v <= prev + 1e-4f);
                CHECK(v >= to - 1e-4f);
            }
            prev = v;
            settled = settled || sp.settledAt(t);
        }
        CHECK(settled);
        // Within 0.1 % of the travel by the chosen settle time.
        CHECK(std::abs(sp.valueAt(450.0) - to) <= 0.0011f * std::abs(to - from));
        CHECK_THAT(sp.valueAt(2000.0), WithinAbs(to, 1e-3));
    }
}

TEST_CASE("STU-11: a spring keeps its velocity when retargeted", "[stu11][motion]")
{
    const float w = motion::springOmegaFor(300);
    const motion::Spring first{0.0f, 100.0f, 0.0f, 0.0, w};
    const double t = 60.0;
    const float v = first.velocityAt(t);
    CHECK(v > 0.0f);
    // Velocity matches a finite difference of the position.
    const float fd = (first.valueAt(t + 0.5) - first.valueAt(t - 0.5)) / 0.001f;
    CHECK_THAT(v, WithinAbs(fd, std::abs(fd) * 0.01 + 1e-3));
    const motion::Spring second{first.valueAt(t), 50.0f, v, t, w};
    CHECK_THAT(second.valueAt(t), WithinAbs(first.valueAt(t), 1e-4));
    CHECK_THAT(second.velocityAt(t), WithinAbs(v, 1e-3));
    CHECK_THAT(second.valueAt(t + 3000.0), WithinAbs(50.0f, 1e-3));
}

TEST_CASE("STU-11: a spring ValueAnimator animates, and snaps under reduced motion", "[stu11][motion][vis7]")
{
    ui::motion::setOverrideForTesting(0);
    ValueAnimator a{[] {}, 300, ValueAnimator::Curve::Spring};
    CHECK(a.curve() == ValueAnimator::Curve::Spring);
    a.setTarget(1.0f);
    CHECK(a.value() == 1.0f);
    a.setTarget(9.0f);
    CHECK(a.isAnimating());
    CHECK(a.target() == 9.0f);

    ui::motion::setOverrideForTesting(1);
    ValueAnimator b{[] {}, 300, ValueAnimator::Curve::Spring};
    b.setTarget(1.0f);
    b.setTarget(9.0f);
    CHECK_FALSE(b.isAnimating());
    CHECK(b.value() == 9.0f);
    ui::motion::setOverrideForTesting(-1);
}

TEST_CASE("STU-12: every catalog genre falls into one of eight families", "[stu12][viz]")
{
    GenreDatabase db;
    const auto json = juce::File(juce::String(__FILE__))
                          .getParentDirectory()
                          .getParentDirectory()
                          .getParentDirectory()
                          .getChildFile("resources/genre_norms/genre_targets.json");
    REQUIRE(json.existsAsFile());
    REQUIRE(db.loadFromFile(json.getFullPathName().toStdString()).is_ok());
    const auto& all = db.listAll();
    CHECK(all.size() >= 185);
    std::set<int> used;
    int other = 0;
    for (const auto& g : all) {
        const int f = constellation::genreFamilyIndex(g.parent);
        REQUIRE(f >= 0);
        REQUIRE(f < constellation::kFamilyCount);
        used.insert(f);
        if (f == constellation::kFamilyCount - 1) ++other;
    }
    CHECK(used.size() == static_cast<size_t>(constellation::kFamilyCount));
    CHECK(other <= 3); // "Other" stays a remainder, not a bucket
    CHECK(constellation::genreFamilyIndex("") == constellation::kFamilyCount - 1);
}

TEST_CASE("STU-12: hover picks the nearest genre within reach", "[stu12][viz]")
{
    const std::vector<juce::Point<float>> pts{{10, 10}, {50, 50}, {52, 49}, {200, 10}};
    CHECK(constellation::nearestPoint(pts, {51.5f, 49.2f}, 24.0f) == 2);
    CHECK(constellation::nearestPoint(pts, {12, 12}, 24.0f) == 0);
    CHECK(constellation::nearestPoint(pts, {120, 120}, 24.0f) == -1);
    CHECK(constellation::nearestPoint({}, {0, 0}, 24.0f) == -1);
}

TEST_CASE("STU-12: the trajectory runs from the original along the render tree", "[stu12][viz]")
{
    struct Node {
        juce::String path;
        int parent = -1;
    };
    // Original -> a -> b, and a branch a -> c that is current.
    const std::vector<Node> tree{{"a.wav", -1}, {"b.wav", 0}, {"c.wav", 0}};
    const std::vector<juce::String> applied{"a.wav", "b.wav"};
    CHECK(constellation::trajectoryPaths(tree, 2, applied, "orig.wav") ==
          std::vector<juce::String>{"orig.wav", "a.wav", "c.wav"});
    CHECK(constellation::trajectoryPaths(tree, 1, applied, "orig.wav") ==
          std::vector<juce::String>{"orig.wav", "a.wav", "b.wav"});
    // No current node: the linear history.
    CHECK(constellation::trajectoryPaths(tree, -1, applied, "orig.wav") ==
          std::vector<juce::String>{"orig.wav", "a.wav", "b.wav"});
    // No renders: the session point alone.
    CHECK(constellation::trajectoryPaths(std::vector<Node>{}, -1, {}, "orig.wav") ==
          std::vector<juce::String>{"orig.wav"});
    // A malformed cycle terminates.
    const std::vector<Node> loop{{"x.wav", 1}, {"y.wav", 0}};
    CHECK(constellation::trajectoryPaths(loop, 0, {}, "").size() <= 2);
}

TEST_CASE("STU-12: framing aims at the centre and backs off to fit", "[stu12][viz]")
{
    const auto one = constellation::framing({{0.5f, 0.2f, -0.1f}});
    CHECK_THAT(one.x, WithinAbs(0.5, 1e-6));
    CHECK_THAT(one.zoom, WithinAbs(1.5, 1e-6)); // a point: as close as the orbit goes
    const auto spread = constellation::framing({{-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}});
    CHECK_THAT(spread.x, WithinAbs(0.0, 1e-6));
    CHECK(spread.zoom > one.zoom);
    CHECK(spread.zoom <= 8.0f);
    const auto none = constellation::framing({});
    CHECK_THAT(none.zoom, WithinAbs(3.5, 1e-6));
}

TEST_CASE("STU-12: contour segments trace a level across the grid", "[stu12][viz]")
{
    // A ramp along columns: the 0.5 contour is the vertical line col = 1.5.
    const std::vector<std::vector<float>> ramp{
        {0.0f, 0.3f, 0.7f, 1.0f}, {0.0f, 0.3f, 0.7f, 1.0f}, {0.0f, 0.3f, 0.7f, 1.0f}};
    const auto segs = contour::segments(ramp, 0.5f);
    REQUIRE(segs.size() == 2); // one per row of cells
    for (const auto& s : segs) {
        CHECK_THAT(s.a.col, WithinAbs(1.5, 1e-5));
        CHECK_THAT(s.b.col, WithinAbs(1.5, 1e-5));
    }
    CHECK(contour::segments(ramp, 2.0f).empty()); // nothing reaches it
    CHECK(contour::segments({{1.0f}}, 0.5f).empty());
    // A single peak is closed by four segments.
    const std::vector<std::vector<float>> peak{{0, 0, 0}, {0, 1, 0}, {0, 0, 0}};
    CHECK(contour::segments(peak, 0.5f).size() == 4);
}
