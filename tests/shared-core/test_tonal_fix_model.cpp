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
