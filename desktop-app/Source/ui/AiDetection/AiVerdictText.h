// desktop-app/Source/ui/AiDetection/AiVerdictText.h
#pragma once

// Wording for an /analyze/artifacts_scan result, shared by the AI Detection
// screen and the Dashboard's AI Artifacts panel (audit 2026-11-06 AID-05 /
// UIX-09). The fused number is an uncalibrated heuristic signal score, so:
//   * it is labelled as such unless the service says `calibrated: true`;
//   * "likely AI-generated" is reserved for provenance evidence (metadata
//     naming a generator, a decoded watermark, a validated model) - the
//     service's `provenance_found`;
//   * heuristic findings read "AI-typical artifacts found"; no findings read
//     "no provenance found - not proof of human-made" (never "no AI"): AI
//     audio without a tag or watermark passed every detector on the
//     2026-11-18 measurement (0 of 300 generated clips flagged), so a clean
//     scan only means no provenance was found, and it is not coloured as a pass;
//   * a checklist row reads "resembles X (score)" unless provenance names X;
//   * detectors that did not run (or errored) are listed, and truncated
//     lists say "+N more".

#include <JuceHeader.h>
#include <khensu/ipc/generated/ApiSchema.h>
#include <cmath>

namespace khensu::ai_verdict_text {

enum class Tone { Provenance, Heuristic, Clear };

struct Headline {
    juce::String text;
    Tone tone = Tone::Clear;
};

inline bool provenanceFound(const juce::var& scan)
{
    return static_cast<bool>(scan[api::fields::ArtifactScanResponse::provenance_found]);
}

inline bool calibrated(const juce::var& scan)
{
    return static_cast<bool>(scan[api::fields::ArtifactScanResponse::calibrated]);
}

/// The verdict banner. "LIKELY AI-GENERATED" only with provenance evidence.
inline Headline headline(const juce::var& scan)
{
    namespace af = api::fields::ArtifactScanResponse;
    const juce::String verdict = scan[af::verdict].toString();
    const bool provenance = provenanceFound(scan);
    if (verdict == "likely_ai" && provenance)
        return {TRANS("LIKELY AI-GENERATED (provenance evidence)"), Tone::Provenance};
    if (provenance)
        return {TRANS("PROVENANCE MARKER FOUND"), Tone::Heuristic};
    if (verdict == "likely_ai" || verdict == "possible_ai")
        return {TRANS("AI-TYPICAL ARTIFACTS FOUND"), Tone::Heuristic};
    return {TRANS("NO PROVENANCE FOUND - NOT PROOF OF HUMAN-MADE"), Tone::Clear};
}

/// "heuristic signal score 42% (uncalibrated)" (or "score 42%" when the
/// service ever reports a calibrated number).
inline juce::String scoreText(const juce::var& scan)
{
    const double conf = static_cast<double>(scan[api::fields::ArtifactScanResponse::confidence]);
    const juce::String pct = juce::String(static_cast<int>(std::lround(conf * 100.0))) + "%";
    if (calibrated(scan))
        return TRANS("score") + " " + pct;
    return TRANS("heuristic signal score") + " " + pct + " " + TRANS("(uncalibrated)");
}

/// One service-checklist row: provenance names it, or it only "resembles".
inline juce::String checklistLine(const juce::var& row)
{
    namespace sc = api::fields::ArtifactServiceCheck;
    const juce::String label = row[sc::label].toString();
    const double likelihood = static_cast<double>(row[sc::likelihood]);
    const juce::String pct = juce::String(static_cast<int>(std::lround(likelihood * 100.0))) + "%";
    const bool detected = static_cast<bool>(row[sc::detected]);
    if (detected && row[sc::basis].toString() != "heuristic")
        return label + ": " + TRANS("named by provenance") + " (" + pct + ")";
    if (likelihood >= 0.05)
        return TRANS("resembles") + " " + label + " (" + pct + ")";
    return label + " (" + pct + ")";
}

/// True when the row's likelihood comes from provenance evidence.
inline bool checklistFromProvenance(const juce::var& row)
{
    namespace sc = api::fields::ArtifactServiceCheck;
    return static_cast<bool>(row[sc::detected]) && row[sc::basis].toString() != "heuristic";
}

/// "+N more" for N hidden items.
inline juce::String moreSuffixCount(int hidden)
{
    return "+" + juce::String(hidden) + " " + TRANS("more");
}

/// "Not run: wavmark_watermark (model weights not pre-cached); ..." plus
/// "Errored: ..." - empty when every detector ran.
inline juce::String notRunLine(const juce::var& scan, int maxItems = 4)
{
    namespace af = api::fields::ArtifactScanResponse;
    namespace nr = api::fields::ArtifactDetectorNotRun;
    juce::StringArray items;
    int total = 0;
    if (auto* arr = scan[af::detectors_not_run].getArray())
        for (const auto& d : *arr) {
            ++total;
            if (items.size() < maxItems)
                items.add(d[nr::id].toString() + " (" + d[nr::reason].toString() + ")");
        }
    if (total == 0) return {};
    juce::String line = TRANS("Not run") + ": " + items.joinIntoString("; ");
    if (total > items.size())
        line << " " << moreSuffixCount(total - items.size());
    return line;
}

/// "+N more" for a list that showed `shown` of `total` items; empty if none hidden.
inline juce::String moreSuffix(int total, int shown)
{
    const int hidden = total - shown;
    return hidden > 0 ? moreSuffixCount(hidden) : juce::String();
}

/// Short tag for a finding's evidence class (S1).
inline juce::String classTag(const juce::String& evidenceClass)
{
    if (evidenceClass == "provenance") return TRANS("provenance");
    if (evidenceClass == "observation") return TRANS("observation (no weight)");
    return TRANS("heuristic");
}

} // namespace khensu::ai_verdict_text
