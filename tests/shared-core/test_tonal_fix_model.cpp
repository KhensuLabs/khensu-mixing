// tests/shared-core/test_tonal_fix_model.cpp
// MEAS-4: the tonal fixes are solved from the track's own spectrum.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <khensu/recommendations/SpectralRecommender.h>
#include <khensu/recommendations/TonalFixModel.h>

using namespace khensu;

namespace {
std::vector<float> pinkish() // -1 dB per octave band: a low-end share near 0.36
{
    std::vector<float> db;
    for (int b = 0; b < 10; ++b)
        db.push_back(-10.0f - 1.0f * static_cast<float>(b));
    return db;
}
} // namespace

TEST_CASE("Shelf response: full gain on its side, none on the other", "[tonal_fix][meas4]")
{
    using namespace tonal_fix;
    CHECK_THAT(10.0 * std::log10(shelfPowerGain(31.5, 150.0, -10.0, false)), Catch::Matchers::WithinAbs(-10.0, 0.5));
    CHECK_THAT(10.0 * std::log10(shelfPowerGain(4000.0, 150.0, -10.0, false)), Catch::Matchers::WithinAbs(0.0, 0.1));
    CHECK_THAT(10.0 * std::log10(shelfPowerGain(16000.0, 4000.0, 6.0, true)), Catch::Matchers::WithinAbs(6.0, 0.5));
}

TEST_CASE("Low-shelf gain lands the low-end share on its aim", "[tonal_fix][meas4]")
{
    using namespace tonal_fix;
    const auto db = pinkish();
    const auto p = bandPowers(db);
    const double before = lowShare(p);
    for (double aim : {before * 0.6, before + (1.0 - before) * 0.3}) {
        const float g = lowShelfGainForShare(db, before, aim, 150.0, -15.0, 15.0);
        REQUIRE(std::isfinite(g));
        CHECK((aim < before ? g < 0.0f : g > 0.0f));
        CHECK_THAT(lowShare(withShelf(p, 150.0, g, false)), Catch::Matchers::WithinAbs(aim, 1e-4));
    }
    CHECK(std::isnan(lowShelfGainForShare({}, 0.3, 0.3, 150.0, -15.0, 15.0)));
}

TEST_CASE("A heavy low end gets a low shelf big enough to clear it", "[tonal_fix][meas4]")
{
    GenreEntry genre;
    genre.low_end_weight = 0.08f; // ceiling ~0.17 - the boosted mix reads ~0.23
    genre.spectral_centroid_hz = 2500.0f;
    AnalysisResult r;
    r.file_id = "t";
    r.spectral.rms_per_octave_db = pinkish();
    // +10 dB on the three low bands.
    for (int b = 0; b < 3; ++b)
        r.spectral.rms_per_octave_db[static_cast<size_t>(b)] += 10.0f;
    const auto p = tonal_fix::bandPowers(r.spectral.rms_per_octave_db);
    r.spectral.balance_low = static_cast<float>(tonal_fix::lowShare(p));
    r.spectral.centroid_hz = 2500.0f;
    REQUIRE(r.spectral.balance_low > SpectralRecommender::lowEndCeiling(genre));
    const auto recs = SpectralRecommender{}.recommend(r, genre, CreativeTarget{});
    const auto it = std::find_if(recs.begin(), recs.end(), [](const Recommendation& x) {
        return x.suggested_parameters.count("eq_freq_hz") && x.suggested_parameters.at("eq_type") == 2.0f;
    });
    REQUIRE(it != recs.end());
    const float g = it->suggested_parameters.at("eq_gain_db");
    CHECK(g < -3.0f); // the old slope asked for ~-0.5 dB here
    const double after = tonal_fix::lowShare(tonal_fix::withShelf(p, SpectralRecommender::kLowShelfHz, g, false));
    CHECK(after < SpectralRecommender::lowEndCeiling(genre));
}

namespace {
/// FFT bins (dB power per bin, 2048 bins at 48 kHz) sloping at `slope` dB
/// per octave in bin power (the band-power tilt reads ~3 dB/octave higher).
std::vector<float> slopedBins(double slopeDbPerOctave)
{
    std::vector<float> bins(2048, -90.0f);
    const double binHz = 24000.0 / 2048.0;
    for (size_t k = 1; k < bins.size(); ++k)
        bins[k] = static_cast<float>(-20.0 + slopeDbPerOctave * std::log2(static_cast<double>(k) * binHz / 1000.0));
    return bins;
}
double tiltOf(const std::vector<float>& bins, double cornerHz = 1000.0, double gainDb = 0.0)
{
    std::vector<double> power(bins.size());
    for (size_t k = 0; k < bins.size(); ++k)
        power[k] = bins[k] <= -89.9f ? 0.0 : std::pow(10.0, bins[k] / 10.0);
    return tonal_fix::tiltWithShelfFft(power, 24000.0 / 2048.0, 48000.0, cornerHz, gainDb);
}
} // namespace

TEST_CASE("High-shelf gain lands the spectral tilt on its aim (FIX-A)", "[tonal_fix]")
{
    const auto bins = slopedBins(-4.5);
    const double measured = tiltOf(bins);
    for (double aim : {measured + 1.5, measured - 1.5}) {
        const float g = tonal_fix::highShelfGainForTiltFft(bins, 48000.0, measured, aim, 1000.0, -12.0, 12.0);
        REQUIRE(std::isfinite(g));
        CHECK((aim > measured ? g > 0.0f : g < 0.0f));
        CHECK_THAT(tiltOf(bins, 1000.0, g), Catch::Matchers::WithinAbs(aim, 0.02));
    }
    CHECK(std::isnan(tonal_fix::highShelfGainForTiltFft({}, 48000.0, -4.0, -3.0, 1000.0, -12.0, 12.0)));
}

TEST_CASE("A tilt finding renders a shelf that clears it (FIX-A)", "[tonal_fix]")
{
    GenreEntry genre;
    genre.spectral_tilt_target_db_per_octave = -3.0f; // window -5 .. -1
    genre.spectral_centroid_hz = 2500.0f;
    genre.low_end_weight = 0.4f;
    AnalysisResult r;
    r.file_id = "t";
    r.sample_rate = 48000.0;
    // Band power sums twice the bins each octave up (+3 dB/octave): -9 per
    // bin reads as about -6 per band, under the -5 edge.
    r.spectral.fft_mid_magnitudes = slopedBins(-9.0);
    r.spectral.tilt_db_per_octave = static_cast<float>(tiltOf(r.spectral.fft_mid_magnitudes));
    r.spectral.centroid_hz = 2500.0f;
    r.spectral.balance_low = 0.4f;
    REQUIRE(r.spectral.tilt_db_per_octave < -5.0f);
    const auto recs = SpectralRecommender{}.recommend(r, genre, CreativeTarget{});
    const auto it = std::find_if(recs.begin(), recs.end(),
                                 [](const Recommendation& x) { return x.id.rfind("tilt_too_dark", 0) == 0; });
    REQUIRE(it != recs.end());
    REQUIRE(it->suggested_parameters.count("eq_gain_db") == 1);
    const float g = it->suggested_parameters.at("eq_gain_db");
    CHECK(g > 0.0f);
    CHECK(it->suggested_parameters.at("eq_freq_hz") == SpectralRecommender::kTiltShelfHz);
    const double after = tiltOf(r.spectral.fft_mid_magnitudes, SpectralRecommender::kTiltShelfHz, g);
    CHECK(after > -5.0); // inside the window: the finding clears
}

TEST_CASE("Low-shelf gain from the FFT bins moves the low-end share the right way (TUNE-10)", "[tonal_fix]")
{
    const auto bins = slopedBins(-4.5);
    const double measured = 0.30;
    for (double aim : {0.40, 0.20}) {
        const float g = tonal_fix::lowShelfGainForShareFft(bins, 48000.0, measured, aim, 150.0, -15.0, 15.0);
        REQUIRE(std::isfinite(g));
        CHECK((aim > measured ? g > 0.0f : g < 0.0f));
    }
    // Larger moves need larger gains (the model is monotone).
    const float small = tonal_fix::lowShelfGainForShareFft(bins, 48000.0, measured, 0.35, 150.0, 0.0, 15.0);
    const float large = tonal_fix::lowShelfGainForShareFft(bins, 48000.0, measured, 0.45, 150.0, 0.0, 15.0);
    CHECK(large > small);
    CHECK(std::isnan(tonal_fix::lowShelfGainForShareFft({}, 48000.0, 0.3, 0.4, 150.0, 0.0, 15.0)));
}
