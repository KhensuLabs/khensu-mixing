// ENH-6: the native chain stages ported from the service (ExtendedChainStages):
// DynamicEQ (self-keyed and keyed), MultibandCompressor, MidSideProcessor,
// Exciter, BassMonoMaker, NoiseGate - their laws on known signals, block-size
// independence of the chunked zero-phase stages, the renderer's key input and
// the masking finding's keyed dynamic EQ. Parity with the service's own
// renders of the same chains is in test_offline_chain_renderer.cpp (the
// render_parity.json cases scripts/gen_render_parity_fixtures.py writes).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <khensu/dsp/ChainProcessor.h>
#include <khensu/dsp/ExtendedChainStages.h>
#include <khensu/dsp/OfflineChainRenderer.h>
#include <khensu/recommendations/MaskingChain.h>
#include <khensu/recommendations/MaskingRecommender.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <map>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;
namespace kd = khensu::dsp;

namespace {

constexpr double kPi = 3.14159265358979323846;
using Signal = std::vector<std::vector<double>>; // [channel][sample]

kd::ChainStageSpec stage(const char* effect, std::map<std::string, float> params = {})
{
    return {effect, std::move(params)};
}

std::vector<double> sine(double freq, double amp, double sr, int n, int from = 0, int to = -1)
{
    std::vector<double> y(static_cast<size_t>(n), 0.0);
    if (to < 0) to = n;
    for (int i = from; i < to; ++i) y[static_cast<size_t>(i)] = amp * std::sin(2.0 * kPi * freq * i / sr);
    return y;
}

kd::PlanarBlock slice(const Signal& s, size_t pos, size_t len)
{
    kd::PlanarBlock b;
    for (const auto& c : s)
        b.ch.emplace_back(c.begin() + static_cast<std::ptrdiff_t>(pos),
                          c.begin() + static_cast<std::ptrdiff_t>(pos + len));
    return b;
}

/// Runs `chain` over `in` in blocks (with profile passes and an optional key).
Signal runChain(const std::vector<kd::ChainStageSpec>& chain, const Signal& in, double sr, int blockSize,
                const Signal* key = nullptr)
{
    kd::ChainProcessor proc;
    std::string error;
    REQUIRE(proc.prepare(chain, sr, static_cast<int>(in.size()), error));
    if (key != nullptr) REQUIRE(proc.attachKey(static_cast<int>(key->size())));
    const size_t n = in.front().size();
    if (proc.needsProfilePass()) {
        const auto replay = [&](const std::function<void(kd::PlanarBlock&&)>& consume) {
            for (size_t pos = 0; pos < n; pos += static_cast<size_t>(blockSize))
                consume(slice(in, pos, std::min(static_cast<size_t>(blockSize), n - pos)));
            return true;
        };
        REQUIRE(proc.runProfilePasses(replay, error));
    }
    Signal out(in.size());
    const auto sink = [&out](const kd::PlanarBlock& b) {
        for (size_t c = 0; c < b.ch.size(); ++c) out[c].insert(out[c].end(), b.ch[c].begin(), b.ch[c].end());
    };
    for (size_t pos = 0; pos < n; pos += static_cast<size_t>(blockSize)) {
        const size_t len = std::min(static_cast<size_t>(blockSize), n - pos);
        if (key != nullptr) {
            const auto k = slice(*key, pos, len);
            proc.process(slice(in, pos, len), sink, &k);
        } else {
            proc.process(slice(in, pos, len), sink);
        }
    }
    proc.finish(sink);
    for (const auto& c : out) REQUIRE(c.size() == n); // sample-aligned, same length
    return out;
}

double rms(const std::vector<double>& x, size_t from, size_t to)
{
    double acc = 0.0;
    for (size_t i = from; i < to; ++i) acc += x[i] * x[i];
    return std::sqrt(acc / static_cast<double>(std::max<size_t>(1, to - from)));
}

double dB(double v)
{
    return 20.0 * std::log10(std::max(v, 1e-30));
}

double magnitudeAt(const std::vector<double>& h, double f, double sr)
{
    std::complex<double> acc = 0.0;
    for (size_t i = 0; i < h.size(); ++i)
        acc += h[i] * std::polar(1.0, -2.0 * kPi * f * static_cast<double>(i) / sr);
    return std::abs(acc);
}

juce::File tempDir()
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("khensu-enh6-tests");
    dir.createDirectory();
    return dir;
}

void writeWav(const juce::File& file, const Signal& chans, double sr)
{
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    REQUIRE(stream != nullptr);
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(sr)
                             .withNumChannels(static_cast<int>(chans.size()))
                             .withBitsPerSample(32)
                             .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto writer = wav.createWriterFor(stream, options);
    REQUIRE(writer != nullptr);
    std::vector<std::vector<float>> f;
    for (const auto& c : chans) {
        auto& dst = f.emplace_back(c.size());
        for (size_t i = 0; i < c.size(); ++i) dst[i] = static_cast<float>(c[i]);
    }
    std::vector<const float*> ptrs;
    for (const auto& c : f) ptrs.push_back(c.data());
    REQUIRE(writer->writeFromFloatArrays(ptrs.data(), static_cast<int>(ptrs.size()),
                                         static_cast<int>(f.front().size())));
}

Signal readWav(const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    REQUIRE(reader != nullptr);
    const int n = static_cast<int>(reader->lengthInSamples);
    juce::AudioBuffer<float> buf(static_cast<int>(reader->numChannels), n);
    REQUIRE(reader->read(&buf, 0, n, 0, true, true));
    Signal out(static_cast<size_t>(buf.getNumChannels()));
    for (int c = 0; c < buf.getNumChannels(); ++c)
        out[static_cast<size_t>(c)].assign(buf.getReadPointer(c), buf.getReadPointer(c) + n);
    return out;
}

} // namespace

TEST_CASE("ENH-6: every ported effect is native", "[dsp][render][enh6]")
{
    for (const char* e :
         {"DynamicEQ", "MultibandCompressor", "MidSideProcessor", "Exciter", "BassMonoMaker", "NoiseGate"})
        CHECK(kd::isNativeChainEffect(e));
    CHECK(kd::effectsNeedingEngine({stage("DynamicEQ"), stage("Reverb")}) == std::vector<std::string>{"Reverb"});
    // A non-finite parameter is refused, as the service refuses it.
    std::string error;
    CHECK(kd::makeChainStage(stage("DynamicEQ", {{"q", std::nanf("")}}), 44100.0, 2, error) == nullptr);
}

TEST_CASE("ENH-6: a DynamicEQ bell cuts only above its threshold", "[dsp][render][enh6]")
{
    const double sr = 44100.0;
    const int n = 3 * 44100;
    const auto spec = stage("DynamicEQ", {{"frequency_hz", 1000.0f},
                                          {"q", 2.0f},
                                          {"threshold_db", -30.0f},
                                          {"ratio", 4.0f},
                                          {"max_reduction_db", 24.0f}});
    const size_t from = 44100, to = static_cast<size_t>(n) - 44100; // steady state, clear of the ends

    SECTION("quiet: under the threshold, untouched")
    {
        const Signal in{sine(1000.0, 0.003, sr, n)}; // -50 dBFS
        const auto out = runChain({spec}, in, sr, 4096);
        double worst = 0.0;
        for (size_t i = 0; i < in[0].size(); ++i) worst = std::max(worst, std::abs(out[0][i] - in[0][i]));
        CHECK(worst < 1e-12);
    }
    SECTION("loud: cut by the ratio law")
    {
        const Signal in{sine(1000.0, 0.5, sr, n)};
        auto upper = spec;
        upper.parameters["threshold_db"] = -24.0f; // clear of the 24 dB floor
        const auto out = runChain({upper}, in, sr, 4096);
        const double gotDb = dB(rms(out[0], from, to) / rms(in[0], from, to));
        CHECK(gotDb < -10.0);
        // gr = (env / thr)^(1/ratio - 1): a threshold 8 dB lower cuts
        // 8 x (1 - 1/4) = 6 dB more on the same steady tone.
        auto lower = spec;
        lower.parameters["threshold_db"] = -32.0f;
        const auto out2 = runChain({lower}, in, sr, 4096);
        const double got2Db = dB(rms(out2[0], from, to) / rms(in[0], from, to));
        INFO("cut " << gotDb << " dB at -24, " << got2Db << " dB at -32");
        CHECK_THAT(got2Db - gotDb, WithinAbs(-6.0, 0.1));
    }
    SECTION("max_reduction_db floors the cut")
    {
        auto floored = spec;
        floored.parameters["max_reduction_db"] = 3.0f;
        const Signal in{sine(1000.0, 0.5, sr, n)};
        const auto out = runChain({floored}, in, sr, 4096);
        CHECK_THAT(dB(rms(out[0], from, to) / rms(in[0], from, to)), WithinAbs(-3.0, 0.1));
    }
    SECTION("a tone outside the band passes while the band is cut")
    {
        Signal in{sine(1000.0, 0.5, sr, n)};
        const auto low = sine(100.0, 0.3, sr, n);
        for (size_t i = 0; i < in[0].size(); ++i) in[0][i] += low[i];
        const auto out = runChain({spec}, in, sr, 4096);
        // The 100 Hz component, read at its own frequency.
        std::vector<double> a(out[0].begin() + static_cast<std::ptrdiff_t>(from), out[0].begin() + static_cast<std::ptrdiff_t>(to));
        std::vector<double> b(in[0].begin() + static_cast<std::ptrdiff_t>(from), in[0].begin() + static_cast<std::ptrdiff_t>(to));
        CHECK_THAT(dB(magnitudeAt(a, 100.0, sr) / magnitudeAt(b, 100.0, sr)), WithinAbs(0.0, 0.1));
        CHECK(dB(magnitudeAt(a, 1000.0, sr) / magnitudeAt(b, 1000.0, sr)) < -10.0);
    }
    SECTION("a low shelf cuts below its frequency only")
    {
        auto shelf = spec;
        shelf.parameters["shape"] = 1.0f;
        shelf.parameters["frequency_hz"] = 300.0f;
        Signal in{sine(100.0, 0.5, sr, n)};
        const auto hi = sine(3000.0, 0.3, sr, n);
        for (size_t i = 0; i < in[0].size(); ++i) in[0][i] += hi[i];
        const auto out = runChain({shelf}, in, sr, 4096);
        std::vector<double> a(out[0].begin() + static_cast<std::ptrdiff_t>(from), out[0].begin() + static_cast<std::ptrdiff_t>(to));
        std::vector<double> b(in[0].begin() + static_cast<std::ptrdiff_t>(from), in[0].begin() + static_cast<std::ptrdiff_t>(to));
        CHECK(dB(magnitudeAt(a, 100.0, sr) / magnitudeAt(b, 100.0, sr)) < -6.0);
        CHECK_THAT(dB(magnitudeAt(a, 3000.0, sr) / magnitudeAt(b, 3000.0, sr)), WithinAbs(0.0, 0.1));
    }
}

TEST_CASE("ENH-6: a keyed DynamicEQ cuts only while the key is loud", "[dsp][render][enh6]")
{
    const double sr = 44100.0;
    const int n = 4 * 44100, half = n / 2;
    // The bed: a steady 1 kHz tone, loud enough to trip the threshold alone.
    const Signal bed{sine(1000.0, 0.3, sr, n), sine(1000.0, 0.3, sr, n)};
    // The key (a "vocal" stem, mono): silent, then loud in the same band.
    const Signal key{sine(1000.0, 0.5, sr, n, half, n)};
    auto spec = stage("DynamicEQ",
                      {{"frequency_hz", 1000.0f}, {"q", 2.0f}, {"threshold_db", -30.0f}, {"max_reduction_db", 9.0f}});

    // Self-keyed (no use_key): the bed's own band trips it the whole way.
    const auto self = runChain({spec}, bed, sr, 8192);
    CHECK(dB(rms(self[0], 22050, static_cast<size_t>(half) - 22050) / rms(bed[0], 22050, static_cast<size_t>(half) - 22050)) < -5.0);

    spec.parameters["use_key"] = 1.0f;
    const auto keyed = runChain({spec}, bed, sr, 8192, &key);
    const size_t q1 = 22050, q2 = static_cast<size_t>(half) - 22050, q3 = static_cast<size_t>(half) + 22050,
                 q4 = static_cast<size_t>(n) - 22050;
    for (size_t c = 0; c < 2; ++c) {
        // Key silent: no cut at all.
        CHECK_THAT(dB(rms(keyed[c], q1, q2) / rms(bed[c], q1, q2)), WithinAbs(0.0, 0.01));
        // Key loud: the full (floored) cut.
        CHECK_THAT(dB(rms(keyed[c], q3, q4) / rms(bed[c], q3, q4)), WithinAbs(-9.0, 0.2));
    }
    // use_key without a key attached keys on the input itself.
    const auto unattached = runChain({spec}, bed, sr, 8192);
    CHECK(dB(rms(unattached[0], q1, q2) / rms(bed[0], q1, q2)) < -5.0);
}

TEST_CASE("ENH-6: M/S EQ on a mono signal leaves the side at zero", "[dsp][render][enh6]")
{
    const double sr = 48000.0;
    const int n = 48000;
    auto x = sine(220.0, 0.4, sr, n);
    const auto y = sine(5000.0, 0.2, sr, n);
    for (size_t i = 0; i < x.size(); ++i) x[i] += y[i];
    const Signal in{x, x};
    const auto spec = stage("MidSideProcessor", {{"mid_gain_db", -2.0f},
                                                 {"side_gain_db", 6.0f},
                                                 {"mid_eq_frequency_hz", 300.0f},
                                                 {"mid_eq_gain_db", -6.0f},
                                                 {"side_eq_frequency_hz", 4000.0f},
                                                 {"side_eq_gain_db", 6.0f},
                                                 {"side_highpass_frequency_hz", 150.0f}});
    const auto out = runChain({spec}, in, sr, 1000);
    double side = 0.0;
    for (size_t i = 0; i < x.size(); ++i) side = std::max(side, std::abs(out[0][i] - out[1][i]));
    CHECK(side == 0.0);
    // ... and the mid path did its EQ and gain (the 220 Hz bell cut).
    CHECK(rms(out[0], 4800, x.size()) < rms(x, 4800, x.size()));

    SECTION("the service's gains only: L/R from scaled mid and side")
    {
        Signal st{sine(300.0, 0.4, sr, n), sine(500.0, 0.3, sr, n)};
        const auto o = runChain({stage("MidSideProcessor", {{"mid_gain_db", -6.0f}, {"side_gain_db", 3.0f}})}, st, sr, 777);
        const double gm = std::pow(10.0, -6.0 / 20.0), gs = std::pow(10.0, 3.0 / 20.0);
        for (size_t i = 0; i < 2000; ++i) {
            const double m = (st[0][i] + st[1][i]) * 0.5 * gm, s = (st[0][i] - st[1][i]) * 0.5 * gs;
            REQUIRE_THAT(o[0][i], WithinAbs(m + s, 1e-12));
            REQUIRE_THAT(o[1][i], WithinAbs(m - s, 1e-12));
        }
    }
    SECTION("a mono file passes unchanged")
    {
        const Signal mono{x};
        const auto o = runChain({spec}, mono, sr, 1000);
        CHECK(o[0] == x);
    }
}

TEST_CASE("ENH-6: multiband crossovers sum flat at ratio 1", "[dsp][render][enh6]")
{
    const double sr = 44100.0;
    const int n = 65536;
    Signal impulse{std::vector<double>(static_cast<size_t>(n), 0.0)};
    impulse[0][static_cast<size_t>(n / 2)] = 1.0; // zero-phase: centred
    const auto spec = stage("MultibandCompressor", {{"low_crossover_hz", 200.0f},
                                                    {"high_crossover_hz", 3000.0f},
                                                    {"low_ratio", 1.0f},
                                                    {"mid_ratio", 1.0f},
                                                    {"high_ratio", 1.0f},
                                                    {"low_threshold_db", -60.0f},
                                                    {"mid_threshold_db", -60.0f},
                                                    {"high_threshold_db", -60.0f}});
    const auto h = runChain({spec}, impulse, sr, 4096)[0];
    double worst = 0.0;
    for (double f = 20.0; f < 20000.0; f *= 1.12) worst = std::max(worst, std::abs(dB(magnitudeAt(h, f, sr))));
    INFO("worst deviation " << worst << " dB");
    CHECK(worst < 0.1);
}

TEST_CASE("ENH-6: the multiband compressor squeezes only the loud band", "[dsp][render][enh6]")
{
    const double sr = 44100.0;
    const int n = 3 * 44100;
    Signal in{sine(80.0, 0.6, sr, n)};
    const auto hi = sine(6000.0, 0.01, sr, n); // -40 dBFS: under every threshold
    for (size_t i = 0; i < in[0].size(); ++i) in[0][i] += hi[i];
    const auto out = runChain({stage("MultibandCompressor", {{"low_threshold_db", -20.0f},
                                                             {"low_ratio", 4.0f},
                                                             {"high_threshold_db", -20.0f},
                                                             {"high_ratio", 4.0f}})},
                              in, sr, 4096);
    std::vector<double> a(out[0].begin() + 44100, out[0].end() - 22050), b(in[0].begin() + 44100, in[0].end() - 22050);
    CHECK(dB(magnitudeAt(a, 80.0, sr) / magnitudeAt(b, 80.0, sr)) < -6.0);
    CHECK_THAT(dB(magnitudeAt(a, 6000.0, sr) / magnitudeAt(b, 6000.0, sr)), WithinAbs(0.0, 0.1));
}

TEST_CASE("ENH-6: BassMonoMaker folds the low side to mono, keeps the highs", "[dsp][render][enh6]")
{
    const double sr = 44100.0;
    const int n = 2 * 44100;
    // 60 Hz in anti-phase (all side), 4 kHz in anti-phase too.
    Signal in{sine(60.0, 0.5, sr, n), sine(60.0, -0.5, sr, n)};
    const auto hiL = sine(4000.0, 0.2, sr, n);
    for (size_t i = 0; i < in[0].size(); ++i) {
        in[0][i] += hiL[i];
        in[1][i] -= hiL[i];
    }
    const auto out = runChain({stage("BassMonoMaker", {{"crossover_hz", 150.0f}})}, in, sr, 3000);
    std::vector<double> a(out[0].begin() + 22050, out[0].end() - 22050), b(in[0].begin() + 22050, in[0].end() - 22050);
    CHECK(dB(magnitudeAt(a, 60.0, sr) / magnitudeAt(b, 60.0, sr)) < -50.0); // its low side is gone
    CHECK_THAT(dB(magnitudeAt(a, 4000.0, sr) / magnitudeAt(b, 4000.0, sr)), WithinAbs(0.0, 0.01));

    // Correlated (mono) content passes flat: the split is power-complementary.
    Signal mono{sine(150.0, 0.5, sr, n), sine(150.0, 0.5, sr, n)};
    const auto o = runChain({stage("BassMonoMaker", {{"crossover_hz", 150.0f}})}, mono, sr, 3000);
    CHECK_THAT(dB(rms(o[0], 22050, 66150) / rms(mono[0], 22050, 66150)), WithinAbs(0.0, 0.01));
}

TEST_CASE("ENH-6: the noise gate closes under its threshold only", "[dsp][render][enh6]")
{
    const double sr = 44100.0;
    const int n = 2 * 44100, half = n / 2;
    Signal in{sine(440.0, 0.02, sr, n, 0, half)}; // -34 dBFS, then loud
    const auto loud = sine(440.0, 0.5, sr, n, half, n);
    for (size_t i = 0; i < in[0].size(); ++i) in[0][i] += loud[i];
    const auto out = runChain(
        {stage("NoiseGate", {{"threshold_db", -20.0f}, {"ratio", 10.0f}, {"attack_ms", 1.0f}, {"release_ms", 50.0f}})},
        in, sr, 512);
    CHECK(dB(rms(out[0], 11025, static_cast<size_t>(half)) / rms(in[0], 11025, static_cast<size_t>(half))) < -40.0);
    CHECK_THAT(dB(rms(out[0], static_cast<size_t>(half) + 4410, static_cast<size_t>(n)) /
                  rms(in[0], static_cast<size_t>(half) + 4410, static_cast<size_t>(n))),
               WithinAbs(0.0, 0.05));
}

TEST_CASE("ENH-6: the exciter adds level above its frequency only", "[dsp][render][enh6]")
{
    const double sr = 44100.0;
    const int n = 2 * 44100;
    Signal in{sine(200.0, 0.4, sr, n)};
    const auto hi = sine(5000.0, 0.1, sr, n);
    for (size_t i = 0; i < in[0].size(); ++i) in[0][i] += hi[i];
    const auto out = runChain({stage("Exciter", {{"frequency_hz", 3000.0f}, {"drive", 4.0f}, {"mix", 0.5f}})}, in, sr, 4096);
    std::vector<double> a(out[0].begin() + 22050, out[0].end() - 22050), b(in[0].begin() + 22050, in[0].end() - 22050);
    CHECK_THAT(dB(magnitudeAt(a, 200.0, sr) / magnitudeAt(b, 200.0, sr)), WithinAbs(0.0, 0.05));
    // The sat path is scaled to the band's RMS: up to +mix at 5 kHz, and
    // harmonics appear at 10 / 15 kHz.
    CHECK(dB(magnitudeAt(a, 5000.0, sr) / magnitudeAt(b, 5000.0, sr)) > 1.0);
    CHECK(magnitudeAt(a, 15000.0, sr) > 10.0 * magnitudeAt(b, 15000.0, sr));
    // mix 0 is the identity, with no latency.
    const auto dry = runChain({stage("Exciter", {{"mix", 0.0f}})}, in, sr, 4096);
    CHECK(dry[0] == in[0]);
}

TEST_CASE("ENH-6: block size never changes the zero-phase stages", "[dsp][render][enh6]")
{
    const double sr = 44100.0;
    const int n = 200000; // past several chunks
    Signal in{std::vector<double>(static_cast<size_t>(n)), std::vector<double>(static_cast<size_t>(n))};
    std::uint32_t s = 7;
    for (auto& c : in)
        for (auto& v : c) {
            s = 1664525u * s + 1013904223u;
            v = 0.3 * ((static_cast<double>(s) / 4294967296.0) * 2.0 - 1.0);
        }
    const Signal key{sine(900.0, 0.4, sr, n, n / 3, n)};
    for (const auto& spec : {stage("DynamicEQ", {{"frequency_hz", 900.0f}, {"threshold_db", -40.0f}}),
                             stage("DynamicEQ", {{"frequency_hz", 40.0f}, {"q", 18.0f}, {"threshold_db", -60.0f}}),
                             stage("MultibandCompressor"), stage("Exciter"), stage("BassMonoMaker")}) {
        DYNAMIC_SECTION(spec.effect << " " << spec.parameters.size())
        {
            const auto a = runChain({spec}, in, sr, 1 << 20);
            const auto b = runChain({spec}, in, sr, 1000);
            double worst = 0.0;
            for (size_t c = 0; c < 2; ++c)
                for (size_t i = 0; i < a[c].size(); ++i) worst = std::max(worst, std::abs(a[c][i] - b[c][i]));
            CHECK(worst < 1e-12);
        }
    }
    SECTION("keyed")
    {
        const auto spec = stage("DynamicEQ", {{"frequency_hz", 900.0f}, {"threshold_db", -40.0f}, {"use_key", 1.0f}});
        const auto a = runChain({spec}, in, sr, 1 << 20, &key);
        const auto b = runChain({stage("Gain", {{"gain_db", 0.0f}}), spec}, in, sr, 1000, &key);
        double worst = 0.0;
        for (size_t c = 0; c < 2; ++c)
            for (size_t i = 0; i < a[c].size(); ++i) worst = std::max(worst, std::abs(a[c][i] - b[c][i]));
        CHECK(worst < 1e-12);
    }
}

TEST_CASE("ENH-6: a chunked stage behind a lookahead stage keeps its key aligned", "[dsp][render][enh6]")
{
    // The limiter in front holds samples back; the key still lines up with
    // the samples the dynamic EQ sees.
    const double sr = 44100.0;
    const int n = 3 * 44100, half = n / 2;
    const Signal bed{sine(1000.0, 0.3, sr, n)};
    const Signal key{sine(1000.0, 0.5, sr, n, half, n)};
    const auto dyn = stage("DynamicEQ", {{"frequency_hz", 1000.0f}, {"threshold_db", -30.0f},
                                         {"max_reduction_db", 9.0f}, {"use_key", 1.0f}});
    const auto a = runChain({dyn}, bed, sr, 4096, &key);
    const auto b = runChain({stage("Limiter", {{"threshold_db", 0.0f}}), dyn}, bed, sr, 4096, &key);
    double worst = 0.0;
    for (size_t i = 0; i < a[0].size(); ++i) worst = std::max(worst, std::abs(a[0][i] - b[0][i]));
    CHECK(worst < 1e-9); // the limiter never engages under 0 dBTP at 0.3
}

TEST_CASE("ENH-6: the file renderer keys a DynamicEQ on a key file", "[dsp][render][enh6]")
{
    const double sr = 44100.0;
    const int n = 3 * 44100, half = n / 2;
    const auto dir = tempDir();
    const auto input = dir.getChildFile("bed.wav"), keyFile = dir.getChildFile("vocal.wav"),
               out = dir.getChildFile("out.wav");
    const Signal bed{sine(1000.0, 0.3, sr, n), sine(1000.0, 0.3, sr, n)};
    writeWav(input, bed, sr);
    writeWav(keyFile, Signal{sine(1000.0, 0.5, sr, n, half, n)}, sr);
    const std::vector<kd::ChainStageSpec> chain{stage(
        "DynamicEQ",
        {{"frequency_hz", 1000.0f}, {"threshold_db", -30.0f}, {"max_reduction_db", 6.0f}, {"use_key", 1.0f}})};

    kd::OfflineRenderOptions opts;
    opts.enforceGuardrail = false;
    opts.keyPath = keyFile.getFullPathName().toStdString();
    auto res = kd::OfflineChainRenderer::renderFile(input, out, chain, opts);
    INFO(res.error);
    REQUIRE(res.status == kd::OfflineRenderResult::Status::Rendered);
    CHECK(res.warnings.empty());
    auto y = readWav(out);
    const size_t q1 = 22050, q2 = static_cast<size_t>(half) - 22050, q3 = static_cast<size_t>(half) + 22050,
                 q4 = static_cast<size_t>(n) - 22050;
    CHECK_THAT(dB(rms(y[0], q1, q2) / rms(bed[0], q1, q2)), WithinAbs(0.0, 0.01));
    CHECK_THAT(dB(rms(y[0], q3, q4) / rms(bed[0], q3, q4)), WithinAbs(-6.0, 0.2));

    // A missing key file: self-keyed, with a warning.
    opts.keyPath = dir.getChildFile("missing.wav").getFullPathName().toStdString();
    res = kd::OfflineChainRenderer::renderFile(input, out, chain, opts);
    REQUIRE(res.status == kd::OfflineRenderResult::Status::Rendered);
    CHECK(res.warnings.size() == 1);
    y = readWav(out);
    CHECK(dB(rms(y[0], q1, q2) / rms(bed[0], q1, q2)) < -3.0);
    out.deleteFile();
}

TEST_CASE("ENH-6: a masking finding renders as a keyed dynamic EQ", "[masking][enh6]")
{
    khensu::AnalysisResult r;
    r.file_id = "f";
    khensu::MaskingSummary m;
    m.bands_hz = {100.0f, 200.0f, 400.0f};
    khensu::MaskingPairSummary pair;
    pair.stem_a = "other";
    pair.stem_b = "vocals";
    pair.collision_score = 0.9f;
    pair.band_scores = {0.2f, 0.9f}; // worst: 200-400 Hz
    m.pairs.push_back(pair);
    r.masking = m;
    const auto recs = khensu::MaskingRecommender{}.recommend(r, {}, {});
    REQUIRE(recs.size() == 1);
    const auto plan = khensu::maskingDynamicEq(recs[0]);
    REQUIRE(plan.has_value());
    CHECK(plan->keyStem == "vocals"); // the vocal leads ...
    CHECK(plan->targetStem == "other"); // ... the bed makes room
    CHECK(plan->stage.effect == "DynamicEQ");
    CHECK_THAT(plan->stage.parameters.at("frequency_hz"), WithinAbs(std::sqrt(200.0 * 400.0), 0.01));
    CHECK_THAT(plan->stage.parameters.at("q"), WithinAbs(1.0, 1e-4)); // one octave
    CHECK(plan->stage.parameters.at("use_key") == 1.0f);
    CHECK_THAT(plan->stage.parameters.at("max_reduction_db"),
               WithinAbs(std::abs(recs[0].suggested_parameters.at("eq_gain_db")), 1e-5));
    // The stage the plan names builds natively.
    std::string error;
    CHECK(kd::makeChainStage(plan->stage, 44100.0, 2, error) != nullptr);

    // Kick and bass: the drums key the bass cut; an unknown stem self-keys.
    std::map<std::string, float> p;
    khensu::writeMaskingDynamicEq(p, "bass", "drums", 60.0f, 120.0f, -3.0f);
    CHECK(khensu::maskingStemName(p.at("dyneq_key_stem")) == "drums");
    CHECK(khensu::maskingStemName(p.at("dyneq_target_stem")) == "bass");
    std::map<std::string, float> q;
    khensu::writeMaskingDynamicEq(q, "synth", "pad", 500.0f, 1000.0f, -3.0f);
    CHECK(q.count("dyneq_key_stem") == 0);
    khensu::Recommendation unknown;
    unknown.suggested_parameters = q;
    CHECK(khensu::maskingDynamicEq(unknown)->stage.parameters.at("use_key") == 0.0f);
}
