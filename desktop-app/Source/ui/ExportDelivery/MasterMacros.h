// desktop-app/Source/ui/ExportDelivery/MasterMacros.h
// FEAT-2: the Master Assistant's five macros over the mastering agent's
// chain - Loudness (gain into the limiter), Tone (how much of the chain's EQ
// moves), Width, Punch (how hard the compression and transient stages work)
// and Ceiling (the limiter's true-peak ceiling). Neutral macros return the
// chain unchanged except the ceiling, which every master sets.
// THREAD_CONTEXT(AnyThread) - pure.
#pragma once
#include <khensu/ipc/HttpServiceBridge.h>
#include <algorithm>
#include <vector>

namespace khensu::master_macros {

using Effect = ipc::HttpServiceBridge::EffectSpec;
using Chain = std::vector<Effect>;

struct Macros {
    float loudnessDb = 0.0f;   ///< -6..+6: gain into the limiter
    float tone = 1.0f;         ///< 0..1.5: scales every EQ stage's gain
    float widthPct = 0.0f;     ///< -50..+50: the stereo width, relative
    float punch = 1.0f;        ///< 0..1.5: scales compression ratio above 1:1 and transient attack
    float ceilingDbtp = -1.0f; ///< -3..-0.1: the limiter's true-peak ceiling

    bool operator==(const Macros& o) const noexcept
    {
        return loudnessDb == o.loudnessDb && tone == o.tone && widthPct == o.widthPct && punch == o.punch &&
               ceilingDbtp == o.ceilingDbtp;
    }
};

inline bool isEq(const juce::String& e) { return e == "PeakFilter" || e == "HighShelfFilter" || e == "LowShelfFilter"; }

/// The chain with the macros applied (see the file comment).
inline Chain apply(const Chain& base, const Macros& m)
{
    Chain out;
    out.reserve(base.size() + 3);
    for (auto fx : base) {
        auto& p = fx.parameters;
        if (isEq(fx.effect) && p.count("gain_db") != 0) {
            p["gain_db"] = std::clamp(p["gain_db"] * m.tone, -24.0f, 24.0f);
        } else if ((fx.effect == "Compressor" || fx.effect == "GlueCompressor") && p.count("ratio") != 0) {
            const float maxRatio = fx.effect == "Compressor" ? 30.0f : 20.0f;
            p["ratio"] = std::clamp(1.0f + (p["ratio"] - 1.0f) * m.punch, 1.0f, maxRatio);
        } else if (fx.effect == "TransientShaper" && p.count("attack_gain_db") != 0) {
            p["attack_gain_db"] = std::clamp(p["attack_gain_db"] * m.punch, -24.0f, 24.0f);
        } else if (fx.effect == "StereoWidener") {
            const float w = p.count("width") != 0 ? p["width"] : 1.0f;
            p["width"] = std::clamp(w * (1.0f + m.widthPct / 100.0f), 0.0f, 2.0f);
        }
        out.push_back(std::move(fx));
    }

    // Width with no widener in the chain: one before the limiter.
    const bool hasWidener =
        std::any_of(out.begin(), out.end(), [](const Effect& e) { return e.effect == "StereoWidener"; });
    auto limiterAt = [&out] {
        return std::find_if(out.begin(), out.end(), [](const Effect& e) { return e.effect == "Limiter"; });
    };
    if (!hasWidener && m.widthPct != 0.0f)
        out.insert(limiterAt(),
                   Effect{"StereoWidener", {{"width", std::clamp(1.0f + m.widthPct / 100.0f, 0.0f, 2.0f)}}});

    // Every master ends in the limiter at the ceiling.
    auto lim = limiterAt();
    if (lim == out.end()) {
        out.push_back(Effect{"Limiter", {{"threshold_db", 0.0f}, {"release_ms", 100.0f}}});
        lim = std::prev(out.end());
    }
    lim->parameters["threshold_db"] = std::clamp(m.ceilingDbtp, -30.0f, 0.0f);

    // Loudness: gain into the limiter (a Gain stage just before it).
    if (m.loudnessDb != 0.0f) {
        if (lim != out.begin() && std::prev(lim)->effect == "Gain") {
            auto& g = std::prev(lim)->parameters["gain_db"];
            g = std::clamp(g + m.loudnessDb, -36.0f, 24.0f);
        } else {
            out.insert(lim, Effect{"Gain", {{"gain_db", std::clamp(m.loudnessDb, -36.0f, 24.0f)}}});
        }
    }
    return out;
}

// ---- ENH-5: mastering styles and saved macro sets ---------------------------

/// The mastering agent's style (the service's `style`): Balanced is the
/// genre's own targets, exactly the agent before styles existed.
enum class Style { Warm = 0, Balanced, Open, Loud };

inline const char* styleId(Style s) noexcept
{
    switch (s) {
    case Style::Warm: return "warm";
    case Style::Open: return "open";
    case Style::Loud: return "loud";
    case Style::Balanced: break;
    }
    return "balanced";
}

inline Style styleFromId(const juce::String& id) noexcept
{
    if (id == "warm") return Style::Warm;
    if (id == "open") return Style::Open;
    if (id == "loud") return Style::Loud;
    return Style::Balanced;
}

/// A named set: the style, its strength (0..1) and the five macros.
struct MacroSet {
    juce::String name;
    Style style = Style::Balanced;
    float strength = 0.5f;
    Macros macros;
};

/// The preference key the saved sets live under (one JSON object, so a
/// delete is a rewrite and needs no store API of its own).
inline constexpr const char* kSetsPreferenceKey = "master_macro_sets";

inline juce::var toVar(const MacroSet& s)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("style", juce::String(styleId(s.style)));
    o->setProperty("strength", s.strength);
    o->setProperty("loudness_db", s.macros.loudnessDb);
    o->setProperty("tone", s.macros.tone);
    o->setProperty("width_pct", s.macros.widthPct);
    o->setProperty("punch", s.macros.punch);
    o->setProperty("ceiling_dbtp", s.macros.ceilingDbtp);
    return juce::var(o);
}

inline MacroSet fromVar(const juce::String& name, const juce::var& v)
{
    MacroSet s;
    s.name = name;
    auto num = [&v](const char* key, float def, float lo, float hi) {
        const auto x = v[key];
        const float f = (x.isDouble() || x.isInt() || x.isInt64()) ? static_cast<float>(static_cast<double>(x)) : def;
        return std::clamp(f, lo, hi);
    };
    s.style = styleFromId(v["style"].toString());
    s.strength = num("strength", 0.5f, 0.0f, 1.0f);
    s.macros.loudnessDb = num("loudness_db", 0.0f, -6.0f, 6.0f);
    s.macros.tone = num("tone", 1.0f, 0.0f, 1.5f);
    s.macros.widthPct = num("width_pct", 0.0f, -50.0f, 50.0f);
    s.macros.punch = num("punch", 1.0f, 0.0f, 1.5f);
    s.macros.ceilingDbtp = num("ceiling_dbtp", -1.0f, -3.0f, -0.1f);
    return s;
}

/// Every saved set in `json` (the preference value), sorted by name.
inline std::vector<MacroSet> parseSets(const juce::String& json)
{
    std::vector<MacroSet> out;
    const auto v = juce::JSON::parse(json);
    if (auto* obj = v.getDynamicObject())
        for (const auto& p : obj->getProperties())
            if (p.name.toString().trim().isNotEmpty()) out.push_back(fromVar(p.name.toString(), p.value));
    std::sort(out.begin(), out.end(),
              [](const MacroSet& a, const MacroSet& b) { return a.name.compareIgnoreCase(b.name) < 0; });
    return out;
}

inline juce::String serializeSets(const std::vector<MacroSet>& sets)
{
    auto* o = new juce::DynamicObject();
    for (const auto& s : sets)
        if (s.name.trim().isNotEmpty()) o->setProperty(s.name.trim(), toVar(s));
    return juce::JSON::toString(juce::var(o), true);
}

/// `sets` with `set` added, replacing one of the same name (case-insensitive).
inline std::vector<MacroSet> withSet(std::vector<MacroSet> sets, MacroSet set)
{
    set.name = set.name.trim();
    sets.erase(std::remove_if(sets.begin(), sets.end(),
                              [&set](const MacroSet& s) { return s.name.equalsIgnoreCase(set.name); }),
               sets.end());
    sets.push_back(std::move(set));
    std::sort(sets.begin(), sets.end(),
              [](const MacroSet& a, const MacroSet& b) { return a.name.compareIgnoreCase(b.name) < 0; });
    return sets;
}

inline std::vector<MacroSet> withoutSet(std::vector<MacroSet> sets, const juce::String& name)
{
    sets.erase(std::remove_if(sets.begin(), sets.end(),
                              [&name](const MacroSet& s) { return s.name.equalsIgnoreCase(name.trim()); }),
               sets.end());
    return sets;
}

} // namespace khensu::master_macros
