// STU-5 / STU-9: the shared monitoring DSP (khensu/dsp/MonitorSolo, the
// desktop transport's Mono / Side / Sub / Dim and the plugin's Solo) and the
// live preview's block-wise chain (khensu/dsp/RealtimeChain):
//  * Mono = (L+R)/2, Side = (L-R)/2 on both sides, Sub = a Linkwitz-Riley
//    low-pass at 120 Hz, Dim = -20 dB, an offline pass hears none of it;
//  * the realtime chain matches the offline renderer sample for sample on
//    the causal stages (ChainProcessor in one block, and OfflineChainRenderer
//    through a file), plans the rest (limiter stand-in, skipped zero-phase
//    stages), holds the limiter's ceiling, and loudness-matches its output.
// (No-allocation checks: tests/plugin/RealtimePreviewAllocationTest.cpp.)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <khensu/dsp/ChainProcessor.h>
#include <khensu/dsp/MonitorSolo.h>
#include <khensu/dsp/OfflineChainRenderer.h>
#include <khensu/dsp/RealtimeChain.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <vector>

namespace kd = khensu::dsp;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000.0;

/// A stereo test signal: different tones left and right plus a little noise,
/// peaking well under full scale (no clip guard in the offline render).
std::vector<std::vector<float>> testSignal(int n, float scale = 0.25f)
{
    std::vector<std::vector<float>> ch(2, std::vector<float>(static_cast<size_t>(n)));
    std::uint32_t x = 12345u;
    for (int i = 0; i < n; ++i) {
        x = 1664525u * x + 1013904223u;
        const double noise = (static_cast<double>(x) / 4294967296.0) * 2.0 - 1.0;
        const double t = static_cast<double>(i) / kRate;
        const auto u = static_cast<size_t>(i);
        ch[0][u] = scale * static_cast<float>(0.6 * std::sin(2 * kPi * 60.0 * t) + 0.3 * std::sin(2 * kPi * 1500.0 * t) +
                                              0.1 * noise);
        ch[1][u] = scale * static_cast<float>(0.5 * std::sin(2 * kPi * 90.0 * t + 0.4) +
                                              0.3 * std::sin(2 * kPi * 4000.0 * t) + 0.1 * noise);
    }
    return ch;
}

juce::AudioBuffer<float> toBuffer(const std::vector<std::vector<float>>& ch)
{
    juce::AudioBuffer<float> b(static_cast<int>(ch.size()), static_cast<int>(ch.front().size()));
    for (int c = 0; c < b.getNumChannels(); ++c)
        b.copyFrom(c, 0, ch[static_cast<size_t>(c)].data(), b.getNumSamples());
    return b;
}

double rms(const float* x, int n)
{
    double s = 0.0;
    for (int i = 0; i < n; ++i) s += static_cast<double>(x[i]) * x[i];
    return std::sqrt(s / std::max(1, n));
}

/// Runs `solo` over `buf` in blocks of 512.
void runSolo(kd::MonitorSolo& solo, juce::AudioBuffer<float>& buf, bool offline = false)
{
    for (int start = 0; start < buf.getNumSamples(); start += 512)
        solo.process(buf, start, std::min(512, buf.getNumSamples() - start), offline);
}

/// The offline renderer's output for `chain` over the whole of `sig`
/// (profile pass, process, finish).
std::vector<std::vector<float>> renderOffline(const std::vector<kd::ChainStageSpec>& chain,
                                              const std::vector<std::vector<float>>& sig)
{
    kd::ChainProcessor offline;
    std::string error;
    REQUIRE(offline.prepare(chain, kRate, static_cast<int>(sig.size()), error));
    kd::PlanarBlock in;
    for (const auto& c : sig) in.ch.emplace_back(c.begin(), c.end());
    if (offline.needsProfilePass())
        REQUIRE(offline.runProfilePasses(
            [&](const std::function<void(kd::PlanarBlock&&)>& consume) {
                kd::PlanarBlock copy = in;
                consume(std::move(copy));
                return true;
            },
            error));
    std::vector<std::vector<float>> out(sig.size());
    const auto sink = [&](const kd::PlanarBlock& b) {
        for (size_t c = 0; c < out.size() && c < b.ch.size(); ++c)
            for (double v : b.ch[c]) out[c].push_back(static_cast<float>(v));
    };
    offline.process(in, sink);
    offline.finish(sink);
    REQUIRE(out[0].size() == sig[0].size());
    return out;
}

/// The live preview's output for `chain` (no loudness match), 512-sample blocks.
std::vector<std::vector<float>> renderLive(const std::vector<kd::ChainStageSpec>& chain,
                                           const std::vector<std::vector<float>>& sig)
{
    kd::RealtimeChain live;
    live.setLoudnessMatch(false);
    std::string error;
    const int nch = static_cast<int>(sig.size());
    REQUIRE(live.prepare(chain, kRate, nch, 512, error));
    for (const auto& stage : live.plan()) {
        INFO(stage.effect);
        CHECK(stage.mode != kd::RealtimeChain::StageMode::Skipped);
    }
    auto out = sig;
    const int n = static_cast<int>(sig[0].size());
    for (int pos = 0; pos < n; pos += 512) {
        float* ptrs[2] = {out[0].data() + pos, out[static_cast<size_t>(nch - 1)].data() + pos};
        live.process(ptrs, nch, std::min(512, n - pos));
    }
    for (const auto& c : out)
        for (float v : c) REQUIRE(std::isfinite(v));
    return out;
}

/// The amplitude of the `hz` component over x[from, from + n) (one DFT bin).
double toneAmplitude(const std::vector<float>& x, int from, int n, double hz)
{
    double re = 0.0, im = 0.0;
    for (int i = 0; i < n; ++i) {
        const double ph = 2.0 * kPi * hz * static_cast<double>(i) / kRate;
        re += static_cast<double>(x[static_cast<size_t>(from + i)]) * std::cos(ph);
        im -= static_cast<double>(x[static_cast<size_t>(from + i)]) * std::sin(ph);
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(n);
}

/// A stereo signal of sines: {hz, left amplitude, right amplitude}.
std::vector<std::vector<float>> sines(int n, std::initializer_list<std::array<double, 3>> tones)
{
    std::vector<std::vector<float>> ch(2, std::vector<float>(static_cast<size_t>(n), 0.0f));
    for (const auto& t : tones)
        for (int i = 0; i < n; ++i) {
            const double s = std::sin(2.0 * kPi * t[0] * static_cast<double>(i) / kRate);
            ch[0][static_cast<size_t>(i)] += static_cast<float>(t[1] * s);
            ch[1][static_cast<size_t>(i)] += static_cast<float>(t[2] * s);
        }
    return ch;
}

double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1e-12)); }

std::vector<kd::ChainStageSpec> causalChain()
{
    return {{"HighpassFilter", {{"cutoff_frequency_hz", 40.0f}, {"slope_db_per_oct", 24.0f}}},
            {"PeakFilter", {{"cutoff_frequency_hz", 1500.0f}, {"gain_db", -4.0f}, {"q", 1.2f}}},
            {"LowShelfFilter", {{"cutoff_frequency_hz", 120.0f}, {"gain_db", 2.5f}, {"q", 0.707f}}},
            {"HighShelfFilter", {{"cutoff_frequency_hz", 8000.0f}, {"gain_db", 1.5f}, {"q", 0.707f}}},
            {"Compressor", {{"threshold_db", -24.0f}, {"ratio", 3.0f}, {"attack_ms", 5.0f}, {"release_ms", 80.0f}}},
            {"TransientShaper", {{"attack_gain_db", 2.0f}, {"sustain_gain_db", -1.0f}}},
            {"StereoWidener", {{"width", 1.2f}}},
            {"MidSideProcessor", {{"side_gain_db", -1.0f}, {"mid_eq_gain_db", 1.0f}}},
            {"NoiseGate", {{"threshold_db", -70.0f}}},
            {"Gain", {{"gain_db", -2.0f}}}};
}

} // namespace

// ---- STU-5: monitoring --------------------------------------------------------

TEST_CASE("MonitorSolo: Mono, Side, Sub and Dim (STU-5)", "[stu5][monitor]")
{
    const int n = static_cast<int>(kRate); // 1 s
    const auto sig = testSignal(n);
    const int settled = static_cast<int>(kRate * kd::MonitorSolo::kFadeSeconds) + 1;

    SECTION("Mono sums (L+R)/2 into both sides")
    {
        kd::MonitorSolo solo;
        solo.prepare(kRate, 512);
        solo.setMode(kd::MonitorSolo::Mode::Mono);
        auto buf = toBuffer(sig);
        runSolo(solo, buf);
        CHECK(solo.isActive());
        for (int i = settled; i < n; i += 97) {
            const float want = 0.5f * (sig[0][static_cast<size_t>(i)] + sig[1][static_cast<size_t>(i)]);
            REQUIRE_THAT(buf.getSample(0, i), Catch::Matchers::WithinAbs(want, 1e-6));
            REQUIRE_THAT(buf.getSample(1, i), Catch::Matchers::WithinAbs(want, 1e-6));
        }
    }
    SECTION("Side is (L-R)/2 on both sides")
    {
        kd::MonitorSolo solo;
        solo.prepare(kRate, 512);
        solo.setMode(kd::MonitorSolo::Mode::Side);
        auto buf = toBuffer(sig);
        runSolo(solo, buf);
        for (int i = settled; i < n; i += 97) {
            const float want = 0.5f * (sig[0][static_cast<size_t>(i)] - sig[1][static_cast<size_t>(i)]);
            REQUIRE_THAT(buf.getSample(0, i), Catch::Matchers::WithinAbs(want, 1e-6));
            REQUIRE_THAT(buf.getSample(1, i), Catch::Matchers::WithinAbs(want, 1e-6));
        }
    }
    SECTION("Sub is the input through a 120 Hz Linkwitz-Riley low-pass")
    {
        kd::MonitorSolo solo;
        solo.prepare(kRate, 512);
        solo.setMode(kd::MonitorSolo::Mode::Sub);
        auto buf = toBuffer(sig);
        runSolo(solo, buf);

        // Reference: JUCE's LR4 low-pass from a clean state over the same input.
        juce::dsp::LinkwitzRileyFilter<float> ref;
        ref.setType(juce::dsp::LinkwitzRileyFilterType::lowpass);
        ref.prepare({kRate, 512, 2});
        ref.setCutoffFrequency(kd::MonitorSolo::kSubHz);
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < 2; ++c) {
                const float want = ref.processSample(c, sig[static_cast<size_t>(c)][static_cast<size_t>(i)]);
                if (i >= settled && i % 61 == 0) REQUIRE_THAT(buf.getSample(c, i), Catch::Matchers::WithinAbs(want, 1e-5));
            }

        // And by ear: 60 Hz passes, 1.5 kHz / 4 kHz are gone (> 40 dB down).
        std::vector<std::vector<float>> tones(2, std::vector<float>(static_cast<size_t>(n)));
        for (int i = 0; i < n; ++i) {
            tones[0][static_cast<size_t>(i)] = 0.5f * static_cast<float>(std::sin(2 * kPi * 50.0 * i / kRate));
            tones[1][static_cast<size_t>(i)] = 0.5f * static_cast<float>(std::sin(2 * kPi * 1500.0 * i / kRate));
        }
        kd::MonitorSolo sub;
        sub.prepare(kRate, 512);
        sub.setMode(kd::MonitorSolo::Mode::Sub);
        auto tb = toBuffer(tones);
        runSolo(sub, tb);
        const int half = n / 2;
        CHECK(std::abs(20.0 * std::log10(rms(tb.getReadPointer(0, half), half) / (0.5 / std::sqrt(2.0)))) < 0.3);
        CHECK(20.0 * std::log10(rms(tb.getReadPointer(1, half), half) / (0.5 / std::sqrt(2.0))) < -40.0);
    }
    SECTION("Dim is -20 dB, combines with a mode, and ramps back")
    {
        kd::MonitorSolo solo;
        solo.prepare(kRate, 512);
        solo.setDim(true);
        auto buf = toBuffer(sig);
        runSolo(solo, buf);
        CHECK(solo.isActive());
        for (int i = settled; i < n; i += 97)
            REQUIRE_THAT(buf.getSample(0, i), Catch::Matchers::WithinAbs(0.1f * sig[0][static_cast<size_t>(i)], 1e-6));

        solo.setMode(kd::MonitorSolo::Mode::Mono);
        auto both = toBuffer(sig);
        runSolo(solo, both);
        for (int i = settled; i < n; i += 97) {
            const float want = 0.1f * 0.5f * (sig[0][static_cast<size_t>(i)] + sig[1][static_cast<size_t>(i)]);
            REQUIRE_THAT(both.getSample(1, i), Catch::Matchers::WithinAbs(want, 1e-6));
        }

        solo.setDim(false);
        solo.setMode(kd::MonitorSolo::Mode::Off);
        auto back = toBuffer(sig);
        runSolo(solo, back);
        for (int i = settled; i < n; i += 97)
            REQUIRE(juce::exactlyEqual(back.getSample(0, i), sig[0][static_cast<size_t>(i)]));
        auto idle = toBuffer(sig);
        runSolo(solo, idle);
        CHECK_FALSE(solo.isActive());
    }
    SECTION("an offline pass never hears monitoring")
    {
        kd::MonitorSolo solo;
        solo.prepare(kRate, 512);
        solo.setMode(kd::MonitorSolo::Mode::Side);
        solo.setDim(true);
        auto buf = toBuffer(sig);
        runSolo(solo, buf, /*offline*/ true);
        for (int i = 0; i < n; i += 97) REQUIRE(juce::exactlyEqual(buf.getSample(0, i), sig[0][static_cast<size_t>(i)]));
        CHECK_FALSE(solo.isActive());
    }
}

// ---- STU-9: the realtime chain ------------------------------------------------

TEST_CASE("RealtimeChain matches the offline chain on causal stages (STU-9)", "[stu9][realtime]")
{
    const int n = static_cast<int>(kRate * 1.5);
    const auto sig = testSignal(n);
    const auto chain = causalChain();

    // Offline: the renderer's processor, one block.
    kd::ChainProcessor offline;
    std::string error;
    REQUIRE(offline.prepare(chain, kRate, 2, error));
    kd::PlanarBlock in;
    for (const auto& c : sig) in.ch.emplace_back(c.begin(), c.end());
    std::vector<std::vector<double>> want(2);
    offline.process(in, [&](const kd::PlanarBlock& out) {
        for (size_t c = 0; c < 2; ++c) want[c].insert(want[c].end(), out.ch[c].begin(), out.ch[c].end());
    });
    REQUIRE(static_cast<int>(want[0].size()) == n);

    // Realtime: odd device blocks, through a smaller internal block.
    kd::RealtimeChain live;
    live.setLoudnessMatch(false);
    REQUIRE(live.prepare(chain, kRate, 2, 300, error));
    for (const auto& stage : live.plan()) {
        INFO(stage.effect);
        CHECK(stage.mode == kd::RealtimeChain::StageMode::Exact);
    }
    CHECK(live.latencySamples() == 0);
    auto buf = toBuffer(sig);
    const int blocks[] = {512, 37, 1024, 441, 2048};
    int pos = 0, k = 0;
    while (pos < n) {
        const int len = std::min(blocks[k++ % 5], n - pos);
        float* ptrs[2] = {buf.getWritePointer(0, pos), buf.getWritePointer(1, pos)};
        live.process(ptrs, 2, len);
        pos += len;
    }
    double worst = 0.0;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i)
            worst = std::max(worst, std::abs(buf.getSample(c, i) - want[static_cast<size_t>(c)][static_cast<size_t>(i)]));
    CHECK(worst < 1e-5);
}

TEST_CASE("RealtimeChain matches an offline file render on causal stages (STU-9)", "[stu9][realtime]")
{
    const int n = static_cast<int>(kRate * 1.0);
    const auto sig = testSignal(n, 0.15f);
    const std::vector<kd::ChainStageSpec> chain = {
        {"HighpassFilter", {{"cutoff_frequency_hz", 30.0f}, {"slope_db_per_oct", 12.0f}}},
        {"PeakFilter", {{"cutoff_frequency_hz", 250.0f}, {"gain_db", 3.0f}, {"q", 0.9f}}},
        {"Compressor", {{"threshold_db", -30.0f}, {"ratio", 2.0f}}},
        {"Gain", {{"gain_db", -1.0f}}}};

    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("khensu-stu9-preview-tests");
    dir.createDirectory();
    const auto input = dir.getChildFile("in.wav"), output = dir.getChildFile("out.wav");
    {
        input.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = input.createOutputStream();
        REQUIRE(stream != nullptr);
        auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                      .withSampleRate(kRate)
                                                      .withNumChannels(2)
                                                      .withBitsPerSample(32)
                                                      .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        REQUIRE(writer != nullptr);
        const float* ptrs[2] = {sig[0].data(), sig[1].data()};
        REQUIRE(writer->writeFromFloatArrays(ptrs, 2, n));
    }
    kd::OfflineRenderOptions options;
    options.enforceGuardrail = false;
    const auto result = kd::OfflineChainRenderer::renderFile(input, output, chain, options);
    REQUIRE(result.status == kd::OfflineRenderResult::Status::Rendered);
    REQUIRE(std::abs(result.outputGainDb) < 1e-9); // quiet: no output-stage trim to model

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(output));
    REQUIRE(reader != nullptr);
    juce::AudioBuffer<float> rendered(2, n);
    REQUIRE(reader->read(&rendered, 0, n, 0, true, true));
    reader.reset();

    kd::RealtimeChain live;
    live.setLoudnessMatch(false);
    std::string error;
    REQUIRE(live.prepare(chain, kRate, 2, 512, error));
    auto buf = toBuffer(sig);
    for (int pos = 0; pos < n; pos += 480) {
        float* ptrs[2] = {buf.getWritePointer(0, pos), buf.getWritePointer(1, pos)};
        live.process(ptrs, 2, std::min(480, n - pos));
    }
    double worst = 0.0;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i) worst = std::max(worst, (double)std::abs(buf.getSample(c, i) - rendered.getSample(c, i)));
    CHECK(worst < 1e-5);
    input.deleteFile();
    output.deleteFile();
}

TEST_CASE("RealtimeChain plans what it cannot run live (STU-9)", "[stu9][stu18][realtime]")
{
    const std::vector<kd::ChainStageSpec> chain = {
        {"PeakFilter", {{"cutoff_frequency_hz", 1000.0f}, {"gain_db", 3.0f}}},
        {"DeEsser", {{"frequency_hz", 6500.0f}}},
        {"DynamicEQ", {{"frequency_hz", 3000.0f}}},
        {"BassMonoMaker", {{"crossover_hz", 120.0f}}},
        {"TapeSaturation", {{"drive_db", 6.0f}}},
        {"Exciter", {{"frequency_hz", 3000.0f}}},
        {"Reverb", {{"room_size", 0.5f}}},
        {"MultibandCompressor", {}},
        {"SpectralDenoise", {}},
        {"Limiter", {{"threshold_db", -1.0f}}}};
    kd::RealtimeChain live;
    std::string error;
    REQUIRE(live.prepare(chain, kRate, 2, 512, error));
    const auto& plan = live.plan();
    REQUIRE(plan.size() == chain.size());
    CHECK(plan[0].mode == kd::RealtimeChain::StageMode::Exact);
    // STU-18: causal stand-ins, each with a note saying how it differs.
    for (size_t k : {1u, 2u, 3u, 4u, 5u, 9u}) {
        INFO(plan[k].effect);
        CHECK(plan[k].mode == kd::RealtimeChain::StageMode::CausalEquivalent);
        CHECK_FALSE(plan[k].note.empty());
    }
    for (size_t k : {6u, 7u, 8u}) {
        INFO(plan[k].effect);
        CHECK(plan[k].mode == kd::RealtimeChain::StageMode::Skipped);
        CHECK_FALSE(plan[k].note.empty());
    }
    // Only the limiter's lookahead delays the preview.
    CHECK(live.latencySamples() == static_cast<int>(std::lround(1.5e-3 * kRate)));
    CHECK_FALSE(live.isIdentity());

    kd::RealtimeChain nothing;
    REQUIRE(nothing.prepare({{"MultibandCompressor", {}}}, kRate, 2, 512, error));
    CHECK(nothing.isIdentity());

    // Where the render passes the signal unchanged, so does the preview -
    // exactly: a bass-mono on a mono signal, an exciter at mix 0.
    kd::RealtimeChain unchanged;
    REQUIRE(unchanged.prepare({{"BassMonoMaker", {}}, {"Exciter", {{"mix", 0.0f}}}}, kRate, 1, 512, error));
    for (const auto& stage : unchanged.plan()) {
        INFO(stage.effect);
        CHECK(stage.mode == kd::RealtimeChain::StageMode::Exact);
    }
    CHECK(unchanged.isIdentity());
}

TEST_CASE("RealtimeChain limiter stand-in holds the ceiling (STU-9)", "[stu9][realtime]")
{
    const int n = static_cast<int>(kRate);
    auto sig = testSignal(n, 1.6f); // peaks well over full scale
    kd::RealtimeChain live;
    live.setLoudnessMatch(false);
    std::string error;
    REQUIRE(live.prepare({{"Limiter", {{"threshold_db", -3.0f}}}}, kRate, 2, 512, error));
    auto buf = toBuffer(sig);
    for (int pos = 0; pos < n; pos += 512) {
        float* ptrs[2] = {buf.getWritePointer(0, pos), buf.getWritePointer(1, pos)};
        live.process(ptrs, 2, std::min(512, n - pos));
    }
    const float ceiling = juce::Decibels::decibelsToGain(-3.05f);
    CHECK(buf.getMagnitude(0, n) <= ceiling * 1.0001f);
}

TEST_CASE("RealtimeChain loudness-matches the preview to its input (STU-9)", "[stu9][realtime]")
{
    const int n = static_cast<int>(kRate * 4.0);
    const auto sig = testSignal(n, 0.2f);
    for (const float gainDb : {6.0f, -6.0f}) {
        INFO("gain " << gainDb);
        kd::RealtimeChain live; // the match is on by default
        std::string error;
        REQUIRE(live.prepare({{"Gain", {{"gain_db", gainDb}}}, {"PeakFilter", {{"cutoff_frequency_hz", 1500.0f}, {"gain_db", 6.0f}}}},
                             kRate, 2, 512, error));
        CHECK(live.predictedGainDb() == gainDb);
        auto buf = toBuffer(sig);
        for (int pos = 0; pos < n; pos += 512) {
            float* ptrs[2] = {buf.getWritePointer(0, pos), buf.getWritePointer(1, pos)};
            live.process(ptrs, 2, std::min(512, n - pos));
        }
        // Over the last second, the preview is as loud as the source (and
        // never louder by a meaningful margin).
        const int tail = static_cast<int>(kRate);
        const double inRms = rms(sig[0].data() + n - tail, tail) + rms(sig[1].data() + n - tail, tail);
        const double outRms = rms(buf.getReadPointer(0, n - tail), tail) + rms(buf.getReadPointer(1, n - tail), tail);
        CHECK(std::abs(20.0 * std::log10(outRms / inRms)) < 0.5);
    }

    // A new chain continues the old one's match, moved by the Gain change:
    // no jump while the averages settle.
    kd::RealtimeChain a, b;
    std::string error;
    REQUIRE(a.prepare({{"Gain", {{"gain_db", 0.0f}}}}, kRate, 2, 512, error));
    REQUIRE(b.prepare({{"Gain", {{"gain_db", 6.0f}}}}, kRate, 2, 512, error));
    auto buf = toBuffer(sig);
    float* ptrs[2] = {buf.getWritePointer(0), buf.getWritePointer(1)};
    a.process(ptrs, 2, static_cast<int>(kRate));
    b.adoptMatchState(a);
    CHECK_THAT(b.matchGainDb(), Catch::Matchers::WithinAbs(a.matchGainDb() - 6.0f, 0.05));
}

// ---- STU-18: the stand-ins track the offline stages ----------------------------
// Not sample for sample (the render's filters are zero-phase, its exciter
// level comes from the whole track): the same audible effect, measured on
// tones over the last half second.

TEST_CASE("Live bass mono removes the low side as the render does (STU-18)", "[stu18][realtime]")
{
    const int n = static_cast<int>(kRate * 1.5);
    // A 50 Hz tone in anti-phase (all side) and a 2 kHz one, also all side.
    const auto sig = sines(n, {{{50.0, 0.3, -0.3}}, {{2000.0, 0.2, -0.2}}});
    const std::vector<kd::ChainStageSpec> chain = {{"BassMonoMaker", {{"crossover_hz", 120.0f}}}};
    const auto want = renderOffline(chain, sig);
    const auto got = renderLive(chain, sig);
    const int from = n - static_cast<int>(kRate / 2), len = static_cast<int>(kRate / 2);
    for (const auto* out : {&want, &got}) {
        std::vector<float> side(static_cast<size_t>(n));
        for (size_t i = 0; i < side.size(); ++i) side[i] = 0.5f * ((*out)[0][i] - (*out)[1][i]);
        CHECK(toneAmplitude(side, from, len, 50.0) < 0.3 * 0.05);  // the low side is gone (< -26 dB)
        CHECK(toneAmplitude(side, from, len, 2000.0) > 0.2 * 0.95); // the image above stays
    }
}

TEST_CASE("Live tape saturation adds the render's harmonics (STU-18)", "[stu18][realtime]")
{
    const int n = static_cast<int>(kRate * 1.0);
    const auto sig = sines(n, {{{1000.0, 0.5, 0.5}}});
    const std::vector<kd::ChainStageSpec> chain = {
        {"TapeSaturation", {{"drive_db", 12.0f}, {"warmth", 0.5f}, {"mix", 1.0f}}}};
    const auto want = renderOffline(chain, sig);
    const auto got = renderLive(chain, sig);
    const int from = n - static_cast<int>(kRate / 2), len = static_cast<int>(kRate / 2);
    for (const double hz : {2000.0, 3000.0}) {
        INFO(hz << " Hz");
        const double offline = toneAmplitude(want[0], from, len, hz) / toneAmplitude(want[0], from, len, 1000.0);
        const double live = toneAmplitude(got[0], from, len, hz) / toneAmplitude(got[0], from, len, 1000.0);
        CHECK(offline > 0.01); // the curve really distorts (> -40 dB)
        CHECK(std::abs(db(live) - db(offline)) < 1.5);
    }
    CHECK(std::abs(db(toneAmplitude(got[0], from, len, 1000.0)) - db(toneAmplitude(want[0], from, len, 1000.0))) <
          0.5);
}

TEST_CASE("Live dynamic EQ and de-esser reduce only over the threshold (STU-18)", "[stu18][realtime]")
{
    const int n = static_cast<int>(kRate * 1.5);
    const int from = n - static_cast<int>(kRate / 2), len = static_cast<int>(kRate / 2);
    struct Case {
        const char* name;
        kd::ChainStageSpec stage;
        double hz;
    };
    const Case cases[] = {
        {"DynamicEQ bell",
         {"DynamicEQ", {{"frequency_hz", 1000.0f}, {"q", 2.0f}, {"threshold_db", -30.0f}, {"ratio", 4.0f}}},
         1000.0},
        {"DynamicEQ high shelf",
         {"DynamicEQ", {{"frequency_hz", 4000.0f}, {"shape", 2.0f}, {"threshold_db", -30.0f}, {"ratio", 3.0f}}},
         9000.0},
        {"DeEsser", {"DeEsser", {{"frequency_hz", 6500.0f}, {"threshold_db", -30.0f}, {"ratio", 4.0f}}}, 6500.0}};
    for (const auto& c : cases) {
        INFO(c.name);
        const std::vector<kd::ChainStageSpec> chain = {c.stage};
        // Over the threshold: both reduce the tone by a similar amount.
        const auto loud = sines(n, {{{c.hz, 0.3, 0.3}}});
        const double in = toneAmplitude(loud[0], from, len, c.hz);
        const double offline = db(toneAmplitude(renderOffline(chain, loud)[0], from, len, c.hz) / in);
        const double live = db(toneAmplitude(renderLive(chain, loud)[0], from, len, c.hz) / in);
        INFO("offline " << offline << " dB, live " << live << " dB");
        CHECK(offline < -6.0);
        CHECK(live < -6.0);
        CHECK(std::abs(live - offline) < 4.0);
        // Well under it: neither touches the tone.
        const auto quiet = sines(n, {{{c.hz, 0.003, 0.003}}});
        const double qin = toneAmplitude(quiet[0], from, len, c.hz);
        CHECK(std::abs(db(toneAmplitude(renderLive(chain, quiet)[0], from, len, c.hz) / qin)) < 0.3);
        CHECK(std::abs(db(toneAmplitude(renderOffline(chain, quiet)[0], from, len, c.hz) / qin)) < 0.3);
    }
}

TEST_CASE("Live exciter adds the render's upper harmonics (STU-18)", "[stu18][realtime]")
{
    const int n = static_cast<int>(kRate * 1.5);
    // A low tone the exciter must leave alone and a high one it excites.
    const auto sig = sines(n, {{{200.0, 0.3, 0.3}}, {{5000.0, 0.2, 0.2}}});
    const std::vector<kd::ChainStageSpec> chain = {
        {"Exciter", {{"frequency_hz", 3000.0f}, {"drive", 4.0f}, {"even_harmonics", 0.35f}, {"mix", 0.3f}}}};
    const auto want = renderOffline(chain, sig);
    const auto got = renderLive(chain, sig);
    const int from = n - static_cast<int>(kRate / 2), len = static_cast<int>(kRate / 2);
    // The even (10 kHz) and odd (15 kHz) harmonics of the high tone.
    for (const double hz : {10000.0, 15000.0}) {
        INFO(hz << " Hz");
        const double offline = toneAmplitude(want[0], from, len, hz);
        const double live = toneAmplitude(got[0], from, len, hz);
        CHECK(offline > 1e-3);
        CHECK(std::abs(db(live / offline)) < 3.0);
    }
    // The low tone passes at its level in both.
    CHECK(std::abs(db(toneAmplitude(got[0], from, len, 200.0) / 0.3)) < 0.2);
    CHECK(std::abs(db(toneAmplitude(want[0], from, len, 200.0) / 0.3)) < 0.2);
}
