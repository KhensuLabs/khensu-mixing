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

    // STU-10: bands 4-6 are free once 1-3 are taken; a band taken is reset
    // to a plain stereo, static band.
    const auto fourth = mapRecommendationToDsp(recWith({{"eq_freq_hz", 300.0f}, {"eq_gain_db", -2.0f}}), 1.0f,
                                               baseFrom({{"eq_on", 1.0f}, {"eq2_on", 1.0f}, {"eq3_on", 1.0f}}));
    CHECK(fourth.values.at("eq4_on") == 1.0f);
    CHECK(fourth.values.at("eq4_freq") == Approx(300.0f));
    CHECK(fourth.values.at("eq4_kind") == static_cast<float>(EqBandKind::Shape));
    CHECK(fourth.values.at("eq4_mode") == static_cast<float>(EqBandChannels::Stereo));
    CHECK(fourth.values.at("eq4_dyn_on") == 0.0f);

    // No free band: the EQ keys are reported, not forced onto a used band.
    std::map<std::string, float> allOn;
    for (const auto& band : kEqBandParamIds) allOn[band.on] = 1.0f;
    const auto full = mapRecommendationToDsp(recWith({{"eq_freq_hz", 300.0f}, {"eq_gain_db", -2.0f}}), 1.0f,
                                             baseFrom(allOn));
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
    const auto fix = mapRecommendationToDsp(
        recWith({{"denoise_reduction_db", 6.0f}, {"reverb_wet", 0.2f}, {"something_new", 1.0f}}), 1.0f, baseFrom({}));
    CHECK(fix.values.empty());
    CHECK(fix.unmappedKeys.size() == 3);
    CHECK(contains(fix.unmappedKeys, "denoise_reduction_db"));
    CHECK(contains(fix.unmappedKeys, "something_new"));
    float lo = 0.0f, hi = 0.0f;
    CHECK(dspParamRange("deess_freq", lo, hi));
    CHECK(lo == Approx(2000.0f));
    CHECK(dspParamRange("eq5_dyn_release_ms", lo, hi));
    CHECK(hi == Approx(2000.0f));
    CHECK(dspParamRange("width_amount", lo, hi));
    CHECK(hi == Approx(2.0f));
    CHECK_FALSE(dspParamRange("not_a_param", lo, hi));
}

TEST_CASE("Width, transient and saturation keys map onto the v5 stages (STU-10)", "[stu10][mapper]")
{
    const auto fix = mapRecommendationToDsp(
        recWith({{"stereo_width", 1.2f}, {"transient_attack_db", 4.0f}, {"saturation_drive_db", 3.0f}}), 0.5f,
        baseFrom({}));
    CHECK(fix.unmappedKeys.empty());
    CHECK(fix.values.at("width_on") == 1.0f);
    CHECK(fix.values.at("width_amount") == Approx(1.1f)); // 1 + (1.2 - 1) x 0.5
    CHECK(fix.values.at("ts_on") == 1.0f);
    CHECK(fix.values.at("ts_attack_db") == Approx(2.0f));
    CHECK(fix.values.at("ts_sustain_db") == Approx(0.0f));
    CHECK(fix.values.at("sat_on") == 1.0f);
    CHECK(fix.values.at("sat_drive_db") == Approx(1.5f));
    CHECK(fix.values.at("sat_warmth") == Approx(0.5f));
    CHECK(fix.values.at("sat_mix") == Approx(1.0f));
}

TEST_CASE("A stereo-width finding without parameters is solved from its evidence (STU-10)", "[stu10][mapper]")
{
    // Width 0.80 over a 0.60 genre maximum: aim 0.03 inside it.
    Recommendation wide = recWith({});
    wide.id = "stereo_too_wide_file1";
    EvidenceItem e;
    e.measurement = "Stereo width";
    e.value = 0.80f;
    e.threshold = 0.60f;
    e.comparison = "above";
    wide.evidence.push_back(e);
    const auto fix = mapRecommendationToDsp(wide, 1.0f, baseFrom({}));
    REQUIRE(fix.values.count("width_amount") == 1);
    const float g = fix.values.at("width_amount");
    CHECK(g == Approx(sideGainForWidth(0.80f, 0.57f)));
    CHECK(g < 1.0f);
    // The gain puts the width where it aimed: r' = g^2 r, w = 2r / (1 + r).
    const double r = 0.80 / (2.0 - 0.80) * static_cast<double>(g) * static_cast<double>(g);
    CHECK(2.0 * r / (1.0 + r) == Approx(0.57).margin(1e-4));
    // Half the amount moves half way from 1.
    CHECK(mapRecommendationToDsp(wide, 0.5f, baseFrom({})).values.at("width_amount") == Approx(1.0f + (g - 1.0f) * 0.5f));

    Recommendation narrow = wide;
    narrow.id = "stereo_too_narrow_file1";
    narrow.evidence[0].value = 0.20f;
    narrow.evidence[0].threshold = 0.35f;
    CHECK(mapRecommendationToDsp(narrow, 1.0f, baseFrom({})).values.at("width_amount") > 1.0f);

    // Without the evidence there is nothing to solve from.
    Recommendation bare = recWith({});
    bare.id = "stereo_too_wide_file1";
    CHECK(mapRecommendationToDsp(bare, 1.0f, baseFrom({})).empty());
}

TEST_CASE("A too-wide width reading clamped at 1 is solved from the M/S balance (STU-16)", "[stu16][mapper]")
{
    // Side 1.5x as loud as the mid in energy: r = 1.5, unclamped width
    // 2r / (1 + r) = 1.2, M/S balance (1 - r) / (1 + r) = -0.2; the analyzer
    // reports the width as 1.
    Recommendation wide = recWith({});
    wide.id = "stereo_too_wide_file1";
    EvidenceItem w;
    w.measurement = "Stereo width";
    w.value = 1.0f;
    w.threshold = 0.60f;
    w.comparison = "above";
    wide.evidence.push_back(w);

    // Without the balance a clamped reading is solved from 1 itself: a
    // partial fix, gentler than the one the balance gives.
    const auto partial = mapRecommendationToDsp(wide, 1.0f, baseFrom({}));
    REQUIRE(partial.values.count("width_amount") == 1);
    CHECK(partial.values.at("width_amount") == Approx(sideGainForWidth(1.0f, 0.57f)));

    EvidenceItem b;
    b.measurement = "Mid/side balance";
    b.value = -0.2f;
    wide.evidence.push_back(b);
    const auto fix = mapRecommendationToDsp(wide, 1.0f, baseFrom({}));
    REQUIRE(fix.values.count("width_amount") == 1);
    const double g = fix.values.at("width_amount");
    CHECK(g == Approx(sideGainForWidth(1.2f, 0.57f)));
    const double r = 1.5 * g * g;
    CHECK(2.0 * r / (1.0 + r) == Approx(0.57).margin(1e-4));

    // An unclamped reading keeps solving from the width itself.
    wide.evidence[0].value = 0.80f;
    CHECK(mapRecommendationToDsp(wide, 1.0f, baseFrom({})).values.at("width_amount")
          == Approx(sideGainForWidth(0.80f, 0.57f)));
}

TEST_CASE("Sub-bass too wide monos the low end; bass_mono_hz sets the crossover (STU-10)", "[stu10][mapper]")
{
    Recommendation sub = recWith({});
    sub.id = "sub_bass_too_wide_file1";
    const auto fix = mapRecommendationToDsp(sub, 1.0f, baseFrom({}));
    CHECK(fix.values.at("bassmono_on") == 1.0f);
    CHECK(fix.values.at("bassmono_freq") == Approx(kSubBassMonoHz));
    const auto keyed = mapRecommendationToDsp(recWith({{"bass_mono_hz", 90.0f}}), 1.0f, baseFrom({}));
    CHECK(keyed.values.at("bassmono_freq") == Approx(90.0f));
    CHECK(keyed.unmappedKeys.empty());
}

TEST_CASE("The stem-masking plan lands on a self-keyed dynamic band (STU-10)", "[stu10][mapper]")
{
    // What MaskingRecommender writes: a static fallback and the dynamic plan.
    const auto rec = recWith({{"eq_freq_hz", 2000.0f},
                              {"eq_gain_db", -3.0f},
                              {"dyneq_freq_hz", 1900.0f},
                              {"dyneq_q", 1.5f},
                              {"dyneq_threshold_db", -30.0f},
                              {"dyneq_max_cut_db", 4.0f},
                              {"dyneq_key_stem", 0.0f},
                              {"dyneq_target_stem", 2.0f}});
    const auto fix = mapRecommendationToDsp(rec, 0.5f, baseFrom({{"eq_on", 1.0f}}));
    CHECK(fix.values.at("eq2_on") == 1.0f);
    CHECK(fix.values.at("eq2_freq") == Approx(1900.0f));
    CHECK(fix.values.at("eq2_gain_db") == Approx(0.0f));
    CHECK(fix.values.at("eq2_q") == Approx(1.5f));
    CHECK(fix.values.at("eq2_dyn_on") == 1.0f);
    CHECK(fix.values.at("eq2_dyn_thresh_db") == Approx(-30.0f));
    CHECK(fix.values.at("eq2_dyn_range_db") == Approx(2.0f)); // the depth follows the amount
    CHECK(fix.values.count("eq3_on") == 0);                    // the static fallback is not placed too
    // The plugin cannot key on another stem: that key is reported.
    CHECK(fix.unmappedKeys == std::vector<std::string>{"dyneq_key_stem"});
}
