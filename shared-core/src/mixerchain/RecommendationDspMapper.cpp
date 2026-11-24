#include <khensu/mixerchain/RecommendationDspMapper.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace khensu::mixerchain {

namespace {
using Ids = DspParamIds;
using Ids3 = DspParamIdsV3;

struct Range {
    const char* id;
    float lo, hi;
};
// Mirrors KhensuCopilotProcessor::createParameterLayout() (plugin-copilot);
// tests/plugin/PluginMapperRangeTest.cpp keeps the two in step.
constexpr Range kRanges[] = {
    {Ids::hpfFreq, 20.0f, 1000.0f},        {Ids::lpfFreq, 1000.0f, 20000.0f},
    {Ids::eqFreq, 60.0f, 16000.0f},        {Ids::eqGainDb, -12.0f, 12.0f},
    {Ids::eqQ, 0.3f, 8.0f},                {Ids::eq2Freq, 60.0f, 16000.0f},
    {Ids::eq2GainDb, -12.0f, 12.0f},       {Ids::eq2Q, 0.3f, 8.0f},
    {Ids::eq3Freq, 60.0f, 16000.0f},       {Ids::eq3GainDb, -12.0f, 12.0f},
    {Ids::eq3Q, 0.3f, 8.0f},               {Ids::compThreshDb, -48.0f, 0.0f},
    {Ids::compRatio, 1.0f, 20.0f},         {Ids::compAttackMs, 0.1f, 200.0f},
    {Ids::compReleaseMs, 5.0f, 1000.0f},   {Ids::outGainDb, -24.0f, 24.0f},
    {Ids::limThreshDb, -12.0f, 0.0f},      {Ids3::eqType, 0.0f, 2.0f},
    {Ids3::eq2Type, 0.0f, 2.0f},           {Ids3::eq3Type, 0.0f, 2.0f},
    {Ids3::deessFreq, 2000.0f, 12000.0f},  {Ids3::deessThreshDb, -60.0f, 0.0f},
    {Ids3::deessRangeDb, 0.0f, 24.0f},     {Ids3::fixAmount, 0.0f, 1.0f},
};

struct Band {
    const char* on;
    const char* freq;
    const char* gain;
    const char* q;
    const char* type;
    int number;
};
constexpr Band kBands[] = {{Ids::eqOn, Ids::eqFreq, Ids::eqGainDb, Ids::eqQ, Ids3::eqType, 1},
                           {Ids::eq2On, Ids::eq2Freq, Ids::eq2GainDb, Ids::eq2Q, Ids3::eq2Type, 2},
                           {Ids::eq3On, Ids::eq3Freq, Ids::eq3GainDb, Ids::eq3Q, Ids3::eq3Type, 3}};

std::string fmt(const char* pattern, double a, double b = 0.0, double c = 0.0)
{
    char buf[160];
    std::snprintf(buf, sizeof(buf), pattern, a, b, c);
    return buf;
}
} // namespace

bool dspParamRange(const std::string& paramId, float& lo, float& hi)
{
    for (const auto& r : kRanges)
        if (paramId == r.id) {
            lo = r.lo;
            hi = r.hi;
            return true;
        }
    return false;
}

Recommendation scaledForAmount(const Recommendation& rec, float amount)
{
    // Same rules as desktop-app's scaledRecommendation() (RecommendationChain.cpp).
    amount = std::isfinite(amount) ? std::clamp(amount, 0.0f, 1.0f) : 1.0f;
    Recommendation out = rec;
    for (auto& [key, value] : out.suggested_parameters) {
        const bool isDb = key.size() > 3 && key.compare(key.size() - 3, 3, "_db") == 0;
        const bool isLevel = key.find("threshold") != std::string::npos;
        if (isDb && !isLevel)
            value *= amount;
        else if (key == "comp_ratio" || key == "stereo_width")
            value = 1.0f + (value - 1.0f) * amount;
    }
    return out;
}

MappedFix mapRecommendationToDsp(const Recommendation& rec, float amount,
                                 const std::function<float(const std::string&)>& base)
{
    MappedFix out;
    const auto scaled = scaledForAmount(rec, amount);
    const auto& p = scaled.suggested_parameters;
    auto has = [&](const char* k) { return p.count(k) != 0; };
    auto get = [&](const char* k, float def = 0.0f) {
        const auto it = p.find(k);
        return it != p.end() && std::isfinite(it->second) ? it->second : def;
    };
    auto baseOf = [&](const char* id) { return base ? base(id) : 0.0f; };
    auto set = [&](const char* id, float v) {
        float lo = 0.0f, hi = 0.0f;
        if (dspParamRange(id, lo, hi)) v = std::clamp(v, lo, hi);
        out.values[id] = v;
    };
    std::set<std::string> used;
    auto use = [&](const char* k) {
        if (has(k)) used.insert(k);
    };

    if (has("highpass_hz")) {
        use("highpass_hz");
        set(Ids::hpfOn, 1.0f);
        set(Ids::hpfFreq, get("highpass_hz"));
        out.notes.push_back(fmt("High-pass at %.0f Hz", get("highpass_hz")));
    }

    // EQ moves land on bands that are free in `base`, in band order.
    std::vector<const Band*> freeBands;
    for (const auto& band : kBands)
        if (baseOf(band.on) < 0.5f) freeBands.push_back(&band);
    size_t nextBand = 0;
    auto placeBand = [&](float freq, float gainDb, float q, EqBandShape shape, const char* what) {
        if (nextBand >= freeBands.size()) return false;
        const Band& band = *freeBands[nextBand++];
        set(band.on, 1.0f);
        set(band.freq, freq);
        set(band.gain, gainDb);
        set(band.q, q);
        set(band.type, static_cast<float>(static_cast<int>(shape)));
        out.notes.push_back("EQ band " + std::to_string(band.number) + ": " + what
                            + fmt(" %.0f Hz %+.1f dB", freq, gainDb));
        return true;
    };

    if (has("eq_freq_hz") && has("eq_gain_db")) {
        const int eqType = has("eq_type") ? static_cast<int>(std::lround(get("eq_type"))) : 0;
        // eq_type uses EqCurveComponent's codes: 1 = high shelf, 2 = low shelf.
        const EqBandShape shape = eqType == 1   ? EqBandShape::HighShelf
                                  : eqType == 2 ? EqBandShape::LowShelf
                                                : EqBandShape::Bell;
        const float q = has("eq_q") ? get("eq_q") : 0.707f;
        if (placeBand(get("eq_freq_hz"), get("eq_gain_db"), q, shape,
                      shape == EqBandShape::Bell ? "bell" : shape == EqBandShape::LowShelf ? "low shelf" : "high shelf")) {
            use("eq_freq_hz");
            use("eq_gain_db");
            use("eq_q");
            use("eq_type");
        }
    }
    if (has("eq_center_hz") && has("eq_cut_db") && !has("eq_gain_db")) {
        const float q = has("eq_q") ? get("eq_q") : 6.0f;
        if (placeBand(get("eq_center_hz"), -std::abs(get("eq_cut_db")), q, EqBandShape::Bell, "notch")) {
            use("eq_center_hz");
            use("eq_cut_db");
            use("eq_q");
        }
    }

    if (has("comp_threshold_db") || has("comp_ratio") || has("comp_attack_ms") || has("comp_release_ms")) {
        set(Ids::compOn, 1.0f);
        if (has("comp_threshold_db")) set(Ids::compThreshDb, get("comp_threshold_db"));
        if (has("comp_ratio")) set(Ids::compRatio, get("comp_ratio"));
        if (has("comp_attack_ms")) set(Ids::compAttackMs, get("comp_attack_ms"));
        if (has("comp_release_ms")) set(Ids::compReleaseMs, get("comp_release_ms"));
        for (const char* k : {"comp_threshold_db", "comp_ratio", "comp_attack_ms", "comp_release_ms"}) use(k);
        out.notes.push_back(fmt("Compressor %.1f dB threshold, %.1f:1", get("comp_threshold_db", baseOf(Ids::compThreshDb)),
                                get("comp_ratio", baseOf(Ids::compRatio))));
    }

    if (has("deess_threshold_db") || has("deess_freq_hz")) {
        set(Ids3::deessOn, 1.0f);
        set(Ids3::deessThreshDb, get("deess_threshold_db", -30.0f));
        set(Ids3::deessFreq, get("deess_freq_hz", 6500.0f));
        // The de-esser's depth follows the amount (the threshold does not).
        const float a = std::isfinite(amount) ? std::clamp(amount, 0.0f, 1.0f) : 1.0f;
        set(Ids3::deessRangeDb, 6.0f * a);
        use("deess_threshold_db");
        use("deess_freq_hz");
        out.notes.push_back(fmt("De-esser %.0f Hz, threshold %.1f dB", get("deess_freq_hz", 6500.0f),
                                get("deess_threshold_db", -30.0f)));
    }

    // Gain keys are relative moves, summed and applied on top of base.
    {
        float delta = 0.0f;
        bool any = false;
        if (has("limiter_gain_db")) {
            delta += std::abs(get("limiter_gain_db"));
            any = true;
        }
        if (has("comp_makeup_gain_db")) {
            delta += get("comp_makeup_gain_db");
            any = true;
        }
        if (has("reduce_gain_db")) {
            delta -= std::abs(get("reduce_gain_db"));
            any = true;
        }
        if (has("increase_gain_db")) {
            delta += std::abs(get("increase_gain_db"));
            any = true;
        }
        if (any) {
            for (const char* k : {"limiter_gain_db", "comp_makeup_gain_db", "reduce_gain_db", "increase_gain_db"}) use(k);
            set(Ids::outGainDb, baseOf(Ids::outGainDb) + delta);
            out.notes.push_back(fmt("Output gain %+.1f dB", delta));
        }
    }

    if (has("true_peak_ceiling_dbtp")) {
        use("true_peak_ceiling_dbtp");
        set(Ids::limOn, 1.0f);
        set(Ids::limThreshDb, get("true_peak_ceiling_dbtp"));
        out.notes.push_back(fmt("Limiter ceiling %.1f dBTP", get("true_peak_ceiling_dbtp")));
    }

    for (const auto& [key, value] : rec.suggested_parameters) {
        (void)value;
        if (used.count(key) != 0)
            out.mappedKeys.push_back(key);
        else
            out.unmappedKeys.push_back(key);
    }
    return out;
}

} // namespace khensu::mixerchain
