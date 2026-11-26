// OVH-9: the native offline chain renderer (shared-core/dsp/OfflineChainRenderer).
//  * parity with the service's chain runner: fixtures written by
//    scripts/gen_render_parity_fixtures.py from copilot/dsp.py (loudness
//    0.05 LU, true peak and octave bands 0.1 dB);
//  * each filter against its analytic magnitude;
//  * the limiter holds its ceiling on inter-sample peaks;
//  * block-wise processing equals one block, the de-esser's chunked
//    zero-phase band equals a whole-signal one, cancellation leaves nothing;
//  * FN2-2a: the denoiser takes the hiss down as much as the service's does,
//    its histogram profile (long files) stays within a hair of the exact one,
//    and the new stages hold back a bounded number of samples.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <khensu/analysis/NativeMeters.h>
#include <khensu/analysis/TruePeakFilter.h>
#include <khensu/dsp/ChainProcessor.h>
#include <khensu/dsp/IirDesign.h>
#include <khensu/dsp/OfflineChainRenderer.h>
#include <khensu/dsp/SpectralDenoiser.h>
#include <khensu/dsp/TapeSaturator.h>
#include <khensu/dsp/TruePeakOversampler.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <iostream>
#include <map>
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

using Catch::Matchers::WithinAbs;
namespace kd = khensu::dsp;
namespace kn = khensu::native;

namespace {

constexpr double kPi = 3.14159265358979323846;

using Planar = std::vector<std::vector<float>>;

juce::File tempDir()
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("khensu-ovh9-render-tests");
    dir.createDirectory();
    return dir;
}

void writeFloatWav(const juce::File& file, const Planar& chans, double sr)
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
    std::vector<const float*> ptrs;
    for (const auto& c : chans) ptrs.push_back(c.data());
    REQUIRE(writer->writeFromFloatArrays(ptrs.data(), static_cast<int>(ptrs.size()),
                                         static_cast<int>(chans.front().size())));
}

Planar readWav(const juce::File& file, double* sr = nullptr)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    REQUIRE(reader != nullptr);
    if (sr != nullptr) *sr = reader->sampleRate;
    const int n = static_cast<int>(reader->lengthInSamples);
    juce::AudioBuffer<float> buf(static_cast<int>(reader->numChannels), n);
    REQUIRE(reader->read(&buf, 0, n, 0, true, true));
    Planar out(static_cast<size_t>(buf.getNumChannels()));
    for (int c = 0; c < buf.getNumChannels(); ++c) out[static_cast<size_t>(c)].assign(buf.getReadPointer(c), buf.getReadPointer(c) + n);
    return out;
}

/// Mirrors synthesize() in scripts/gen_native_meter_fixtures.py (the recipes
/// render_parity.json reuses).
Planar synthesize(const juce::var& spec)
{
    const double sr = static_cast<double>(spec["sample_rate"]);
    const auto n = static_cast<std::int64_t>(std::nearbyint(static_cast<double>(spec["duration_sec"]) * sr));
    Planar out;
    const auto* chans = spec["channels"].getArray();
    REQUIRE(chans != nullptr);
    for (const auto& ch : *chans) {
        std::vector<double> y(static_cast<size_t>(n), 0.0);
        if (const auto* tones = ch["tones"].getArray())
            for (const auto& tone : *tones) {
                const double f = tone["freq"], a = tone["amp"];
                const double ph = tone.hasProperty("phase") ? static_cast<double>(tone["phase"]) : 0.0;
                for (std::int64_t i = 0; i < n; ++i)
                    y[static_cast<size_t>(i)] += a * std::sin(2.0 * kPi * f * static_cast<double>(i) / sr + ph);
            }
        if (ch.hasProperty("noise_seed")) {
            const double amp = ch.hasProperty("noise_amp") ? static_cast<double>(ch["noise_amp"]) : 1.0;
            auto x = static_cast<std::uint32_t>(static_cast<juce::int64>(ch["noise_seed"]));
            for (std::int64_t i = 0; i < n; ++i) {
                x = 1664525u * x + 1013904223u;
                y[static_cast<size_t>(i)] += amp * ((static_cast<double>(x) / 4294967296.0) * 2.0 - 1.0);
            }
        }
        if (const auto* steps = ch["gain_steps"].getArray()) {
            std::vector<double> g(static_cast<size_t>(n), 1.0);
            for (const auto& step : *steps) {
                const auto start = static_cast<std::int64_t>(std::nearbyint(static_cast<double>(step[0]) * sr));
                for (std::int64_t i = std::max<std::int64_t>(0, start); i < n; ++i)
                    g[static_cast<size_t>(i)] = static_cast<double>(step[1]);
            }
            for (size_t i = 0; i < y.size(); ++i) y[i] *= g[i];
        }
        std::vector<float> f(y.size());
        for (size_t i = 0; i < y.size(); ++i) f[i] = static_cast<float>(y[i]);
        out.push_back(std::move(f));
    }
    return out;
}

std::vector<kd::ChainStageSpec> chainFrom(const juce::var& chain)
{
    std::vector<kd::ChainStageSpec> out;
    const auto* arr = chain.getArray();
    REQUIRE(arr != nullptr);
    for (const auto& e : *arr) {
        kd::ChainStageSpec s;
        s.effect = e["effect"].toString().toStdString();
        if (auto* params = e["parameters"].getDynamicObject())
            for (const auto& p : params->getProperties())
                s.parameters[p.name.toString().toStdString()] = static_cast<float>(static_cast<double>(p.value));
        out.push_back(std::move(s));
    }
    return out;
}

kd::ChainStageSpec stage(const char* effect, std::map<std::string, float> params = {})
{
    return {effect, std::move(params)};
}

/// `in` cut into blocks of `blockSize`.
void forEachBlock(const std::vector<std::vector<double>>& in, int blockSize,
                  const std::function<void(kd::PlanarBlock&&)>& consume)
{
    const auto n = in.front().size();
    for (size_t pos = 0; pos < n; pos += static_cast<size_t>(blockSize)) {
        const size_t len = std::min(static_cast<size_t>(blockSize), n - pos);
        kd::PlanarBlock b;
        for (const auto& c : in)
            b.ch.emplace_back(c.begin() + static_cast<std::ptrdiff_t>(pos),
                              c.begin() + static_cast<std::ptrdiff_t>(pos + len));
        consume(std::move(b));
    }
}

/// Runs `chain` over `in` in blocks of `blockSize` through ChainProcessor
/// (with its profile passes, as the file renderer runs them).
std::vector<std::vector<double>> runChain(const std::vector<kd::ChainStageSpec>& chain,
                                          const std::vector<std::vector<double>>& in, double sr, int blockSize)
{
    kd::ChainProcessor proc;
    std::string error;
    REQUIRE(proc.prepare(chain, sr, static_cast<int>(in.size()), error));
    if (proc.needsProfilePass()) {
        const auto replay = [&in, blockSize](const std::function<void(kd::PlanarBlock&&)>& consume) {
            forEachBlock(in, blockSize, consume);
            return true;
        };
        REQUIRE(proc.runProfilePasses(replay, error));
    }
    std::vector<std::vector<double>> out(in.size());
    const auto sink = [&out](const kd::PlanarBlock& b) {
        for (size_t c = 0; c < b.ch.size(); ++c) out[c].insert(out[c].end(), b.ch[c].begin(), b.ch[c].end());
    };
    forEachBlock(in, blockSize, [&](kd::PlanarBlock&& b) { proc.process(std::move(b), sink); });
    proc.finish(sink);
    return out;
}

double dB(double linear)
{
    return 20.0 * std::log10(linear);
}

/// |H| of a stage measured from its impulse response by a direct DFT.
double measuredMagnitude(const std::vector<double>& h, double f, double sr)
{
    std::complex<double> acc = 0.0;
    for (size_t i = 0; i < h.size(); ++i)
        acc += h[i] * std::polar(1.0, -2.0 * kPi * f * static_cast<double>(i) / sr);
    return std::abs(acc);
}

std::vector<double> impulseResponse(const kd::ChainStageSpec& spec, double sr, int n = 16384)
{
    std::vector<std::vector<double>> in(1, std::vector<double>(static_cast<size_t>(n), 0.0));
    in[0][0] = 1.0;
    return runChain({spec}, in, sr, n)[0];
}

/// RBJ cookbook magnitudes, written from the cookbook (not from IirDesign).
double rbjMagnitude(const char* type, double f0, double q, double gainDb, double f, double sr)
{
    const double A = std::pow(10.0, gainDb / 40.0), w0 = 2.0 * kPi * f0 / sr;
    const double cw = std::cos(w0), alpha = std::sin(w0) / (2.0 * q);
    double b0, b1, b2, a0, a1, a2;
    const std::string t(type);
    if (t == "peak") {
        b0 = 1 + alpha * A, b1 = -2 * cw, b2 = 1 - alpha * A, a0 = 1 + alpha / A, a1 = -2 * cw, a2 = 1 - alpha / A;
    } else if (t == "low") {
        const double s = 2 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) - (A - 1) * cw + s), b1 = 2 * A * ((A - 1) - (A + 1) * cw), b2 = A * ((A + 1) - (A - 1) * cw - s);
        a0 = (A + 1) + (A - 1) * cw + s, a1 = -2 * ((A - 1) + (A + 1) * cw), a2 = (A + 1) + (A - 1) * cw - s;
    } else {
        const double s = 2 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) + (A - 1) * cw + s), b1 = -2 * A * ((A - 1) + (A + 1) * cw), b2 = A * ((A + 1) + (A - 1) * cw - s);
        a0 = (A + 1) - (A - 1) * cw + s, a1 = 2 * ((A - 1) - (A + 1) * cw), a2 = (A + 1) - (A - 1) * cw - s;
    }
    const auto z1 = std::polar(1.0, -2.0 * kPi * f / sr), z2 = z1 * z1;
    return std::abs((b0 + b1 * z1 + b2 * z2) / (a0 + a1 * z1 + a2 * z2));
}

} // namespace

TEST_CASE("OVH-9: native chain renders match the service's chain runner", "[dsp][render][parity]")
{
    const auto file = juce::File(KHENSU_REPO_ROOT_DIR)
                          .getChildFile("tests/shared-core/fixtures/render_parity.json");
    REQUIRE(file.existsAsFile());
    const auto fixtures = juce::JSON::parse(file);
    const double lufsTol = fixtures["loudness_tolerance"];
    const double bandTol = fixtures["band_tolerance_db"];
    const double tpTol = fixtures["true_peak_tolerance_db"];
    const auto* signals = fixtures["signals"].getArray();
    const auto* cases = fixtures["cases"].getArray();
    REQUIRE(signals != nullptr);
    REQUIRE(cases != nullptr);
    REQUIRE(cases->size() >= 77); // FN2-2a: + denoise, transient shaper, saturation, widener; ENH-6: + 7 stages, shaped + keyed DynamicEQ

    const auto dir = tempDir();
    std::map<juce::String, juce::File> inputs;
    double worstLufs = 0.0, worstTp = 0.0, worstBand = 0.0; // printed for the record
    for (const auto& spec : *signals) {
        const auto f = dir.getChildFile(spec["name"].toString() + ".wav");
        writeFloatWav(f, synthesize(spec), spec["sample_rate"]);
        inputs[spec["name"].toString()] = f;
    }
    // ENH-6: the key signals the keyed DynamicEQ cases read (OfflineRenderOptions::keyPath).
    if (const auto* keySignals = fixtures["key_signals"].getArray()) {
        for (const auto& spec : *keySignals) {
            const auto f = dir.getChildFile(spec["name"].toString() + ".wav");
            writeFloatWav(f, synthesize(spec), spec["sample_rate"]);
            inputs[spec["name"].toString()] = f;
        }
    }

    for (const auto& c : *cases) {
        const auto name = c["signal"].toString() + " / " + c["chain_name"].toString();
        DYNAMIC_SECTION(name.toStdString())
        {
            const auto out = dir.getChildFile("parity_out.wav");
            kd::OfflineRenderOptions opts;
            opts.enforceGuardrail = false; // a refusal is the service's too; compare the audio
            if (c.hasProperty("key_signal"))
                opts.keyPath = inputs[c["key_signal"].toString()].getFullPathName().toStdString();
            const auto res =
                kd::OfflineChainRenderer::renderFile(inputs[c["signal"].toString()], out, chainFrom(c["chain"]), opts);
            INFO(res.error);
            REQUIRE(res.status == kd::OfflineRenderResult::Status::Rendered);
            double sr = 0.0;
            const auto y = readWav(out, &sr);
            std::vector<const float*> ptrs;
            for (const auto& ch : y) ptrs.push_back(ch.data());
            const auto r = kn::measureAll(ptrs.data(), static_cast<int>(ptrs.size()),
                                          static_cast<std::int64_t>(y.front().size()), sr);
            const auto& e = c["expected"];
            worstLufs = std::max(worstLufs, std::abs(r.loudness.integratedLufs - static_cast<double>(e["integrated_lufs"])));
            worstTp = std::max(worstTp, std::abs(r.loudness.truePeakDbtp - static_cast<double>(e["true_peak_dbtp"])));
            INFO("lufs native " << r.loudness.integratedLufs << " vs " << e["integrated_lufs"].toString().toStdString());
            CHECK_THAT(r.loudness.integratedLufs, WithinAbs(static_cast<double>(e["integrated_lufs"]), lufsTol));
            INFO("tp native " << r.loudness.truePeakDbtp << " vs " << e["true_peak_dbtp"].toString().toStdString());
            CHECK_THAT(r.loudness.truePeakDbtp, WithinAbs(static_cast<double>(e["true_peak_dbtp"]), tpTol));
            const auto* levels = e["octave_levels_db"].getArray();
            REQUIRE(levels != nullptr);
            REQUIRE(r.spectrum.levelsDb.size() == static_cast<size_t>(levels->size()));
            for (int i = 0; i < levels->size(); ++i) {
                const double want = (*levels)[i];
                INFO("octave " << r.spectrum.centersHz[static_cast<size_t>(i)] << " Hz: native "
                               << r.spectrum.levelsDb[static_cast<size_t>(i)] << " vs " << want);
                // Bands with no real content (FFT round-off) compare loosely.
                if (want >= -120.0)
                    worstBand = std::max(worstBand, std::abs(r.spectrum.levelsDb[static_cast<size_t>(i)] - want));
                CHECK_THAT(r.spectrum.levelsDb[static_cast<size_t>(i)], WithinAbs(want, want < -120.0 ? 10.0 : bandTol));
            }
            out.deleteFile();
        }
    }
    std::cout << "OVH-9 render parity, worst case: " << worstLufs << " LU integrated, " << worstTp
              << " dB true peak, " << worstBand << " dB octave band\n";
}

TEST_CASE("OVH-9: shelf, peak and notch biquads have the RBJ magnitude", "[dsp][render]")
{
    const double sr = 44100.0;
    struct Case {
        const char* effect;
        const char* rbj;
        float f, gain, q;
    };
    for (const auto& c : {Case{"LowShelfFilter", "low", 120.0f, 6.0f, 0.707f},
                          Case{"HighShelfFilter", "high", 4000.0f, -4.0f, 0.707f},
                          Case{"PeakFilter", "peak", 1000.0f, 6.0f, 1.0f},
                          Case{"PeakFilter", "peak", 3150.0f, -9.0f, 6.0f}}) {
        const auto h = impulseResponse(stage(c.effect, {{"cutoff_frequency_hz", c.f}, {"gain_db", c.gain}, {"q", c.q}}), sr);
        for (const double f : {30.0, 100.0, 120.0, 500.0, 1000.0, 3150.0, 4000.0, 10000.0}) {
            INFO(c.effect << " at " << f << " Hz");
            CHECK_THAT(dB(measuredMagnitude(h, f, sr)), WithinAbs(dB(rbjMagnitude(c.rbj, c.f, c.q, c.gain, f, sr)), 1e-3));
        }
    }
    // The shelves reach their gain at the far end; the peak's centre is its gain.
    CHECK_THAT(dB(kd::BiquadCascade{}.magnitudeAt(1.0, sr)), WithinAbs(0.0, 1e-9));
    const auto low = impulseResponse(stage("LowShelfFilter", {{"cutoff_frequency_hz", 120.0f}, {"gain_db", 6.0f}}), sr);
    CHECK_THAT(dB(measuredMagnitude(low, 5.0, sr)), WithinAbs(6.0, 0.05));
    const auto notch = impulseResponse(stage("PeakFilter", {{"cutoff_frequency_hz", 3150.0f}, {"gain_db", -9.0f}, {"q", 6.0f}}), sr);
    CHECK_THAT(dB(measuredMagnitude(notch, 3150.0, sr)), WithinAbs(-9.0, 1e-3));
}

TEST_CASE("OVH-9: Butterworth stages have the bilinear Butterworth magnitude", "[dsp][render]")
{
    const double sr = 48000.0;
    // Digital Butterworth via the pre-warped bilinear transform:
    // |H(f)|^2 = 1 / (1 + (tan(pi f / fs) / tan(pi fc / fs))^(+-2N)).
    const auto ratio = [sr](double f, double fc) { return std::tan(kPi * f / sr) / std::tan(kPi * fc / sr); };
    for (const int slope : {6, 12, 18, 24, 48}) {
        const int order = slope / 6;
        const float fc = 100.0f;
        const auto h = impulseResponse(stage("HighpassFilter", {{"cutoff_frequency_hz", fc}, {"slope_db_per_oct", float(slope)}}), sr, 65536);
        for (const double f : {25.0, 50.0, 100.0, 200.0, 1000.0}) {
            const double want = -10.0 * std::log10(1.0 + std::pow(ratio(f, fc), -2.0 * order));
            INFO("HPF order " << order << " at " << f << " Hz");
            CHECK_THAT(dB(measuredMagnitude(h, f, sr)), WithinAbs(want, 2e-3));
        }
    }
    const auto lp = impulseResponse(stage("LowpassFilter", {{"cutoff_frequency_hz", 5000.0f}}), sr);
    for (const double f : {1000.0, 5000.0, 10000.0})
        CHECK_THAT(dB(measuredMagnitude(lp, f, sr)), WithinAbs(-10.0 * std::log10(1.0 + std::pow(ratio(f, 5000.0), 4.0)), 2e-3));
    // Band-pass (the de-esser's split): 0 dB at the warped centre, -3 dB at both edges.
    kd::BiquadCascade bp;
    bp.setSections(kd::butterworth(kd::ButterworthType::Bandpass, 4, sr, 4375.0, 11200.0), 1);
    const double centre = sr / kPi * std::atan(std::sqrt(std::tan(kPi * 4375.0 / sr) * std::tan(kPi * 11200.0 / sr)));
    CHECK_THAT(dB(bp.magnitudeAt(centre, sr)), WithinAbs(0.0, 1e-6));
    CHECK_THAT(dB(bp.magnitudeAt(4375.0, sr)), WithinAbs(-3.0103, 1e-3));
    CHECK_THAT(dB(bp.magnitudeAt(11200.0, sr)), WithinAbs(-3.0103, 1e-3));
}

TEST_CASE("OVH-9: the 4x oversampler is the meters' true-peak filter", "[dsp][render]")
{
    const auto& mine = kd::oversamplerPhases(4);
    const auto& meters = khensu::analysis::truePeakPhases();
    for (int p = 0; p < 4; ++p)
        for (int j = 0; j < kd::kOversamplePhaseTaps; ++j)
            CHECK_THAT(mine.w[static_cast<size_t>(p * kd::kOversamplePhaseTaps + j)], WithinAbs(meters.w[p][j], 1e-15));
}

TEST_CASE("OVH-9: the limiter holds its true-peak ceiling on inter-sample peaks", "[dsp][render]")
{
    // fs/4 at 45 degrees: every sample sits 3 dB under the true peak.
    const double sr = 44100.0;
    const int n = 44100;
    std::vector<std::vector<double>> in(2, std::vector<double>(static_cast<size_t>(n)));
    for (int i = 0; i < n; ++i)
        in[0][static_cast<size_t>(i)] = in[1][static_cast<size_t>(i)] = 0.5 * std::sin(kPi / 2.0 * i + kPi / 4.0);
    const auto y = runChain({stage("Gain", {{"gain_db", 6.0f}}), stage("Limiter", {{"threshold_db", -1.0f}})}, in, sr, 4096);
    REQUIRE(y[0].size() == static_cast<size_t>(n));
    std::vector<float> f(y[0].size());
    for (size_t i = 0; i < f.size(); ++i) f[i] = static_cast<float>(y[0][i]);
    const double tp4 = dB(kn::truePeakLinear(f.data(), n));
    kd::StreamingTruePeak tp8;
    tp8.prepare(1, 8);
    const double* ptr = y[0].data();
    tp8.push(&ptr, 1, n);
    const double tp8db = dB(tp8.finishLinear());
    INFO("4x " << tp4 << " dBTP, 8x " << tp8db << " dBTP");
    CHECK(tp4 <= -1.0 + 0.01);
    CHECK(tp8db <= -1.0 + 0.01);
    CHECK(tp8db > -1.5); // limited, not crushed

    // Through the file renderer the residual trim lands it under the margin.
    const auto dir = tempDir();
    const auto src = dir.getChildFile("isp_src.wav"), out = dir.getChildFile("isp_out.wav");
    Planar fin(2, std::vector<float>(static_cast<size_t>(n)));
    for (size_t c = 0; c < 2; ++c)
        for (size_t i = 0; i < fin[c].size(); ++i) fin[c][i] = static_cast<float>(in[c][i]);
    writeFloatWav(src, fin, sr);
    const auto res = kd::OfflineChainRenderer::renderFile(
        src, out, {stage("Gain", {{"gain_db", 6.0f}}), stage("Limiter", {{"threshold_db", -1.0f}})});
    REQUIRE(res.status == kd::OfflineRenderResult::Status::Rendered);
    const auto yy = readWav(out);
    CHECK(dB(kn::truePeakLinear(yy[0].data(), static_cast<std::int64_t>(yy[0].size()))) <= -1.05 + 1e-3);
    CHECK(res.after.truePeakDbtp <= -1.05 + 1e-3);
    src.deleteFile();
    out.deleteFile();
}

TEST_CASE("OVH-9: block size never changes the render", "[dsp][render]")
{
    const double sr = 44100.0;
    const int n = 3 * 65536 + 12345; // spans several de-esser chunks
    std::vector<std::vector<double>> in(2, std::vector<double>(static_cast<size_t>(n)));
    std::uint32_t x = 7;
    for (int i = 0; i < n; ++i) {
        x = 1664525u * x + 1013904223u;
        const double noise = (static_cast<double>(x) / 4294967296.0) * 2.0 - 1.0;
        const double env = (i / 11025) % 2 == 0 ? 0.9 : 0.1;
        in[0][static_cast<size_t>(i)] = env * (0.3 * noise + 0.5 * std::sin(2 * kPi * 80.0 * i / sr));
        in[1][static_cast<size_t>(i)] = env * (0.3 * noise + 0.4 * std::sin(2 * kPi * 6500.0 * i / sr));
    }
    const std::vector<kd::ChainStageSpec> chain = {
        stage("HighpassFilter", {{"cutoff_frequency_hz", 40.0f}}),
        stage("LowShelfFilter", {{"cutoff_frequency_hz", 120.0f}, {"gain_db", -3.0f}}),
        stage("SpectralDenoise", {{"reduction_db", 12.0f}}), // FN2-2a: profiled through the two above
        stage("DeEsser", {{"threshold_db", -30.0f}, {"frequency_hz", 6500.0f}}),
        stage("TransientShaper", {{"attack_gain_db", 4.0f}, {"sustain_gain_db", -2.0f}}),
        stage("Compressor", {{"threshold_db", -18.0f}, {"ratio", 3.0f}}),
        stage("TapeSaturation", {{"drive_db", 8.0f}, {"mix", 0.7f}}),
        stage("StereoWidener", {{"width", 1.3f}}),
        stage("Gain", {{"gain_db", 6.0f}}),
        stage("Limiter", {{"threshold_db", -1.0f}}),
    };
    const auto oneShot = runChain(chain, in, sr, n);
    for (const int block : {777, 4096, 100000}) {
        const auto blocked = runChain(chain, in, sr, block);
        REQUIRE(blocked[0].size() == static_cast<size_t>(n));
        double maxDiff = 0.0;
        for (size_t c = 0; c < 2; ++c)
            for (size_t i = 0; i < blocked[c].size(); ++i) maxDiff = std::max(maxDiff, std::abs(blocked[c][i] - oneShot[c][i]));
        INFO("block " << block);
        CHECK(maxDiff == 0.0);
    }
}

TEST_CASE("OVH-9: the de-esser's chunked zero-phase band equals a whole-signal one", "[dsp][render]")
{
    const double sr = 48000.0;
    const int n = 2 * 65536 + 5000;
    const int margin = 8192;
    std::vector<double> x(static_cast<size_t>(n));
    std::uint32_t s = 3;
    for (int i = 0; i < n; ++i) {
        s = 1664525u * s + 1013904223u;
        x[static_cast<size_t>(i)] = ((i / 4800) % 3 == 0 ? 0.8 : 0.05) * ((static_cast<double>(s) / 4294967296.0) * 2.0 - 1.0);
    }
    const auto y = runChain({stage("DeEsser", {{"threshold_db", -40.0f}, {"ratio", 6.0f}, {"frequency_hz", 7000.0f}})},
                            {x}, sr, 10000)[0];
    // Reference: the band over the whole signal at once (same odd extension).
    std::vector<double> ext;
    for (int k = margin; k >= 1; --k) ext.push_back(2.0 * x[0] - x[static_cast<size_t>(k)]);
    ext.insert(ext.end(), x.begin(), x.end());
    for (int k = 1; k <= margin; ++k) ext.push_back(2.0 * x.back() - x[static_cast<size_t>(n - 1 - k)]);
    const auto sections = kd::butterworth(kd::ButterworthType::Bandpass, 4, sr, 7000.0 / 1.6, 7000.0 * 1.6);
    kd::BiquadCascade bp;
    bp.setSections(sections, 1);
    bp.process(0, ext.data(), static_cast<int>(ext.size()));
    std::reverse(ext.begin(), ext.end());
    bp.reset();
    bp.process(0, ext.data(), static_cast<int>(ext.size()));
    std::reverse(ext.begin(), ext.end());
    const double a = std::exp(-1.0 / (0.008 * sr)), thr = std::pow(10.0, -40.0 / 20.0);
    double env = 0.0, maxDiff = 0.0;
    for (int i = 0; i < n; ++i) {
        const double band = ext[static_cast<size_t>(margin + i)];
        env = (1.0 - a) * std::abs(band) + a * env;
        const double over = env / thr;
        const double want = (x[static_cast<size_t>(i)] - band) + band * (over > 1.0 ? std::pow(over, 1.0 / 6.0 - 1.0) : 1.0);
        maxDiff = std::max(maxDiff, std::abs(want - y[static_cast<size_t>(i)]));
    }
    CHECK(maxDiff < 1e-9);
}

TEST_CASE("OVH-9: a cancelled or engine-only render leaves no file", "[dsp][render]")
{
    const auto dir = tempDir();
    const auto src = dir.getChildFile("cancel_src.wav"), out = dir.getChildFile("cancel_out.wav");
    Planar sig(2, std::vector<float>(200000));
    for (size_t i = 0; i < sig[0].size(); ++i) sig[0][i] = sig[1][i] = 0.3f * std::sin(0.01f * static_cast<float>(i));
    writeFloatWav(src, sig, 44100.0);
    out.deleteFile();

    int polls = 0;
    kd::OfflineRenderOptions opts;
    opts.blockSize = 4096;
    opts.shouldCancel = [&polls] { return ++polls > 3; };
    const auto cancelled = kd::OfflineChainRenderer::renderFile(src, out, {stage("Gain", {{"gain_db", -3.0f}})}, opts);
    CHECK(cancelled.status == kd::OfflineRenderResult::Status::Cancelled);
    CHECK_FALSE(out.existsAsFile());
    CHECK(dir.findChildFiles(juce::File::findFiles, false, "*.tmp.wav").isEmpty());

    const auto engine = kd::OfflineChainRenderer::renderFile(
        src, out, {stage("LowShelfFilter", {{"gain_db", -3.0f}}), stage("Reverb")});
    CHECK(engine.status == kd::OfflineRenderResult::Status::NeedsEngine);
    REQUIRE(engine.needsEngine == std::vector<std::string>{"Reverb"});
    CHECK_FALSE(out.existsAsFile());
    CHECK_FALSE(kd::OfflineChainRenderer::canRender({stage("Reverb"), stage("Gain")}));
    CHECK(kd::OfflineChainRenderer::canRender({stage("HighpassFilter"), stage("Limiter")}));
    // FN2-2a: the four that used to need the engine.
    CHECK(kd::OfflineChainRenderer::canRender({stage("SpectralDenoise"), stage("TransientShaper"),
                                               stage("TapeSaturation"), stage("StereoWidener")}));
    CHECK_FALSE(kd::OfflineChainRenderer::canRender({}));

    // Cancelled during the denoiser's profile pass: nothing is left either.
    polls = 0;
    const auto profiling = kd::OfflineChainRenderer::renderFile(src, out, {stage("SpectralDenoise")}, opts);
    CHECK(profiling.status == kd::OfflineRenderResult::Status::Cancelled);
    CHECK_FALSE(out.existsAsFile());
    CHECK(dir.findChildFiles(juce::File::findFiles, false, "*.tmp.wav").isEmpty());

    // A plain render keeps length and alignment: Gain only is x * g exactly.
    const auto ok = kd::OfflineChainRenderer::renderFile(src, out, {stage("Gain", {{"gain_db", -6.0f}})});
    REQUIRE(ok.status == kd::OfflineRenderResult::Status::Rendered);
    const auto y = readWav(out);
    REQUIRE(y[0].size() == sig[0].size());
    const float g = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
    double maxDiff = 0.0;
    for (size_t i = 0; i < y[0].size(); ++i) maxDiff = std::max(maxDiff, static_cast<double>(std::abs(y[0][i] - sig[0][i] * g)));
    CHECK(maxDiff < 1e-6);
    CHECK_THAT(ok.matchedGainDb, WithinAbs(6.0, 0.02));
    src.deleteFile();
    out.deleteFile();
}

namespace {

/// Mean octave-band level from `fromHz` up, as the fixture's floor_db.
double floorDb(const Planar& y, double sr, double fromHz)
{
    std::vector<const float*> ptrs;
    for (const auto& ch : y) ptrs.push_back(ch.data());
    const auto r = kn::measureAll(ptrs.data(), static_cast<int>(ptrs.size()),
                                  static_cast<std::int64_t>(y.front().size()), sr);
    double sum = 0.0;
    int count = 0;
    for (size_t i = 0; i < r.spectrum.centersHz.size(); ++i)
        if (r.spectrum.centersHz[i] >= fromHz) {
            sum += r.spectrum.levelsDb[i];
            ++count;
        }
    REQUIRE(count > 0);
    return sum / count;
}

} // namespace

TEST_CASE("FN2-2a: the native denoiser takes the hiss down as far as the service's", "[dsp][render][parity]")
{
    const auto fixtures = juce::JSON::parse(
        juce::File(KHENSU_REPO_ROOT_DIR).getChildFile("tests/shared-core/fixtures/render_parity.json"));
    const auto& nf = fixtures["noise_floor"];
    REQUIRE(nf.isObject());
    const double tol = fixtures["noise_floor_tolerance_db"];
    const double fromHz = nf["from_hz"];
    juce::var spec, chain;
    for (const auto& s : *fixtures["signals"].getArray())
        if (s["name"] == nf["signal"]) spec = s;
    for (const auto& c : *fixtures["cases"].getArray())
        if (c["signal"] == nf["signal"] && c["chain_name"] == nf["chain_name"]) chain = c["chain"];
    REQUIRE(spec.isObject());
    REQUIRE(chain.isArray());

    const double sr = spec["sample_rate"];
    const auto x = synthesize(spec);
    const auto dir = tempDir();
    const auto src = dir.getChildFile("hiss_src.wav"), out = dir.getChildFile("hiss_out.wav");
    writeFloatWav(src, x, sr);
    const auto res = kd::OfflineChainRenderer::renderFile(src, out, chainFrom(chain));
    INFO(res.error);
    REQUIRE(res.status == kd::OfflineRenderResult::Status::Rendered);
    const double inFloor = floorDb(x, sr, fromHz), outFloor = floorDb(readWav(out), sr, fromHz);
    const double serviceReduction =
        static_cast<double>(nf["input_floor_db"]) - static_cast<double>(nf["output_floor_db"]);
    std::cout << "FN2-2a denoise: hiss floor " << inFloor << " -> " << outFloor << " dB natively (reduction "
              << inFloor - outFloor << " dB; the service's " << serviceReduction << " dB)\n";
    CHECK_THAT(inFloor, WithinAbs(static_cast<double>(nf["input_floor_db"]), tol));
    CHECK_THAT(outFloor, WithinAbs(static_cast<double>(nf["output_floor_db"]), tol));
    CHECK_THAT(inFloor - outFloor, WithinAbs(serviceReduction, tol));
    CHECK(inFloor - outFloor > 4.0);
    src.deleteFile();
    out.deleteFile();
}

TEST_CASE("FN2-2a: the denoiser's histogram profile stays within a hair of the exact one", "[dsp][render]")
{
    // Exponential (noise-like) powers over 70 dB of bin levels.
    const int columns = 64, rows = 3000;
    std::mt19937 rng(9);
    std::exponential_distribution<double> expo(1.0);
    kd::PercentileProfile exact(columns, rows), folded(columns, 100);
    std::vector<double> row(static_cast<size_t>(columns));
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < columns; ++c)
            row[static_cast<size_t>(c)] = expo(rng) * std::pow(10.0, -7.0 * c / columns);
        exact.add(row.data());
        folded.add(row.data());
    }
    CHECK(exact.exact());
    CHECK_FALSE(folded.exact()); // switched to the bounded histogram after 100 rows
    for (const double p : {10.0, 50.0, 90.0}) {
        const auto a = exact.percentile(p), b = folded.percentile(p);
        double worst = 0.0;
        for (int c = 0; c < columns; ++c)
            worst = std::max(worst, std::abs(10.0 * std::log10(b[static_cast<size_t>(c)] / a[static_cast<size_t>(c)])));
        INFO("p" << p << " worst " << worst << " dB");
        CHECK(worst < 0.05);
    }
    // numpy's linear interpolation on a tiny exact case: [1, 2, 3, 4] -> p10 = 1.3.
    kd::PercentileProfile tiny(1, 10);
    for (const double v : {4.0, 1.0, 3.0, 2.0}) tiny.add(&v);
    CHECK_THAT(tiny.percentile(10.0)[0], WithinAbs(1.3, 1e-12));

    // A whole render with the folded profile is within a hair of the exact one.
    const double sr = 44100.0;
    const int n = 20 * 44100;
    std::vector<std::vector<double>> in(1, std::vector<double>(static_cast<size_t>(n)));
    std::uniform_real_distribution<double> uni(-1.0, 1.0);
    for (int i = 0; i < n; ++i)
        in[0][static_cast<size_t>(i)] =
            0.004 * uni(rng) + ((i / 22050) % 2 == 0 ? 0.3 : 0.05) * std::sin(2.0 * kPi * 440.0 * i / sr);
    const auto render = [&](int exactFrames) {
        auto st = kd::makeSpectralDenoiseStage(12.0, 3.0, 1, exactFrames);
        REQUIRE(st->needsProfile());
        forEachBlock(in, 8192, [&](kd::PlanarBlock&& b) { st->profile(b); });
        st->endProfile();
        std::vector<double> y;
        forEachBlock(in, 8192, [&](kd::PlanarBlock&& b) {
            st->push(b);
            y.insert(y.end(), b.ch[0].begin(), b.ch[0].end());
        });
        kd::PlanarBlock tail;
        st->finish(tail);
        y.insert(y.end(), tail.ch[0].begin(), tail.ch[0].end());
        return y;
    };
    const auto yExact = render(kd::denoise::kExactProfileFrames), yFolded = render(50);
    REQUIRE(yExact.size() == in[0].size());
    REQUIRE(yFolded.size() == in[0].size());
    double e = 0.0, d = 0.0;
    for (size_t i = 0; i < yExact.size(); ++i) {
        e += yExact[i] * yExact[i];
        d += (yFolded[i] - yExact[i]) * (yFolded[i] - yExact[i]);
    }
    const double diffDb = 10.0 * std::log10(d / e + 1e-30);
    INFO("difference " << diffDb << " dB under the output");
    CHECK(diffDb < -40.0);
}

TEST_CASE("FN2-2a: the new stages hold back a bounded number of samples", "[dsp][render]")
{
    // Streamed over a minute, each stage's output never trails its input by
    // more than its look-ahead: a long file is never buffered whole.
    const double sr = 48000.0;
    const int n = 60 * 48000, block = 4096;
    std::vector<std::vector<double>> in(2, std::vector<double>(static_cast<size_t>(n)));
    std::uint32_t x = 5;
    for (int i = 0; i < n; ++i) {
        x = 1664525u * x + 1013904223u;
        const double noise = (static_cast<double>(x) / 4294967296.0) * 2.0 - 1.0;
        in[0][static_cast<size_t>(i)] = 0.2 * noise;
        in[1][static_cast<size_t>(i)] = 0.1 * noise + 0.3 * std::sin(2.0 * kPi * 100.0 * i / sr);
    }
    struct Bound {
        const char* effect;
        long long maxLag;
    };
    for (const auto& b : {Bound{"SpectralDenoise", kd::denoise::kFftSize + kd::denoise::kHop + block},
                          Bound{"TapeSaturation", 64 + block}, Bound{"TransientShaper", 0},
                          Bound{"StereoWidener", 0}}) {
        std::string error;
        auto st = kd::makeChainStage(stage(b.effect), sr, 2, error);
        REQUIRE(st != nullptr);
        if (st->needsProfile()) {
            forEachBlock(in, block, [&](kd::PlanarBlock&& blk) { st->profile(blk); });
            st->endProfile();
        }
        long long received = 0, emitted = 0, worstLag = 0;
        forEachBlock(in, block, [&](kd::PlanarBlock&& blk) {
            received += blk.numSamples();
            st->push(blk);
            emitted += blk.numSamples();
            worstLag = std::max(worstLag, received - emitted);
        });
        kd::PlanarBlock tail;
        st->finish(tail);
        INFO(b.effect << " worst lag " << worstLag);
        CHECK(worstLag <= b.maxLag);
        CHECK(emitted + tail.numSamples() == n);
    }
}

TEST_CASE("FN2-2a: firwin's Kaiser low-pass is scipy's", "[dsp][render]")
{
    // scipy.signal.firwin(81, 0.25, window=("kaiser", 5.0)): symmetric,
    // unity gain at DC, zeros where the sinc is (every 4th tap off centre).
    const auto h = kd::firwinKaiser(81, 0.25, 5.0);
    REQUIRE(h.size() == 81);
    double sum = 0.0;
    for (const double v : h) sum += v;
    CHECK_THAT(sum, WithinAbs(1.0, 1e-12));
    for (size_t i = 0; i < 40; ++i) CHECK_THAT(h[i], WithinAbs(h[80 - i], 1e-15));
    CHECK_THAT(h[36], WithinAbs(0.0, 1e-15));
    CHECK_THAT(h[40], WithinAbs(0.25015914127227795, 1e-12)); // scipy 1.x
    CHECK_THAT(h[41], WithinAbs(0.22490814275255214, 1e-12));
}
