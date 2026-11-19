// tests/shared-core/test_analysis_result_json.cpp
// WF-8: the measured block round-trips, so the plugin grades the same numbers.
#include <catch2/catch_test_macros.hpp>
#include <khensu/analysis/AnalysisResultJson.h>
#include <khensu/recommendations/RecommendationEngine.h>
#include <khensu/targets/CreativeTarget.h>

using namespace khensu;

TEST_CASE("Measured block round-trips and grades the same", "[wf8][analysis_json]")
{
    AnalysisResult a;
    a.file_id = "f1";
    a.sample_rate = 48000.0;
    a.channel_layout = {"Stereo", 2, false};
    a.loudness = {-7.5f, -5.0f, 3.2f, true, 7.0f, 0.4f};
    a.spectral.rms_per_octave_db = {-20, -18, -19, -22, -25, -27, -30, -33, -38, -45};
    a.spectral.fft_mid_magnitudes.assign(2048, -60.0f);
    a.spectral.centroid_hz = 4100.0f;
    a.spectral.balance_low = 0.62f;
    a.spectral.balance_mid = 0.30f;
    a.spectral.balance_high = 0.08f;
    a.spectral.tilt_db_per_octave = -2.6f;
    a.dynamics = {6.1f, 5.0f, 6.1f};
    a.stereo = {0.21f, 0.8f, 0.1f};
    a.phase.overall_correlation = 0.8f;
    a.artifacts.has_clipping = true;
    a.artifacts.clipping_frame_count = 42;
    a.artifacts.noise_floor_db = -48.0f;
    a.artifacts.noise_quiet_seconds = 3.0f;
    a.artifacts.noise_floor_spread_db = 2.0f;
    a.artifacts.dc_offset = 0.01f;
    a.artifacts.dc_offset_steady = false;
    AnalysisResult::MonoCompatData mc;
    mc.per_band_correlation = {{"sub", 0.9f}, {"air", 0.2f}};
    mc.low_end_stereo_pct = 12.0f;
    a.mono_compat = mc;

    const auto text = juce::JSON::toString(analysis_json::measuredToVar(a));
    AnalysisResult b;
    b.file_id = "f1";
    REQUIRE(analysis_json::measuredFromVar(juce::JSON::parse(text), b));
    CHECK(b.loudness.true_peak_dbtp == a.loudness.true_peak_dbtp);
    CHECK(b.spectral.rms_per_octave_db == a.spectral.rms_per_octave_db);
    CHECK(b.spectral.fft_mid_magnitudes.size() == 2048);
    CHECK(b.artifacts.clipping_frame_count == 42);
    CHECK_FALSE(b.artifacts.dc_offset_steady);
    REQUIRE(b.mono_compat.has_value());
    CHECK(b.mono_compat->per_band_correlation.at("air") == 0.2f);
    CHECK(b.channel_layout.layout_name == "Stereo");

    GenreEntry genre;
    const auto ra = RecommendationEngine{}.generate(a, genre, CreativeTarget{});
    const auto rb = RecommendationEngine{}.generate(b, genre, CreativeTarget{});
    REQUIRE(ra.size() == rb.size());
    for (size_t i = 0; i < ra.size(); ++i) {
        CHECK(ra[i].id == rb[i].id);
        CHECK(ra[i].suggested_parameters == rb[i].suggested_parameters);
    }

    AnalysisResult untouched;
    CHECK_FALSE(analysis_json::measuredFromVar(juce::JSON::parse("{\"version\": 99}"), untouched));
    CHECK_FALSE(analysis_json::measuredFromVar({}, untouched));
}
