// tests/shared-core/test_meter_ballistics.cpp
// UI-5: the studio meters' display ballistics and the lock-free stereo ring
// the Vectorscope drains (khensu/ui/MeterBallistics.h, StereoSampleRing.h).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <khensu/ui/MeterBallistics.h>
#include <khensu/ui/StereoSampleRing.h>

#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

using namespace khensu::ui;
using Catch::Approx;

TEST_CASE("Peak-programme ballistics rise at once and fall 20 dB in 1.7 s", "[ui5][meters]")
{
    meter::PeakFall pf;
    CHECK(pf.update(-6.0f, 0.016) == Approx(-6.0f));
    // Falls linearly, never below the input.
    float v = -6.0f;
    for (int i = 0; i < 100; ++i) v = pf.update(-70.0f, 0.017); // 1.7 s
    CHECK(v == Approx(-26.0f).margin(0.05f));
    // Frame-rate independent: one 1.7 s step lands in the same place.
    meter::PeakFall once;
    once.update(-6.0f, 0.0);
    CHECK(once.update(-70.0f, 1.7) == Approx(-26.0f).margin(0.01f));
    // Never below the floor; NaN reads as the floor.
    CHECK(once.update(std::nanf(""), 100.0) == Approx(once.floorDb));
}

TEST_CASE("Peak hold holds for its time, then falls", "[ui5][meters]")
{
    meter::PeakHold hold;
    hold.holdSeconds = 2.0f;
    hold.fallDbPerSec = 10.0f;
    hold.update(-3.0f, 0.0);
    CHECK(hold.update(-40.0f, 1.0) == Approx(-3.0f)); // within the hold
    CHECK(hold.update(-40.0f, 0.9) == Approx(-3.0f));
    CHECK(hold.update(-40.0f, 0.5) == Approx(-8.0f)); // past 2 s: 0.5 s x 10 dB/s
    CHECK(hold.update(-1.0f, 0.1) == Approx(-1.0f));  // a new peak resets the hold
    hold.reset();
    CHECK(hold.value == Approx(hold.floorDb));
}

TEST_CASE("Max hold keeps the highest value until reset", "[ui5][meters]")
{
    meter::MaxHold max;
    CHECK_FALSE(max.hasValue());
    max.update(-12.0f);
    max.update(-0.3f);
    max.update(-20.0f);
    max.update(std::nanf(""));
    CHECK(max.value == Approx(-0.3f));
    max.reset();
    CHECK_FALSE(max.hasValue());
}

TEST_CASE("The smoother reaches 63% of a step in one time constant at any frame rate", "[ui5][meters]")
{
    for (double fps : {30.0, 60.0, 144.0}) {
        meter::Smoother s;
        s.riseSeconds = 0.1f;
        s.fallSeconds = 0.4f;
        s.snap(0.0f);
        const int frames = static_cast<int>(std::lround(0.1 * fps));
        for (int i = 0; i < frames; ++i) s.update(1.0f, 1.0 / fps);
        INFO("fps " << fps);
        CHECK(s.value == Approx(1.0 - std::exp(-1.0)).margin(0.02));
    }
    // Rise and fall use their own constants.
    meter::Smoother s;
    s.riseSeconds = 0.0f;
    s.fallSeconds = 1.0f;
    s.snap(0.0f);
    CHECK(s.update(1.0f, 0.01) == Approx(1.0f)); // instant rise
    CHECK(s.update(0.0f, 1.0) == Approx(std::exp(-1.0)).margin(1e-4));
    // An unprimed smoother snaps to its first value.
    meter::Smoother fresh;
    CHECK(fresh.update(-23.0f, 0.016) == Approx(-23.0f));
}

TEST_CASE("Meter proportion clamps to the scale", "[ui5][meters]")
{
    CHECK(meter::proportion(-24.0f, -48.0f, 0.0f) == Approx(0.5f));
    CHECK(meter::proportion(6.0f, -48.0f, 0.0f) == Approx(1.0f));
    CHECK(meter::proportion(-100.0f, -48.0f, 0.0f) == Approx(0.0f));
    CHECK(meter::proportion(std::nanf(""), -48.0f, 0.0f) == Approx(0.0f));
}

TEST_CASE("The stereo ring keeps order, drops when full and never exceeds its capacity", "[ui5][meters][ring]")
{
    StereoSampleRing ring(100); // rounds up to 128
    REQUIRE(ring.capacity() == 128);
    std::vector<float> l(200), r(200);
    for (int i = 0; i < 200; ++i) {
        l[static_cast<size_t>(i)] = static_cast<float>(i);
        r[static_cast<size_t>(i)] = static_cast<float>(-i);
    }
    CHECK(ring.push(l.data(), r.data(), 200) == 128); // the rest is dropped
    CHECK(ring.dropped() == 72u);
    CHECK(ring.available() == 128);

    std::vector<float> outL(64), outR(64);
    REQUIRE(ring.pop(outL.data(), outR.data(), 64) == 64);
    CHECK(outL[0] == 0.0f);
    CHECK(outL[63] == 63.0f);
    CHECK(outR[63] == -63.0f);
    // Wraps around the end of the storage.
    CHECK(ring.push(l.data() + 128, r.data() + 128, 64) == 64);
    std::vector<float> all(200), allR(200);
    REQUIRE(ring.pop(all.data(), allR.data(), 200) == 128);
    for (int i = 0; i < 128; ++i) CHECK(all[static_cast<size_t>(i)] == static_cast<float>(64 + i));
    CHECK(ring.available() == 0);

    // Mono: a null right channel mirrors the left.
    ring.push(l.data(), nullptr, 4);
    REQUIRE(ring.pop(outL.data(), outR.data(), 4) == 4);
    CHECK(outR[3] == 3.0f);
    ring.push(l.data(), r.data(), 10);
    ring.clear();
    CHECK(ring.available() == 0);
}

TEST_CASE("The stereo ring passes every sample in order between two threads", "[ui5][meters][ring]")
{
    StereoSampleRing ring(256);
    constexpr int kTotal = 200000;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        float block[32], blockR[32];
        int next = 0;
        while (next < kTotal) {
            const int n = std::min(32, kTotal - next);
            for (int i = 0; i < n; ++i) {
                block[i] = static_cast<float>(next + i);
                blockR[i] = -block[i];
            }
            int stored = 0;
            while (stored < n) stored += ring.push(block + stored, blockR + stored, n - stored); // retry, no drops
            next += n;
        }
        done = true;
    });
    int expected = 0;
    bool ordered = true;
    float l[64], r[64];
    while (expected < kTotal) {
        const int n = ring.pop(l, r, 64);
        for (int i = 0; i < n; ++i) {
            ordered = ordered && l[i] == static_cast<float>(expected) && r[i] == -static_cast<float>(expected);
            ++expected;
        }
        if (n == 0 && done && ring.available() == 0) break;
    }
    producer.join();
    CHECK(ordered);
    CHECK(expected == kTotal);
}
