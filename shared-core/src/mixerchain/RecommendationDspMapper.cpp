#include <khensu/mixerchain/RecommendationDspMapper.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace khensu::mixerchain {

namespace {
using Ids = DspParamIds;
using Ids3 = DspParamIdsV3;
using Ids5 = DspParamIdsV5;

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
    // STU-10 (state v5): the stages; the EQ bands' ranges are in bandRange().
    {Ids5::tsAttackDb, -24.0f, 24.0f},     {Ids5::tsSustainDb, -24.0f, 24.0f},
    {Ids5::tsAttackMs, 1.0f, 50.0f},       {Ids5::tsReleaseMs, 20.0f, 1000.0f},
    {Ids5::satDriveDb, 0.0f, 24.0f},       {Ids5::satWarmth, 0.0f, 1.0f},
    {Ids5::satMix, 0.0f, 1.0f},            {Ids5::satTrimDb, -24.0f, 6.0f},
    {Ids5::bassMonoFreq, 40.0f, 300.0f},   {Ids5::widthAmount, 0.0f, 2.0f},
};

/// Every band's parameter ranges (bands 1-3's freq / gain / q / type are
/// also in kRanges, with the same values).
bool bandRange(const std::string& id, float& lo, float& hi)
{
    for (const auto& b : kEqBandParamIds) {
        const auto set = [&](float l, float h) {
            lo = l;
            hi = h;
            return true;
        };
        if (id == b.freq) return set(60.0f, 16000.0f);
        if (id == b.gain) return set(-12.0f, 12.0f);
        if (id == b.q) return set(0.3f, 8.0f);
        if (id == b.type) return set(0.0f, 2.0f);
        if (id == b.kind) return set(0.0f, 3.0f);
        if (id == b.mode) return set(0.0f, 2.0f);
        if (id == b.dynThreshDb) return set(-60.0f, 0.0f);
        if (id == b.dynRangeDb) return set(0.0f, 24.0f);
        if (id == b.dynAttackMs) return set(0.1f, 200.0f);
        if (id == b.dynReleaseMs) return set(5.0f, 2000.0f);
    }
    return false;
}

std::string fmt(const char* pattern, double a, double b = 0.0, double c = 0.0)
{
    char buf[160];
    std::snprintf(buf, sizeof(buf), pattern, a, b, c);
    return buf;
}

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

/// The finding's evidence item named `measurement`, if any.
const EvidenceItem* evidenceFor(const Recommendation& rec, const char* measurement)
{
    for (const auto& e : rec.evidence)
        if (e.measurement == measurement) return &e;
    return nullptr;
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
    return bandRange(paramId, lo, hi);
}

float sideGainForWidth(float measuredWidth, float targetWidth)
{
    const auto valid = [](float w, float hi) { return std::isfinite(w) && w > 0.0f && w < hi; };
    // The measured width may be the unclamped one (up to 2, STU-16).
    if (!valid(measuredWidth, 2.0f) || !valid(targetWidth, 1.0f)) return 1.0f;
    // width = 2r / (1 + r) with r = S^2 / M^2, so r = w / (2 - w); a side
    // gain g scales r by g^2.
    const double r = measuredWidth / (2.0 - measuredWidth);
    const double rt = targetWidth / (2.0 - targetWidth);
    return static_cast<float>(std::sqrt(rt / r));
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
    std::vector<const EqBandParamIds*> freeBands;
    for (const auto& band : kEqBandParamIds)
        if (baseOf(band.on) < 0.5f) freeBands.push_back(&band);
    size_t nextBand = 0;
    // A band it takes is reset to a plain stereo, static band of `shape`
    // (whatever kind / mode / dynamics it was left with while off).
    auto placeBand = [&](float freq, float gainDb, float q, EqBandShape shape, const char* what) -> const EqBandParamIds* {
        if (nextBand >= freeBands.size()) return nullptr;
        const EqBandParamIds& band = *freeBands[nextBand++];
        set(band.on, 1.0f);
        set(band.freq, freq);
        set(band.gain, gainDb);
        set(band.q, q);
        set(band.type, static_cast<float>(static_cast<int>(shape)));
        set(band.kind, static_cast<float>(static_cast<int>(EqBandKind::Shape)));
        set(band.mode, static_cast<float>(static_cast<int>(EqBandChannels::Stereo)));
        set(band.dynOn, 0.0f);
        out.notes.push_back("EQ band " + std::to_string(band.number) + ": " + what
                            + fmt(" %.0f Hz %+.1f dB", freq, gainDb));
        return &band;
    };

    // The stem-masking plan (MaskingChain.h): a dynamic band, self-keyed.
    const bool maskingPlan = has("dyneq_freq_hz");
    if (maskingPlan) {
        const float maxCut = std::abs(get("dyneq_max_cut_db", 4.0f)); // "_db": scaled by the amount
        const float threshold = get("dyneq_threshold_db", -30.0f);
        const float q = has("dyneq_q") ? get("dyneq_q") : 2.0f;
        if (const auto* band = placeBand(get("dyneq_freq_hz"), 0.0f, q, EqBandShape::Bell, "dynamic cut")) {
            set(band->dynOn, 1.0f);
            set(band->dynThreshDb, threshold);
            set(band->dynRangeDb, maxCut);
            set(band->dynAttackMs, 5.0f);
            set(band->dynReleaseMs, 120.0f);
            out.notes.back() += fmt(" (up to -%.1f dB over %.0f dB, keyed on this track)", maxCut, threshold);
            for (const char* k : {"dyneq_freq_hz", "dyneq_q", "dyneq_threshold_db", "dyneq_max_cut_db",
                                  "dyneq_target_stem", "eq_freq_hz", "eq_gain_db", "eq_q"})
                use(k);
        }
    }

    if (!maskingPlan && has("eq_freq_hz") && has("eq_gain_db")) {
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

    // STU-10: the stages the plugin gained in state v5.
    if (has("transient_attack_db")) {
        use("transient_attack_db");
        set(Ids5::tsOn, 1.0f);
        set(Ids5::tsAttackDb, get("transient_attack_db"));
        set(Ids5::tsSustainDb, 0.0f);
        set(Ids5::tsAttackMs, 3.0f);
        set(Ids5::tsReleaseMs, 200.0f);
        out.notes.push_back(fmt("Transient shaper: attack %+.1f dB", get("transient_attack_db")));
    }
    if (has("saturation_drive_db")) {
        use("saturation_drive_db");
        set(Ids5::satOn, 1.0f);
        set(Ids5::satDriveDb, get("saturation_drive_db"));
        set(Ids5::satWarmth, 0.5f);
        set(Ids5::satMix, 1.0f);
        set(Ids5::satTrimDb, 0.0f);
        out.notes.push_back(fmt("Tape saturation: drive %.1f dB", get("saturation_drive_db")));
    }

    const std::string code = issueCodeOf(rec);
    {
        float width = 0.0f;
        bool widthFix = false;
        if (has("stereo_width")) {
            use("stereo_width");
            width = get("stereo_width"); // already moved towards 1 by the amount
            widthFix = true;
        } else if (startsWith(code, "stereo_too_wide") || startsWith(code, "stereo_too_narrow")) {
            // No parameters: solve the side gain from the finding's own
            // measurement and the genre edge it crossed.
            const auto* e = evidenceFor(rec, "Stereo width");
            if (e != nullptr && e->threshold.has_value()) {
                const bool tooWide = startsWith(code, "stereo_too_wide");
                const float aim = *e->threshold + (tooWide ? -kWidthAimInside : kWidthAimInside);
                float measured = e->value;
                // STU-16: a reading clamped at 1 (side as loud as the mid or
                // louder) says nothing about how far over it is; the M/S
                // balance b gives the unclamped width 2S^2/(M^2+S^2) = 1 - b.
                if (const auto* b = evidenceFor(rec, "Mid/side balance");
                    b != nullptr && measured >= kWidthClampedReading && std::isfinite(b->value))
                    measured = std::max(measured, 1.0f - b->value);
                const float g = sideGainForWidth(measured, aim);
                const float a = std::isfinite(amount) ? std::clamp(amount, 0.0f, 1.0f) : 1.0f;
                width = 1.0f + (g - 1.0f) * a;
                widthFix = std::abs(g - 1.0f) > 1.0e-3f;
            }
        }
        if (widthFix) {
            set(Ids5::widthOn, 1.0f);
            set(Ids5::widthAmount, width);
            out.notes.push_back(fmt("Stereo width x%.2f", width));
        }
    }
    if (has("bass_mono_hz") || startsWith(code, "sub_bass_too_wide")) {
        use("bass_mono_hz");
        const float hz = get("bass_mono_hz", kSubBassMonoHz);
        set(Ids5::bassMonoOn, 1.0f);
        set(Ids5::bassMonoFreq, hz);
        out.notes.push_back(fmt("Bass mono below %.0f Hz", hz));
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
