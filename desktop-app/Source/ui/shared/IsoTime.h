#pragma once
#include <JuceHeader.h>

namespace khensu {

/// "HH:MM" in the user's local time for a service timestamp. The service
/// writes UTC ISO-8601 with microseconds ("2026-10-16T12:34:56.123456+00:00",
/// events/db.py); slicing the string showed UTC. Fractional seconds are
/// dropped before parsing (juce::Time::fromISO8601 reads at most three
/// digits). Falls back to the raw clock digits if the string won't parse.
inline juce::String localHourMinute(const juce::String& iso)
{
    const int tIdx = iso.indexOfChar('T');
    if (tIdx < 0) return iso;
    juce::String s = iso;
    const int dot = s.indexOfChar(tIdx, '.');
    if (dot > 0) {
        int end = dot + 1;
        while (end < s.length() && juce::CharacterFunctions::isDigit(s[end]))
            ++end;
        s = s.substring(0, dot) + s.substring(end);
    }
    const auto t = juce::Time::fromISO8601(s);
    if (t.toMilliseconds() == 0) return iso.substring(tIdx + 1, tIdx + 6);
    return t.formatted("%H:%M");
}

} // namespace khensu
