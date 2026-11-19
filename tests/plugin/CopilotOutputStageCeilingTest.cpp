// FIX-12: the limiter ceiling holds in every A/B listen mode, not only on the
// processed side. The Original side is the dry signal times the level-match
// gain (up to +12 dB) and Delta is processed minus that; both used to be mixed
// in after the limiter with nothing to stop them. Now the match gain is capped
// by the headroom of the dry side and Delta is turned down as a whole; the
// processed path is not touched.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "../../plugin-copilot/Source/CopilotOutputStage.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using khensu::CopilotOutputStage;

namespace {
constexpr double kRate = 48000.0;
constexpr int kBlock = 480;
constexpr float kCeilingDb = -1.0f;
constexpr int kProcessed = 0, kOriginal = 1, kDelta = 2, kBlind = 3;
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr float kBurstPeak = 0.8f;

enum class Dry { Bursts, Silent };

/// Unprocessed input with a high crest factor: 10 ms of a 0.8-peak 1 kHz tone
/// in every 100 ms, over a quiet bed. 1 kHz is 48 samples a cycle here, so
/// every burst has the same sample peak (exactly 0.8, left channel). The
/// right channel is quieter and out of phase, so a per-channel gain would show.
float burstDry(int channel, long long n)
{
    const double amp = (n % 4800) < 480 ? static_cast<double>(kBurstPeak) : 0.05;
    const double scale = channel == 0 ? 1.0 : 0.7;
    return static_cast<float>(scale * amp * std::sin(kTwoPi * 1000.0 * static_cast<double>(n) / kRate + 0.9 * channel));
}

/// The "processed" signal: dense, so the level match has to turn the bursts up.
float denseWet(int channel, long long n, float amp)
{
    return amp * static_cast<float>(std::sin(kTwoPi * 440.0 * static_cast<double>(n) / kRate + 0.5 * channel));
}

/// Feeds the stage one dry signal and an unrelated processed one, block by
/// block, with the limiter on at kCeilingDb.
class Driver {
  public:
    Driver(int channels, float wetAmp, Dry dry = Dry::Bursts)
        : channels_(channels), wetAmp_(wetAmp), dry_(dry), buf_(channels, kBlock)
    {
        stage_.prepareToPlay(kRate, kBlock, channels);
    }

    /// Runs `blocks` blocks in `abMode`; returns the largest output magnitude
    /// and appends every output sample to `capture` when given.
    float run(int blocks, int abMode, std::vector<float>* capture = nullptr, bool limiterOn = true)
    {
        float peak = 0.0f;
        for (int block = 0; block < blocks; ++block) {
            for (int c = 0; c < channels_; ++c)
                for (int i = 0; i < kBlock; ++i)
                    buf_.setSample(c, i, dry_ == Dry::Bursts ? burstDry(c, pos_ + i) : 0.0f);
            stage_.captureDry(buf_, abMode);
            for (int c = 0; c < channels_; ++c)
                for (int i = 0; i < kBlock; ++i) buf_.setSample(c, i, denseWet(c, pos_ + i, wetAmp_));
            stage_.process(buf_, false, limiterOn, kCeilingDb, abMode, 0);
            pos_ += kBlock;
            for (int c = 0; c < channels_; ++c)
                for (int i = 0; i < kBlock; ++i) {
                    const float v = buf_.getSample(c, i);
                    peak = juce::jmax(peak, std::abs(v));
                    if (capture != nullptr) capture->push_back(v);
                }
        }
        return peak;
    }

    CopilotOutputStage& stage() noexcept { return stage_; }
    const juce::AudioBuffer<float>& lastBlock() const noexcept { return buf_; }
    /// Absolute index of the first sample of the last block.
    long long lastBlockStart() const noexcept { return pos_ - kBlock; }

  private:
    CopilotOutputStage stage_;
    int channels_;
    float wetAmp_;
    Dry dry_;
    juce::AudioBuffer<float> buf_;
    long long pos_ = 0;
};

float ceilingGain()
{
    return juce::Decibels::decibelsToGain(kCeilingDb);
}

std::uint64_t fingerprint(const std::vector<float>& samples)
{
    std::uint64_t h = 1469598103934665603ull; // FNV-1a over the float bits
    for (const float v : samples) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        for (int b = 0; b < 4; ++b) {
            h ^= (bits >> (8 * b)) & 0xffu;
            h *= 1099511628211ull;
        }
    }
    return h;
}
} // namespace

TEST_CASE("FIX-12: Original and Delta stay under the limiter ceiling", "[plugin][fix12]")
{
    const float ceiling = ceilingGain();
    for (const int channels : {1, 2}) {
        Driver d(channels, 0.8f);
        const float settlePeak = d.run(300, kProcessed); // 3 s: the level match settles
        const float matchGain = juce::Decibels::decibelsToGain(d.stage().matchGainDb());
        CHECK(d.stage().monitorTrimDb() == 0.0f); // the processed side is never trimmed

        const float originalPeak = d.run(600, kOriginal); // includes the crossfade in
        const float deltaPeak = d.run(600, kDelta);
        const float backPeak = d.run(100, kProcessed);
        // Switching every block restarts the 15 ms crossfade before it ends.
        float togglePeak = 0.0f;
        const int modes[] = {kOriginal, kDelta, kProcessed, kDelta, kOriginal, kBlind, kProcessed};
        for (int k = 0; k < 70; ++k) togglePeak = juce::jmax(togglePeak, d.run(1, modes[k % 7]));

        INFO(channels << " ch: match gain " << d.stage().matchGainDb() << " dB, matched dry peak "
                      << kBurstPeak * matchGain << ", ceiling " << ceiling << "; output peak Processed " << settlePeak
                      << ", Original " << originalPeak << ", Delta " << deltaPeak << ", back to Processed " << backPeak
                      << ", toggling " << togglePeak);
        // The premise: at the matched level the dry peaks are far over the ceiling.
        REQUIRE(kBurstPeak * matchGain > 2.0f * ceiling);
        CHECK(settlePeak <= ceiling);
        CHECK(originalPeak <= ceiling);
        CHECK(deltaPeak <= ceiling);
        CHECK(backPeak <= ceiling);
        CHECK(togglePeak <= ceiling);
    }
}

TEST_CASE("FIX-12: an Original held under the ceiling is still the dry signal, turned down", "[plugin][fix12]")
{
    // Not clipped and not limited: one gain for both channels, so the Original
    // keeps its own dynamics and stereo image. It uses all the headroom there
    // is: the dry peak lands on the ceiling.
    const float ceiling = ceilingGain();
    for (const int channels : {1, 2}) {
        Driver d(channels, 0.8f);
        d.run(300, kProcessed);
        d.run(600, kOriginal);
        const float peak = d.run(1, kOriginal);
        const int latency = d.stage().latencySamples();
        const float gain = ceiling / kBurstPeak;
        float worst = 0.0f;
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < kBlock; ++i) {
                const float expected = gain * burstDry(c, d.lastBlockStart() + i - latency);
                worst = juce::jmax(worst, std::abs(d.lastBlock().getSample(c, i) - expected));
            }
        INFO(channels << " ch: peak " << peak << ", worst deviation from a pure gain " << worst);
        CHECK(worst <= 1.0e-6f);
        CHECK(peak == Catch::Approx(ceiling).epsilon(1.0e-5));
        // The stage says how far short of a level match that leaves the Original.
        const float expectedTrimDb = juce::Decibels::gainToDecibels(gain) - d.stage().matchGainDb();
        INFO("match gain " << d.stage().matchGainDb() << " dB, trim " << d.stage().monitorTrimDb() << " dB");
        CHECK(expectedTrimDb < -6.0f);
        CHECK(d.stage().monitorTrimDb() == Catch::Approx(expectedTrimDb).margin(0.01));
    }
}

TEST_CASE("FIX-12: a Blind trial stays level matched when the Original has to be turned down", "[plugin][fix12]")
{
    // A blind comparison with one side quieter is no comparison: the hidden
    // processed side takes the same trim as the hidden original.
    const float ceiling = ceilingGain();
    const auto rms = [](const std::vector<float>& x) {
        double sum = 0.0;
        for (const float v : x) sum += static_cast<double>(v) * v;
        return std::sqrt(sum / static_cast<double>(x.size()));
    };
    for (const int channels : {1, 2}) {
        Driver d(channels, 0.8f);
        d.run(300, kProcessed);
        d.run(3, kBlind);
        double rmsProcessed = -1.0, rmsOriginal = -1.0;
        float peak = 0.0f;
        for (int attempt = 0; attempt < 200 && (rmsProcessed < 0.0 || rmsOriginal < 0.0); ++attempt) {
            d.stage().requestBlindReshuffle();
            peak = juce::jmax(peak, d.run(3, kBlind)); // the draw, and its crossfade
            const bool processedSide = d.stage().blindIsProcessed();
            std::vector<float> out;
            peak = juce::jmax(peak, d.run(10, kBlind, &out)); // one whole burst period
            (processedSide ? rmsProcessed : rmsOriginal) = rms(out);
        }
        REQUIRE(rmsProcessed > 0.0);
        REQUIRE(rmsOriginal > 0.0);
        const double differenceDb = 20.0 * std::log10(rmsProcessed / rmsOriginal);
        INFO(channels << " ch: blind processed rms " << rmsProcessed << ", blind original rms " << rmsOriginal
                      << " (" << differenceDb << " dB apart), peak " << peak << ", trim "
                      << d.stage().monitorTrimDb() << " dB");
        CHECK(std::abs(differenceDb) < 0.5);
        CHECK(peak <= ceiling);
        CHECK(d.stage().monitorTrimDb() < -6.0f);
    }
}

TEST_CASE("FIX-12: with the limiter off there is no ceiling and nothing is trimmed", "[plugin][fix12]")
{
    const float ceiling = ceilingGain();
    for (const int channels : {1, 2}) {
        Driver d(channels, 0.8f);
        d.run(300, kProcessed, nullptr, false);
        const float originalPeak = d.run(100, kOriginal, nullptr, false);
        const float matchGain = juce::Decibels::decibelsToGain(d.stage().matchGainDb());
        INFO(channels << " ch: Original peak " << originalPeak << ", match gain " << d.stage().matchGainDb() << " dB");
        CHECK(originalPeak > 2.0f * ceiling);
        CHECK(originalPeak == Catch::Approx(kBurstPeak * matchGain).epsilon(0.05));
        CHECK(d.stage().monitorTrimDb() == 0.0f);
    }
}

TEST_CASE("FIX-12: with headroom to spare the Original is level matched exactly as before", "[plugin][fix12]")
{
    // The same signal as the limiter-off level-match test, with the limiter
    // on: 0.1 in, +6 dB of "processing", so the matched Original peaks at 0.2.
    for (const int channels : {1, 2}) {
        CopilotOutputStage stage;
        stage.prepareToPlay(kRate, kBlock, channels);
        juce::AudioBuffer<float> buf(channels, kBlock);
        long long pos = 0;
        double sumSquares = 0.0;
        for (int block = 0; block < 2000; ++block) {
            for (int c = 0; c < channels; ++c)
                for (int i = 0; i < kBlock; ++i) buf.setSample(c, i, denseWet(c, pos + i, 0.1f));
            pos += kBlock;
            stage.captureDry(buf, kOriginal);
            buf.applyGain(2.0f);
            stage.process(buf, false, true, kCeilingDb, kOriginal, 0);
            sumSquares = 0.0;
            for (int c = 0; c < channels; ++c)
                for (int i = 0; i < kBlock; ++i) sumSquares += double(buf.getSample(c, i)) * buf.getSample(c, i);
        }
        const double rms = std::sqrt(sumSquares / (channels * kBlock));
        CHECK(rms == Catch::Approx(0.2 / std::sqrt(2.0)).epsilon(0.02));
        CHECK(stage.matchGainDb() == Catch::Approx(6.02f).margin(0.1f));
        CHECK(stage.monitorTrimDb() == 0.0f);
    }
}

TEST_CASE("FIX-12: the processed output does not depend on the dry side or on earlier A/B listening",
          "[plugin][fix12]")
{
    // A hot processed signal, so the limiter is working throughout.
    for (const int channels : {1, 2}) {
        std::vector<float> reference, silentDry, afterAb;
        Driver a(channels, 1.5f, Dry::Bursts);
        a.run(400, kProcessed, &reference);

        Driver b(channels, 1.5f, Dry::Silent);
        b.run(400, kProcessed, &silentDry);
        CHECK(reference == silentDry);

        Driver c(channels, 1.5f, Dry::Bursts);
        c.run(100, kProcessed, &afterAb);
        c.run(100, kOriginal, &afterAb);
        c.run(100, kDelta, &afterAb);
        c.run(100, kProcessed, &afterAb);
        REQUIRE(afterAb.size() == reference.size());
        // Bit-identical before the first switch, and again once the 15 ms
        // crossfade back to Processed has finished (2 blocks).
        const size_t perBlock = static_cast<size_t>(channels * kBlock);
        const bool sameBefore = std::equal(reference.begin(), reference.begin() + static_cast<std::ptrdiff_t>(100 * perBlock),
                                           afterAb.begin());
        const bool sameAfter = std::equal(reference.begin() + static_cast<std::ptrdiff_t>(302 * perBlock), reference.end(),
                                          afterAb.begin() + static_cast<std::ptrdiff_t>(302 * perBlock));
        CHECK(sameBefore);
        CHECK(sameAfter);
    }
}

// Not a pass/fail test: prints a fingerprint (FNV-1a over the float bits) of
// the stage's output for a fixed set of signals, to compare before and after a
// change that must not alter them. Run with "[fix12-fingerprint]".
TEST_CASE("FIX-12: output fingerprints of the paths the ceiling guard must not change", "[.][fix12-fingerprint]")
{
    for (const int channels : {1, 2}) {
        {
            std::vector<float> out;
            Driver d(channels, 1.5f);
            const float peak = d.run(400, kProcessed, &out);
            WARN(channels << " ch, Processed, limiter working: " << fingerprint(out) << " peak " << peak);
        }
        {
            std::vector<float> out;
            Driver d(channels, 0.8f);
            const float peak = d.run(400, kProcessed, &out);
            WARN(channels << " ch, Processed, under the ceiling: " << fingerprint(out) << " peak " << peak);
        }
        {
            std::vector<float> out;
            Driver d(channels, 0.8f);
            d.run(100, kProcessed, &out, false);
            d.run(100, kOriginal, &out, false);
            d.run(100, kDelta, &out, false);
            const float peak = d.run(100, kProcessed, &out, false);
            WARN(channels << " ch, limiter off, Processed/Original/Delta/Processed: " << fingerprint(out)
                          << " last peak " << peak);
        }
        {
            // Original and Delta with headroom to spare (dry 0.1, +6 dB).
            std::vector<float> out;
            CopilotOutputStage stage;
            stage.prepareToPlay(kRate, kBlock, channels);
            juce::AudioBuffer<float> buf(channels, kBlock);
            long long pos = 0;
            for (int block = 0; block < 600; ++block) {
                const int mode = block < 200 ? kProcessed : (block < 400 ? kOriginal : kDelta);
                for (int c = 0; c < channels; ++c)
                    for (int i = 0; i < kBlock; ++i) buf.setSample(c, i, denseWet(c, pos + i, 0.1f));
                pos += kBlock;
                stage.captureDry(buf, mode);
                buf.applyGain(2.0f);
                stage.process(buf, false, true, kCeilingDb, mode, 0);
                for (int c = 0; c < channels; ++c)
                    for (int i = 0; i < kBlock; ++i) out.push_back(buf.getSample(c, i));
            }
            WARN(channels << " ch, limiter on, headroom to spare, Processed/Original/Delta: " << fingerprint(out));
        }
        {
            // Bypass fades (to the raw aligned dry) from Processed and back.
            std::vector<float> out;
            CopilotOutputStage stage;
            stage.prepareToPlay(kRate, kBlock, channels);
            juce::AudioBuffer<float> buf(channels, kBlock);
            long long pos = 0;
            for (int block = 0; block < 60; ++block) {
                const bool bypass = block >= 20 && block < 40;
                for (int c = 0; c < channels; ++c)
                    for (int i = 0; i < kBlock; ++i) buf.setSample(c, i, burstDry(c, pos + i));
                stage.captureDry(buf, kProcessed);
                if (stage.needsProcessedPath(bypass))
                    for (int c = 0; c < channels; ++c)
                        for (int i = 0; i < kBlock; ++i) buf.setSample(c, i, denseWet(c, pos + i, 1.5f));
                pos += kBlock;
                stage.process(buf, bypass, true, kCeilingDb, kProcessed, 0);
                for (int c = 0; c < channels; ++c)
                    for (int i = 0; i < kBlock; ++i) out.push_back(buf.getSample(c, i));
            }
            WARN(channels << " ch, limiter on, bypass in and out: " << fingerprint(out));
        }
    }
}
