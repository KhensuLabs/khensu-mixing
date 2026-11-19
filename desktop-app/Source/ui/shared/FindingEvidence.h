// desktop-app/Source/ui/shared/FindingEvidence.h
// NEW-2: what each finding was measured to do on real music, beside its
// heuristic / calibrated basis - from resources/calibration/finding_evidence.json
// (scripts/measure_genre_norms.py evidence + report, MEAS-4): how often it
// fires on released tracks graded against their own genre (its false-alarm
// rate on that corpus), and how often it catches an injected defect it names
// on the same tracks. Loaded once; header-only and testable through parse().
#pragma once
#include <JuceHeader.h>
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

struct Entry {
    double falseAlarmRate = 0.0;
    int firedOn = 0;
    int corpusTracks = 0;
    std::vector<Detection> detects;
    /// MEAS-4: of the defects this finding caught, the share the app's own
    /// Fix cleared (-1 = not measured); `adviceOnly` when it has no render.
    double fixClearsRate = -1.0;
    int fixTried = 0;
    bool adviceOnly = false;
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
            e.corpusTracks = tracks;
            if (auto* arr = p.value["detects"].getArray())
                for (const auto& d : *arr)
                    e.detects.push_back({d["defect"].toString(), (double)d["rate"], (int)d["tracks"]});
            const auto fix = p.value["fix"];
            if (fix.getDynamicObject() != nullptr) {
                e.fixTried = (int)fix["tried"];
                e.adviceOnly = (bool)fix["advice_only"];
                if (e.fixTried > 0) e.fixClearsRate = (double)fix["clears_rate"];
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
        const auto exeDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
        auto f = exeDir.getChildFile("resources/calibration/finding_evidence.json");
        if (!f.existsAsFile())
            f = juce::File::getCurrentWorkingDirectory().getChildFile("resources/calibration/finding_evidence.json");
        if (f.existsAsFile()) entries = parse(juce::JSON::parse(f));
    });
    return entries;
}

/// "fires on 12% of 1,140 released tracks; catches a +10 dB low shelf on 84%"
/// or empty when the finding was not measured.
inline juce::String describe(const Entry& e)
{
    juce::String s = TRANS("Tested on released music: fires on %p% of %n tracks graded against their own genre")
                         .replace("%p", juce::String(e.falseAlarmRate * 100.0, 1))
                         .replace("%n", juce::String(e.corpusTracks));
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

} // namespace khensu::finding_evidence
