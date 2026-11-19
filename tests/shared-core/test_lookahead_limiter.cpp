// tests/shared-core/test_lookahead_limiter.cpp
// Audit CQ-03: the plugin's brickwall limiter must actually hold its ceiling
// (the previous exponential-attack version overshot) and must not allocate on
// the audio thread. These prove the ceiling invariant across random
// transients / sample rates / block sizes, plus latency and bypass behavior.
#include <catch2/catch_test_macros.hpp>
#include <khensu/dsp/LookaheadBrickwallLimiter.h>
#include <cmath>
#include <random>
#include <vector>

using khensu::dsp::LookaheadBrickwallLimiter;

namespace {

// Runs `total` samples of a signal generator through the limiter in blocks and
// returns the maximum absolute OUTPUT sample seen. `gen(i)` produces input[i].
template <typename Gen>
float peakOutput(LookaheadBrickwallLimiter& lim, int channels, int total, int blockSize, float ceiling, bool enabled,
                 Gen gen)
{
    std::vector<std::vector<float>> data(static_cast<size_t>(channels), std::vector<float>(static_cast<size_t>(total)));
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < total; ++i)
            data[static_cast<size_t>(c)][static_cast<size_t>(i)] = gen(i, c);

    float maxOut = 0.0f;
    for (int start = 0; start < total; start += blockSize) {
        const int n = std::min(blockSize, total - start);
        std::vector<float*> ptrs(static_cast<size_t>(channels));
        for (int c = 0; c < channels; ++c)
            ptrs[static_cast<size_t>(c)] = &data[static_cast<size_t>(c)][static_cast<size_t>(start)];
        lim.process(ptrs.data(), channels, n, ceiling, enabled);
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < n; ++i)
                maxOut = std::max(maxOut, std::abs(ptrs[static_cast<size_t>(c)][i]));
    }
    return maxOut;
}

} // namespace

TEST_CASE("LookaheadBrickwallLimiter never exceeds the ceiling on a hard transient", "[limiter][cq03]")
{
    LookaheadBrickwallLimiter lim;
    lim.prepare(44100.0, 2, 66); // ~1.5 ms lookahead at 44.1k

    const float ceiling = 0.5f;
    // A single huge spike surrounded by silence - the classic overshoot case.
    const float maxOut = peakOutput(lim, 2, 4096, 128, ceiling, /*enabled=*/true,
                                    [](int i, int) { return i == 2000 ? 4.0f : 0.0f; });

    // A tiny epsilon over the ceiling is allowed for float rounding only.
    CHECK(maxOut <= ceiling + 1e-4f);
}

TEST_CASE("LookaheadBrickwallLimiter holds the ceiling across randomized transients, rates, and block sizes",
          "[limiter][cq03]")
{
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> amp(-6.0f, 6.0f);

    for (double sr : {44100.0, 48000.0, 96000.0}) {
        for (int block : {1, 16, 64, 512}) {
            LookaheadBrickwallLimiter lim;
            const int look = std::max(1, static_cast<int>(std::round(sr * 0.0015)));
            lim.prepare(sr, 2, look);

            const float ceiling = 0.7f;
            const float maxOut = peakOutput(lim, 2, 8000, block, ceiling, /*enabled=*/true,
                                            [&](int, int) { return amp(rng); });
            INFO("sr=" << sr << " block=" << block);
            CHECK(maxOut <= ceiling + 1e-4f);
        }
    }
}

TEST_CASE("LookaheadBrickwallLimiter passes signal already under the ceiling unchanged (after latency)",
          "[limiter][cq03]")
{
    LookaheadBrickwallLimiter lim;
    const int look = 32;
    lim.prepare(44100.0, 1, look);

    // A constant 0.2 well under a 0.9 ceiling: once the delay line has filled,
    // the output must equal the input (unity gain, no reduction).
    std::vector<float> buf(2048, 0.2f);
    float* p = buf.data();
    lim.process(&p, 1, 2048, 0.9f, /*enabled=*/true);

    for (int i = look + 4; i < 2048; ++i)
        CHECK(std::abs(buf[static_cast<size_t>(i)] - 0.2f) < 1e-5f);
}

TEST_CASE("LookaheadBrickwallLimiter is a pure delay when disabled (constant latency, no gain change)",
          "[limiter][cq03][cq02]")
{
    LookaheadBrickwallLimiter lim;
    const int look = 10;
    lim.prepare(44100.0, 1, look);

    std::vector<float> buf(200);
    for (int i = 0; i < 200; ++i)
        buf[static_cast<size_t>(i)] = static_cast<float>(i + 1); // ramp incl. values > any ceiling

    float* p = buf.data();
    lim.process(&p, 1, 200, /*ceiling=*/0.1f, /*enabled=*/false);

    // Disabled -> output is the input delayed by `look`, at unity gain (the
    // 0.1 ceiling is ignored). Bypass must not apply gain reduction (CQ-02).
    for (int i = look; i < 200; ++i)
        CHECK(buf[static_cast<size_t>(i)] == static_cast<float>(i + 1 - look));
}

TEST_CASE("LookaheadBrickwallLimiter reports its lookahead as latency", "[limiter][cq03]")
{
    LookaheadBrickwallLimiter lim;
    lim.prepare(48000.0, 2, 72);
    CHECK(lim.latencySamples() == 72);
}

TEST_CASE("LookaheadBrickwallLimiter releases back toward unity after a transient passes", "[limiter][cq03]")
{
    LookaheadBrickwallLimiter lim;
    lim.prepare(44100.0, 1, 32);

    // Spike early, then a long quiet-but-nonzero tail under the ceiling.
    std::vector<float> buf(20000, 0.1f);
    buf[100] = 5.0f;
    float* p = buf.data();
    lim.process(&p, 1, 20000, 0.5f, true);

    // Long after the spike, gain should have released back to ~unity, so the
    // 0.1 tail passes at full level again.
    CHECK(std::abs(buf[19000] - 0.1f) < 1e-3f);
}

TEST_CASE("LookaheadBrickwallLimiter ramps gain across the lookahead instead of stepping (AUDIT-030)",
          "[limiter][audit030]")
{
    const double sr = 48000.0;
    const int look = 72; // 1.5 ms at 48k
    LookaheadBrickwallLimiter lim;
    lim.prepare(sr, 1, look);

    const float ceiling = std::pow(10.0f, -1.0f / 20.0f); // -1 dBFS
    const float quiet = std::pow(10.0f, -20.0f / 20.0f);  // -20 dBFS
    const int total = 24000;
    const int burstStart = 6000;
    std::vector<float> buf(static_cast<size_t>(total));
    for (int i = 0; i < total; ++i) {
        const float amp = i < burstStart ? quiet : 1.0f; // -20 -> 0 dBFS step
        buf[static_cast<size_t>(i)] = amp * std::sin(2.0f * 3.14159265f * 997.0f * static_cast<float>(i) / 48000.0f);
    }

    float maxOut = 0.0f;
    float maxStep = 0.0f;
    float prevGain = 1.0f;
    for (int start = 0; start < total; start += 64) {
        float* p = buf.data() + start;
        // One sample at a time so every gain value can be inspected.
        for (int i = 0; i < 64; ++i) {
            float* q = p + i;
            lim.process(&q, 1, 1, ceiling, true);
            maxOut = std::max(maxOut, std::abs(*q));
            maxStep = std::max(maxStep, std::abs(lim.currentGain() - prevGain));
            prevGain = lim.currentGain();
        }
    }

    CHECK(maxOut <= ceiling + 1e-5f);
    // The full reduction is (1 - ceiling) ~= 0.109; the old instant attack
    // took most of it in one sample. Spread over L+1 samples it is ~0.0015.
    const float fullReduction = 1.0f - ceiling;
    INFO("maxStep=" << maxStep);
    CHECK(maxStep <= fullReduction / static_cast<float>(look + 1) * 1.05f + 1e-6f);
    // ...and the limiter actually reduced the burst (it is not just passing it).
    CHECK(prevGain < 0.95f);
}

TEST_CASE("LookaheadBrickwallLimiter silences channels it has no delay storage for (AUDIT-088)", "[limiter][audit088]")
{
    LookaheadBrickwallLimiter lim;
    lim.prepare(44100.0, 1, 8);
    std::vector<float> a(64, 0.25f), b(64, 0.25f);
    float* ptrs[2] = {a.data(), b.data()};
    lim.process(ptrs, 2, 64, 0.9f, true);
    for (float v : b)
        CHECK(v == 0.0f);
    CHECK(a[63] == 0.25f); // the prepared channel is delayed, not dropped
}

TEST_CASE("LookaheadBrickwallLimiter holds the ceiling only with a time-aligned external detector (CORE-02)",
          "[limiter][core02]")
{
    // A detector that reports each peak D samples late leaves the start of a
    // step at unity gain; the same detector delivered aligned (audio delayed
    // by D first, as CopilotOutputStage does) holds the ceiling exactly.
    const int look = 66;
    const int lag = 36;
    const float ceiling = 0.5f;
    const int total = 4000;
    const int stepAt = 2000;
    std::vector<float> input(static_cast<size_t>(total));
    for (int i = 0; i < total; ++i)
        input[static_cast<size_t>(i)] = i >= stepAt ? 2.0f : 0.1f;

    auto run = [&](bool aligned) {
        LookaheadBrickwallLimiter lim;
        lim.prepare(44100.0, 1, look);
        std::vector<float> audio(static_cast<size_t>(total)), detector(static_cast<size_t>(total));
        for (int i = 0; i < total; ++i) {
            const int src = i - lag;
            const float lagged = src >= 0 ? std::abs(input[static_cast<size_t>(src)]) : 0.0f;
            detector[static_cast<size_t>(i)] = lagged;
            // Aligned: delay the audio by the detector lag too.
            audio[static_cast<size_t>(i)] = aligned ? (src >= 0 ? input[static_cast<size_t>(src)] : 0.0f)
                                                    : input[static_cast<size_t>(i)];
        }
        float* p = audio.data();
        lim.process(&p, 1, total, ceiling, true, detector.data());
        float peak = 0.0f;
        for (float v : audio)
            peak = std::max(peak, std::abs(v));
        return peak;
    };

    CHECK(run(false) > ceiling * 1.5f); // the defect: a late detector overshoots
    CHECK(run(true) <= ceiling + 1e-4f);
}
