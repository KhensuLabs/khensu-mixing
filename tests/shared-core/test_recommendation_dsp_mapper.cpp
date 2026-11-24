// tests/shared-core/test_recommendation_dsp_mapper.cpp
// OVH-8: a finding's suggested_parameters -> the Copilot plugin's DSP
// parameters, scaled by an amount (khensu/mixerchain/RecommendationDspMapper.h).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <khensu/mixerchain/RecommendationDspMapper.h>

#include <algorithm>
#include <map>
#include <string>

using namespace khensu;
using namespace khensu::mixerchain;
using Catch::Approx;

namespace {
Recommendation recWith(std::map<std::string, float> params)
{
    Recommendation rec;
    rec.id = "r";
    rec.suggested_parameters = std::move(params);
    return rec;
}

std::function<float(const std::string&)> baseFrom(std::map<std::string, float> values)
{
    return [values](const std::string& id) {
        const auto it = values.find(id);
        return it != values.end() ? it->second : 0.0f;
    };
}

bool contains(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}
} // namespace

TEST_CASE("Amount scaling follows the desktop's scaledRecommendation rules", "[ovh8][mapper]")
{
    const auto rec = recWith({{"eq_gain_db", -4.0f},
                              {"eq_freq_hz", 3000.0f},
                              {"comp_ratio", 5.0f},
                              {"comp_threshold_db", -20.0f},
                              {"stereo_width", 0.5f},
                              {"reduce_gain_db", 2.0f}});
    const auto half = scaledForAmount(rec, 0.5f);
    CHECK(half.suggested_parameters.at("eq_gain_db") == Approx(-2.0f));
    CHECK(half.suggested_parameters.at("eq_freq_hz") == Approx(3000.0f));      // frequencies as is
    CHECK(half.suggested_parameters.at("comp_ratio") == Approx(3.0f));         // 1 + (5-1)/2
    CHECK(half.suggested_parameters.at("comp_threshold_db") == Approx(-20.0f)); // levels as is
    CHECK(half.suggested_parameters.at("stereo_width") == Approx(0.75f));
    CHECK(half.suggested_parameters.at("reduce_gain_db") == Approx(1.0f));
    // Out-of-range amounts clamp.
    CHECK(scaledForAmount(rec, 7.0f).suggested_parameters.at("eq_gain_db") == Approx(-4.0f));
    CHECK(scaledForAmount(rec, -1.0f).suggested_parameters.at("eq_gain_db") == Approx(0.0f));
}

TEST_CASE("Shelves land on a real shelf band, bells and notches on free bands", "[ovh8][mapper]")
{
    // Band 1 is in use: the low shelf goes to band 2, the notch to band 3.
    const auto base = baseFrom({{"eq_on", 1.0f}});
    const auto fix = mapRecommendationToDsp(
        recWith({{"eq_freq_hz", 120.0f}, {"eq_gain_db", -3.0f}, {"eq_type", 2.0f}}), 1.0f, base);
    CHECK(fix.values.count("eq_on") == 0); // band 1 untouched
    CHECK(fix.values.at("eq2_on") == 1.0f);
    CHECK(fix.values.at("eq2_type") == static_cast<float>(EqBandShape::LowShelf));
    CHECK(fix.values.at("eq2_freq") == Approx(120.0f));
    CHECK(fix.values.at("eq2_gain_db") == Approx(-3.0f));
    CHECK(fix.values.at("eq2_q") == Approx(0.707f));
    CHECK(fix.unmappedKeys.empty());

    const auto high = mapRecommendationToDsp(
        recWith({{"eq_freq_hz", 8000.0f}, {"eq_gain_db", 2.0f}, {"eq_type", 1.0f}}), 1.0f, baseFrom({}));
    CHECK(high.values.at("eq_type") == static_cast<float>(EqBandShape::HighShelf));

    const auto notch = mapRecommendationToDsp(recWith({{"eq_center_hz", 2500.0f}, {"eq_cut_db", 6.0f}}), 0.5f,
                                              baseFrom({{"eq_on", 1.0f}, {"eq2_on", 1.0f}}));
    CHECK(notch.values.at("eq3_on") == 1.0f);
    CHECK(notch.values.at("eq3_type") == static_cast<float>(EqBandShape::Bell));
    CHECK(notch.values.at("eq3_gain_db") == Approx(-3.0f)); // a cut, scaled by the amount
    CHECK(notch.values.at("eq3_q") == Approx(6.0f));

    // No free band: the EQ keys are reported, not forced onto a used band.
    const auto full = mapRecommendationToDsp(recWith({{"eq_freq_hz", 300.0f}, {"eq_gain_db", -2.0f}}), 1.0f,
                                             baseFrom({{"eq_on", 1.0f}, {"eq2_on", 1.0f}, {"eq3_on", 1.0f}}));
    CHECK(full.values.empty());
    CHECK(contains(full.unmappedKeys, "eq_gain_db"));
}

TEST_CASE("Highpass, compressor, limiter and de-esser keys map onto their stages", "[ovh8][mapper]")
{
    const auto fix = mapRecommendationToDsp(recWith({{"highpass_hz", 40.0f},
                                                     {"comp_threshold_db", -24.0f},
                                                     {"comp_ratio", 3.0f},
                                                     {"comp_attack_ms", 10.0f},
                                                     {"comp_release_ms", 120.0f},
                                                     {"true_peak_ceiling_dbtp", -1.0f},
                                                     {"deess_threshold_db", -28.0f},
                                                     {"deess_freq_hz", 7000.0f}}),
                                            1.0f, baseFrom({}));
    CHECK(fix.values.at("hpf_on") == 1.0f);
    CHECK(fix.values.at("hpf_freq") == Approx(40.0f));
    CHECK(fix.values.at("comp_on") == 1.0f);
    CHECK(fix.values.at("comp_thresh_db") == Approx(-24.0f));
    CHECK(fix.values.at("comp_ratio") == Approx(3.0f));
    CHECK(fix.values.at("comp_attack_ms") == Approx(10.0f));
    CHECK(fix.values.at("comp_release_ms") == Approx(120.0f));
    CHECK(fix.values.at("lim_on") == 1.0f);
    CHECK(fix.values.at("lim_thresh_db") == Approx(-1.0f));
    CHECK(fix.values.at("deess_on") == 1.0f);
    CHECK(fix.values.at("deess_thresh_db") == Approx(-28.0f));
    CHECK(fix.values.at("deess_freq") == Approx(7000.0f));
    CHECK(fix.values.at("deess_range_db") == Approx(6.0f));
    CHECK(fix.unmappedKeys.empty());
    CHECK(fix.mappedKeys.size() == 8);

    // The amount scales the ratio and the de-esser depth, not the levels.
    const auto half = mapRecommendationToDsp(
        recWith({{"comp_ratio", 3.0f}, {"comp_threshold_db", -24.0f}, {"deess_freq_hz", 6000.0f}}), 0.5f, baseFrom({}));
    CHECK(half.values.at("comp_ratio") == Approx(2.0f));
    CHECK(half.values.at("comp_thresh_db") == Approx(-24.0f));
    CHECK(half.values.at("deess_range_db") == Approx(3.0f));
}

TEST_CASE("Gain keys move the output gain relative to its value before the fix", "[ovh8][mapper]")
{
    const auto base = baseFrom({{"out_gain_db", 2.0f}});
    const auto fix = mapRecommendationToDsp(
        recWith({{"reduce_gain_db", 3.0f}, {"comp_makeup_gain_db", 1.0f}, {"limiter_gain_db", -0.5f}}), 1.0f, base);
    CHECK(fix.values.at("out_gain_db") == Approx(2.0f - 3.0f + 1.0f + 0.5f));
    const auto half = mapRecommendationToDsp(recWith({{"increase_gain_db", 4.0f}}), 0.5f, base);
    CHECK(half.values.at("out_gain_db") == Approx(4.0f));
    // Values clamp to the plugin's ranges.
    const auto big = mapRecommendationToDsp(recWith({{"increase_gain_db", 40.0f}}), 1.0f, base);
    CHECK(big.values.at("out_gain_db") == Approx(24.0f));
}

TEST_CASE("Keys the built-in chain cannot do are reported", "[ovh8][mapper]")
{
    const auto fix = mapRecommendationToDsp(recWith({{"stereo_width", 1.2f},
                                                     {"denoise_reduction_db", 6.0f},
                                                     {"transient_attack_db", 2.0f},
                                                     {"saturation_drive_db", 3.0f},
                                                     {"reverb_wet", 0.2f},
                                                     {"something_new", 1.0f}}),
                                            1.0f, baseFrom({}));
    CHECK(fix.values.empty());
    CHECK(fix.unmappedKeys.size() == 6);
    CHECK(contains(fix.unmappedKeys, "stereo_width"));
    CHECK(contains(fix.unmappedKeys, "something_new"));
    float lo = 0.0f, hi = 0.0f;
    CHECK(dspParamRange("deess_freq", lo, hi));
    CHECK(lo == Approx(2000.0f));
    CHECK_FALSE(dspParamRange("not_a_param", lo, hi));
}
