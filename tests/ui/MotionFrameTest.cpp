// tests/ui/MotionFrameTest.cpp
// UX2-7: the app's motion checked frame by frame instead of by eye.
//  - The 200 ms value transition (ValueAnimator's Tween) sampled at 60 and
//    120 Hz: it starts where it was, never overshoots or reverses, lands
//    exactly on the target at 200 ms, and no single frame jumps more than a
//    third of the way.
//  - The A/B crossfade (PlaybackEngine's equal-power law over kCrossfadeMs):
//    power stays within 0.1 dB of constant for uncorrelated material, the
//    gains never step by more than one sample's share, and a crossfade
//    between two out-of-phase sines produces no sample-to-sample jump larger
//    than the sines' own slope - i.e. no click.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "session/PlaybackEngine.h"
#include "ui/shared/ValueAnimator.h"
#include <cmath>
#include <vector>

using namespace khensu;
using Catch::Matchers::WithinAbs;

namespace {
constexpr double kPi = 3.14159265358979323846;

std::vector<float> framesAt(double fps, const motion::Tween& tw)
{
    std::vector<float> v;
    const double step = 1000.0 / fps;
    for (double t = tw.startMs; t <= tw.startMs + tw.durationMs + step; t += step)
        v.push_back(tw.valueAt(t));
    return v;
}
} // namespace

TEST_CASE("UX2-7: a value transition lands in 200 ms without overshoot at 60 and 120 Hz", "[ux2][motion]")
{
    for (const auto& tw : {motion::Tween{45.0f, 86.0f, 1000.0, motion::kDurationMs},
                           motion::Tween{86.0f, 12.0f, 0.0, motion::kDurationMs}}) {
        CHECK(motion::kDurationMs >= 150);
        CHECK(motion::kDurationMs <= 250);
        CHECK(tw.valueAt(tw.startMs) == tw.from);
        CHECK(tw.valueAt(tw.startMs + tw.durationMs) == tw.to);
        CHECK(tw.finishedAt(tw.startMs + tw.durationMs));
        CHECK_FALSE(tw.finishedAt(tw.startMs + tw.durationMs - 1.0));
        const float span = std::abs(tw.to - tw.from);
        const float dir = tw.to > tw.from ? 1.0f : -1.0f;
        for (const double fps : {60.0, 120.0}) {
            const auto f = framesAt(fps, tw);
            INFO(fps << " fps, " << f.size() << " frames");
            REQUIRE(f.size() >= static_cast<size_t>(fps * 0.2));
            for (size_t i = 1; i < f.size(); ++i) {
                const float step = (f[i] - f[i - 1]) * dir;
                CHECK(step >= -1e-4f);                    // never reverses
                CHECK(step <= span / 3.0f);               // no frame jumps a third of the way
                CHECK((f[i] - tw.to) * dir <= 1e-4f);     // never overshoots
            }
            CHECK_THAT(f.back(), WithinAbs(tw.to, 1e-4));
        }
    }
    // Ease-out: most of the move happens early, the landing is gentle.
    CHECK(motion::easeOutCubic(0.5f) > 0.8f);
    CHECK(motion::easeOutCubic(1.0f) - motion::easeOutCubic(0.9f) < 0.01f);
}

TEST_CASE("UX2-7: the A/B crossfade keeps power constant and does not click", "[ux2][motion][crossfade]")
{
    const double sr = 48000.0;
    const int fade = static_cast<int>(std::lround(sr * PlaybackEngine::kCrossfadeMs / 1000.0));
    REQUIRE(fade > 0);

    float prevA = 1.0f, prevB = 0.0f;
    double worstPowerDb = 0.0;
    for (int i = 0; i <= fade; ++i) {
        float a = 0.0f, b = 0.0f;
        PlaybackEngine::equalPowerGains(static_cast<float>(i) / static_cast<float>(fade), a, b);
        const double power = static_cast<double>(a) * a + static_cast<double>(b) * b;
        worstPowerDb = std::max(worstPowerDb, std::abs(10.0 * std::log10(power)));
        // A smooth ramp: each sample moves the gains by at most a sample's
        // share of a quarter sine (pi/2 / fade), with a little slack.
        CHECK(std::abs(a - prevA) <= static_cast<float>(kPi / 2.0 / fade) * 1.05f);
        CHECK(std::abs(b - prevB) <= static_cast<float>(kPi / 2.0 / fade) * 1.05f);
        CHECK(a <= prevA + 1e-6f);
        CHECK(b >= prevB - 1e-6f);
        prevA = a;
        prevB = b;
    }
    INFO("worst power deviation " << worstPowerDb << " dB");
    CHECK(worstPowerDb < 0.1);

    // Two out-of-phase 1 kHz sines crossfaded: the output's largest
    // sample-to-sample step stays within the sines' own (no click).
    const double w = 2.0 * kPi * 1000.0 / sr;
    const double maxSlope = 0.5 * w; // amplitude 0.5
    double prev = 0.0, worstStep = 0.0;
    for (int i = -100; i <= fade + 100; ++i) {
        const float t = static_cast<float>(juce::jlimit(0, fade, i)) / static_cast<float>(fade);
        float a = 0.0f, b = 0.0f;
        PlaybackEngine::equalPowerGains(t, a, b);
        const double y = a * 0.5 * std::sin(w * i) + b * 0.5 * std::sin(w * i + kPi);
        if (i > -100) worstStep = std::max(worstStep, std::abs(y - prev));
        prev = y;
    }
    INFO("worst step " << worstStep << " vs sine slope " << maxSlope);
    CHECK(worstStep <= maxSlope * 1.1);
}
