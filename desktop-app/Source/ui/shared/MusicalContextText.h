// desktop-app/Source/ui/shared/MusicalContextText.h
// AUDIT-052: the deep analysis returns meter, felt (dotted-quarter) tempo,
// Camelot code and key/BPM confidence, but the key/BPM displays dropped them.
// One formatter so every panel shows the same detail, and only what the
// service actually returned.
// THREAD_CONTEXT(AnyThread) - pure string formatting.
#pragma once
#include <JuceHeader.h>
#include <khensu/analysis/AnalysisResult.h>

namespace khensu::musical_context {

/// True when time_signature is a real detection - a 4/4 with
/// meter_confidence <= 0.2 is the analyzer's fallback, not a measurement.
inline bool hasDetectedMeter(const AnalysisResult::PythonData& pd)
{
    return !pd.time_signature.empty() && !(pd.time_signature == "4/4" && pd.meter_confidence <= 0.2f);
}

/// "6/8  felt 64.0  87%" - detected meter, dotted-quarter tempo and BPM
/// confidence; "" when none were returned.
inline juce::String tempoDetail(const AnalysisResult::PythonData& pd)
{
    juce::StringArray parts;
    if (hasDetectedMeter(pd)) parts.add(juce::String(pd.time_signature));
    if (pd.felt_bpm > 0.0f) parts.add(TRANS("felt") + " " + juce::String(pd.felt_bpm, 1));
    if (pd.bpm_detected > 0.0f && pd.bpm_confidence > 0.0f)
        parts.add(juce::String(juce::roundToInt(pd.bpm_confidence * 100.0f)) + "%");
    return parts.joinIntoString("  ");
}

/// "8A  78%" - Camelot code and key confidence; "" when none were returned.
inline juce::String keyDetail(const AnalysisResult::PythonData& pd)
{
    juce::StringArray parts;
    if (!pd.camelot.empty()) parts.add(juce::String(pd.camelot));
    if (!pd.pitch_key.empty() && pd.pitch_confidence > 0.0f)
        parts.add(juce::String(juce::roundToInt(pd.pitch_confidence * 100.0f)) + "%");
    return parts.joinIntoString("  ");
}

} // namespace khensu::musical_context
