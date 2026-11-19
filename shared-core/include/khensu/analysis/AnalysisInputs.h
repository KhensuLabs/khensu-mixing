#pragma once
#include <cstdint>
#include <string>

/// Which optional inputs an analysis had (REF-1).
///
/// A finding is only raised when its recommender has the data it reads: no
/// deep pass means no hook, memorability, structure or heaviness findings; no
/// drum scan means no kick/bass collision. So "the finding is absent" has two
/// meanings - measured and fine, or not measured - and the fix check, the
/// results gate and service recovery need to tell them apart.
///
/// - AnalysisResult::inputs_available() is derived from the result's own
///   fields, so it cannot drift from them and needs no persistence.
/// - AnalysisResult::inputs_failed records inputs that were asked for and not
///   obtained (the engine did not answer, or a request failed).
/// - Recommendation::requires_inputs is set by RecommendationEngine::generate
///   to the inputs the finding was derived from.
///
/// THREAD_CONTEXT(AnyThread) - constants and pure functions.
namespace khensu::analysis_inputs {

using Mask = std::uint32_t;

inline constexpr Mask kNone = 0;
inline constexpr Mask kDeep = 1u << 0;            ///< the service's deep pass (python_data: hooks, memorability, ...)
inline constexpr Mask kHarshness = 1u << 1;       ///< AnalysisResult::harshness
inline constexpr Mask kMonoCompat = 1u << 2;      ///< AnalysisResult::mono_compat
inline constexpr Mask kTranslationRisk = 1u << 3; ///< AnalysisResult::translation_risk
inline constexpr Mask kResonance = 1u << 4;       ///< AnalysisResult::resonance
inline constexpr Mask kMidSide = 1u << 5;         ///< AnalysisResult::mid_side_imbalance
inline constexpr Mask kSections = 1u << 6;        ///< AnalysisResult::section_analysis
inline constexpr Mask kDrums = 1u << 7;           ///< AnalysisResult::drums
inline constexpr Mask kMasking = 1u << 8;         ///< AnalysisResult::masking (on demand, needs stems)
inline constexpr Mask kReference = 1u << 9;       ///< AnalysisResult::reference_deltas
inline constexpr Mask kChords = 1u << 10;         ///< AnalysisResult::chord_analysis
inline constexpr Mask kTrust = 1u << 11;          ///< AnalysisResult::measurement_trust

/// Everything the deep response carries: one failed deep pass loses them all.
inline constexpr Mask kDeepResponse = kDeep | kHarshness | kMonoCompat | kTranslationRisk | kResonance | kMidSide;

/// What one analysis run asks the engine for: the deep response and the four
/// follow-up requests, plus the reference comparison when a reference is set.
/// Stem masking is on demand, so it is never "asked for and not obtained".
constexpr Mask serviceInputs(bool withReference) noexcept
{
    return kDeepResponse | kSections | kDrums | kChords | kTrust | (withReference ? kReference : kNone);
}

/// Plain-English list for a log line or a status: "deep analysis, drums".
inline std::string describe(Mask mask)
{
    struct Name {
        Mask bit;
        const char* text;
    };
    static constexpr Name kNames[] = {
        {kDeep, "deep analysis"},      {kHarshness, "harshness"},  {kMonoCompat, "mono compatibility"},
        {kTranslationRisk, "translation risk"}, {kResonance, "resonances"}, {kMidSide, "mid/side balance"},
        {kSections, "sections"},       {kDrums, "drums"},          {kMasking, "stem masking"},
        {kReference, "reference"},     {kChords, "chords"},        {kTrust, "measurement quality"},
    };
    std::string out;
    for (const auto& n : kNames) {
        if ((mask & n.bit) == 0) continue;
        if (!out.empty()) out += ", ";
        out += n.text;
    }
    return out;
}

} // namespace khensu::analysis_inputs
