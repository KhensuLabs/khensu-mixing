// desktop-app/Source/ui/shared/FindingEvidence.h
// NEW-2: what each finding was measured to do on real music, beside its
// heuristic / calibrated basis - from resources/calibration/finding_evidence.json
// (scripts/measure_genre_norms.py evidence + report, MEAS-4): how often it
// fires on released tracks graded against their own genre (its false-alarm
// rate on that corpus), and how often it catches an injected defect it names
// on the same tracks. Loaded once; header-only and testable through parse().
#pragma once
#include <JuceHeader.h>
#include "../../BundledResources.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace khensu::finding_evidence {

struct Detection {
    juce::String defect;
    double rate = 0.0;
    int tracks = 0;
};

/// OVH-6: a finding the app's own Fix raised on the corpus where it was
/// not there before (resources/calibration/finding_evidence.json
/// fix.side_effects; absent in older tables).
struct SideEffect {
    juce::String code;
    double rate = 0.0;
    int tracks = 0;
};

struct Entry {
    double falseAlarmRate = 0.0;
    int firedOn = 0;
    int corpusTracks = 0;
    /// EVID-1: the share it fires on at moderate severity or worse (-1 = not
    /// measured). A minor note is not an alarm.
    double alarmRate = -1.0;
    /// EVID-2: a delivery-spec check - its corpus rate is the share of
    /// released tracks that miss the spec, not a false alarm.
    bool spec = false;
    std::vector<Detection> detects;
    /// MEAS-4: of the defects this finding caught, the share the app's own
    /// Fix cleared (-1 = not measured); `adviceOnly` when it has no render.
    double fixClearsRate = -1.0;
    int fixTried = 0;
    bool adviceOnly = false;
    /// OVH-6: what the Fix introduced, most frequent first (may be empty).
    std::vector<SideEffect> sideEffects;
};

inline std::map<juce::String, Entry> parse(const juce::var& doc)
{
    std::map<juce::String, Entry> out;
    const int tracks = (int)doc["tracks"];
    if (auto* obj = doc["findings"].getDynamicObject())
        for (const auto& p : obj->getProperties()) {
            Entry e;
            e.falseAlarmRate = (double)p.value["false_alarm_rate"];
            e.firedOn = (int)p.value["fired_on"];
            // FULL-1: a finding measured through the service names its own count.
            e.corpusTracks = p.value.hasProperty("tracks") ? (int)p.value["tracks"] : tracks;
            if (p.value.hasProperty("alarm_rate")) e.alarmRate = (double)p.value["alarm_rate"];
            e.spec = p.value["kind"].toString() == "spec";
            if (auto* arr = p.value["detects"].getArray())
                for (const auto& d : *arr)
                    e.detects.push_back({d["defect"].toString(), (double)d["rate"], (int)d["tracks"]});
            const auto fix = p.value["fix"];
            if (fix.getDynamicObject() != nullptr) {
                e.fixTried = (int)fix["tried"];
                e.adviceOnly = (bool)fix["advice_only"];
                if (e.fixTried > 0) e.fixClearsRate = (double)fix["clears_rate"];
                if (auto* effects = fix["side_effects"].getArray())
                    for (const auto& se : *effects) {
                        const auto code = se["code"].toString();
                        if (code.isEmpty() || (double)se["rate"] <= 0.0) continue;
                        e.sideEffects.push_back({code, (double)se["rate"], (int)se["tracks"]});
                    }
                std::stable_sort(e.sideEffects.begin(), e.sideEffects.end(),
                                 [](const SideEffect& a, const SideEffect& b) { return a.rate > b.rate; });
            }
            out[p.name.toString()] = std::move(e);
        }
    return out;
}

inline const std::map<juce::String, Entry>& table()
{
    static std::once_flag once;
    static std::map<juce::String, Entry> entries;
    std::call_once(once, [] {
        const auto f = bundled_resources::find("resources/calibration/finding_evidence.json");
        if (f.existsAsFile()) entries = parse(juce::JSON::parse(f));
    });
    return entries;
}

/// "fires on 12% of 1,140 released tracks; catches a +10 dB low shelf on 84%"
/// or empty when the finding was not measured.
inline juce::String describe(const Entry& e)
{
    juce::String s;
    if (e.spec)
        s = TRANS("A delivery-spec check: %p% of %n released tracks miss it")
                .replace("%p", juce::String(e.falseAlarmRate * 100.0, 1))
                .replace("%n", juce::String(e.corpusTracks));
    else
        s = TRANS("Tested on released music: fires on %p% of %n tracks graded against their own genre")
                .replace("%p", juce::String(e.falseAlarmRate * 100.0, 1))
                .replace("%n", juce::String(e.corpusTracks));
    if (e.alarmRate >= 0.0 && e.alarmRate < e.falseAlarmRate)
        s << " " << TRANS("(%a% at moderate or worse; the rest are minor notes)")
                        .replace("%a", juce::String(e.alarmRate * 100.0, 1));
    for (const auto& d : e.detects)
        s << "; " << TRANS("catches a %d on %r% of %t tracks")
                         .replace("%d", d.defect.replace("_", " "))
                         .replace("%r", juce::String(juce::roundToInt(d.rate * 100.0)))
                         .replace("%t", juce::String(d.tracks));
    if (e.fixClearsRate >= 0.0)
        s << "; " << TRANS("the app's Fix cleared it on %r% of %t")
                         .replace("%r", juce::String(juce::roundToInt(e.fixClearsRate * 100.0)))
                         .replace("%t", juce::String(e.fixTried));
    else if (e.adviceOnly)
        s << "; " << TRANS("advice only - the fix is made at the source");
    return s + ".";
}

inline juce::String describe(const juce::String& issueCode)
{
    const auto& t = table();
    const auto it = t.find(issueCode);
    return it == t.end() ? juce::String() : describe(it->second);
}

/// "low_end_light" -> "Low end light": an issue code as a reader sees it.
inline juce::String readableCode(const juce::String& code)
{
    // Units and abbreviations keep their capitals ("lufs_too_quiet" ->
    // "LUFS too quiet", "dc_offset" -> "DC offset").
    static const std::pair<const char*, const char*> kAcronyms[] = {
        {"lufs", "LUFS"}, {"lra", "LRA"}, {"dc", "DC"}, {"ms", "M/S"}, {"eq", "EQ"}, {"ai", "AI"}};
    juce::StringArray words = juce::StringArray::fromTokens(code.replaceCharacter('_', ' ').trim(), " ", "");
    for (auto& w : words)
        for (const auto& [from, to] : kAcronyms)
            if (w == from) w = to;
    auto out = words.joinIntoString(" ");
    return out.isEmpty() ? out : out.substring(0, 1).toUpperCase() + out.substring(1);
}

/// OVH-6: one line that predicts the Fix before it renders, from what the
/// app's own Fix did on released tracks with the defect injected:
/// "Fix clears this on 78% of 100 released tracks tested; may introduce:
/// Low end light (12%)". Empty when the finding's fix was never measured.
inline juce::String predictFix(const Entry& e)
{
    juce::String s;
    if (e.fixClearsRate >= 0.0 && e.fixTried > 0)
        s = TRANS("Fix clears this on %r% of %t released tracks tested")
                .replace("%r", juce::String(juce::roundToInt(e.fixClearsRate * 100.0)))
                .replace("%t", juce::String(e.fixTried));
    else if (e.adviceOnly)
        return TRANS("Advice only on released tracks - the fix is made at the source");
    else
        return {};
    juce::StringArray effects;
    for (const auto& se : e.sideEffects) {
        if (effects.size() >= 2) break;
        effects.add(readableCode(se.code) + " (" + juce::String(juce::roundToInt(se.rate * 100.0)) + "%)");
    }
    if (!effects.isEmpty()) s << "; " << TRANS("may introduce:") << " " << effects.joinIntoString(", ");
    return s;
}

inline juce::String predictFix(const juce::String& issueCode)
{
    const auto& t = table();
    const auto it = t.find(issueCode);
    return it == t.end() ? juce::String() : predictFix(it->second);
}

} // namespace khensu::finding_evidence
