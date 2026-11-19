// CE-PLT-3: the native meters agree with the Python service's meters.
// Fixtures: tests/shared-core/fixtures/native_meter_parity.json, written by
// scripts/gen_native_meter_fixtures.py from loudness.py / dsp_metrics.py.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <juce_core/juce_core.h>
#include <khensu/analysis/NativeMeters.h>

#include <cmath>
#include <cstdint>
#include <vector>

using Catch::Matchers::WithinAbs;
namespace kn = khensu::native;

namespace {

juce::var loadFixtures()
{
    const auto file = juce::File(KHENSU_REPO_ROOT_DIR)
                          .getChildFile("tests")
                          .getChildFile("shared-core")
                          .getChildFile("fixtures")
                          .getChildFile("native_meter_parity.json");
    REQUIRE(file.existsAsFile());
    auto parsed = juce::JSON::parse(file);
    REQUIRE(parsed.isObject());
    return parsed;
}

/// Mirrors synthesize() in scripts/gen_native_meter_fixtures.py.
std::vector<std::vector<float>> synthesize(const juce::var& spec)
{
    const double sr = static_cast<double>(spec["sample_rate"]);
    const auto n = static_cast<std::int64_t>(std::nearbyint(static_cast<double>(spec["duration_sec"]) * sr));
    std::vector<std::vector<float>> out;
    const auto* chans = spec["channels"].getArray();
    REQUIRE(chans != nullptr);
    for (const auto& ch : *chans) {
        std::vector<double> y(static_cast<size_t>(n), 0.0);
        if (const auto* tones = ch["tones"].getArray()) {
            for (const auto& tone : *tones) {
                const double f = tone["freq"], a = tone["amp"];
                const double ph = tone.hasProperty("phase") ? static_cast<double>(tone["phase"]) : 0.0;
                for (std::int64_t i = 0; i < n; ++i) {
                    const double t = static_cast<double>(i) / sr;
                    y[static_cast<size_t>(i)] += a * std::sin(2.0 * 3.141592653589793 * f * t + ph);
                }
            }
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

void checkClose(const char* what, double native, const juce::var& expected, double tol)
{
    INFO(what << ": native " << native << " vs python " << expected.toString());
    if (expected.isVoid() || expected.isUndefined()) {
        CHECK_FALSE(std::isfinite(native));
        return;
    }
    REQUIRE(std::isfinite(native));
    CHECK_THAT(native, WithinAbs(static_cast<double>(expected), tol));
}

} // namespace

TEST_CASE("Native meters match the Python service meters", "[native][parity]")
{
    const auto fixtures = loadFixtures();
    const double tol = fixtures["tolerance"];
    const auto* signals = fixtures["signals"].getArray();
    REQUIRE(signals != nullptr);
    REQUIRE(signals->size() >= 4);

    for (const auto& spec : *signals) {
        const auto name = spec["name"].toString();
        DYNAMIC_SECTION(name.toStdString())
        {
            const auto chans = synthesize(spec);
            std::vector<const float*> ptrs;
            for (const auto& c : chans) ptrs.push_back(c.data());
            const auto n = static_cast<std::int64_t>(chans.front().size());
            const double sr = spec["sample_rate"];
            const auto r = kn::measureAll(ptrs.data(), static_cast<int>(ptrs.size()), n, sr);
            const auto& e = spec["expected"];

            checkClose("integrated", r.loudness.integratedLufs, e["integrated_lufs"], tol);
            checkClose("lra", r.loudness.lra, e["loudness_range_lu"], tol);
            checkClose("short-term max", r.loudness.shortTermMaxLufs, e["short_term_max_lufs"], tol);
            checkClose("momentary max", r.loudness.momentaryMaxLufs, e["momentary_max_lufs"], tol);
            checkClose("true peak", r.loudness.truePeakDbtp, e["true_peak_dbtp"], tol);
            checkClose("sample peak", r.loudness.samplePeakDbfs, e["sample_peak_dbfs"], 1e-3);
            const auto* levels = e["octave_levels_db"].getArray();
            REQUIRE(levels != nullptr);
            // The tilt is a regression over every band level, so it is only
            // comparable when every band holds real content: a band at the
            // 1e-30 floor (the pure fs/4 tone) is FFT round-off in both
            // implementations, and its level - hence the slope - is noise.
            bool tiltWellConditioned = true;
            for (const auto& lv : *levels)
                if (static_cast<double>(lv) < -250.0) tiltWellConditioned = false;
            if (tiltWellConditioned) checkClose("tilt", r.spectrum.tiltDbPerOctave, e["tilt_db_per_octave"], tol);
            REQUIRE(r.spectrum.levelsDb.size() == static_cast<size_t>(levels->size()));
            for (int i = 0; i < levels->size(); ++i) {
                // Bands with no real content sit near the -300 dB floor, where
                // a ULP of FFT round-off is a large dB move: compare those loosely.
                const double want = (*levels)[i];
                const double t = want < -150.0 ? 30.0 : tol;
                checkClose("octave level", r.spectrum.levelsDb[static_cast<size_t>(i)], (*levels)[i], t);
            }

            if (e.hasProperty("stereo_width")) {
                REQUIRE(r.stereo.stereo);
                checkClose("width", r.stereo.width, e["stereo_width"], 1e-4);
                checkClose("correlation", r.stereo.correlation, e["stereo_correlation"], 1e-4);
            }
        }
    }
}

TEST_CASE("Native meters: edge cases", "[native]")
{
    SECTION("shorter than one 400 ms block: integrated and LRA undefined")
    {
        std::vector<float> x(4800, 0.25f);
        const float* p = x.data();
        const auto m = kn::measureLoudness(&p, 1, static_cast<std::int64_t>(x.size()), 48000.0);
        CHECK_FALSE(std::isfinite(m.integratedLufs));
        CHECK_FALSE(std::isfinite(m.lra));
        CHECK(m.shortTerm.size() == 1); // one window over everything
    }
    SECTION("digital silence: NaN windows, nothing gated in")
    {
        std::vector<float> x(48000 * 4, 0.0f);
        const float* p[2] = {x.data(), x.data()};
        const auto m = kn::measureLoudness(p, 2, static_cast<std::int64_t>(x.size()), 48000.0);
        CHECK_FALSE(std::isfinite(m.integratedLufs));
        CHECK_FALSE(std::isfinite(m.shortTermMaxLufs));
        const auto s = kn::measureStereo(x.data(), x.data(), static_cast<std::int64_t>(x.size()));
        CHECK(s.correlation == 1.0);
        CHECK(s.width == 0.0);
    }
    SECTION("unsupported channel count is reported, peaks still measured")
    {
        std::vector<float> x(48000, 0.5f);
        std::vector<const float*> p(7, x.data());
        const auto m = kn::measureLoudness(p.data(), 7, static_cast<std::int64_t>(x.size()), 48000.0);
        CHECK_FALSE(m.supportedLayout);
        CHECK_THAT(m.samplePeakDbfs, WithinAbs(-6.0206, 1e-3));
    }
    SECTION("anti-phase content clamps width to 1 and correlation to -1")
    {
        std::vector<float> l(44100), r(44100);
        for (size_t i = 0; i < l.size(); ++i) {
            l[i] = 0.5f * static_cast<float>(std::sin(0.05 * static_cast<double>(i)));
            r[i] = -l[i];
        }
        const auto s = kn::measureStereo(l.data(), r.data(), static_cast<std::int64_t>(l.size()));
        CHECK_THAT(s.width, WithinAbs(1.0, 1e-9));
        CHECK_THAT(s.correlation, WithinAbs(-1.0, 1e-9));
    }
}
