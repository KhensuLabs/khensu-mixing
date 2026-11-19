#include <khensu/recommendations/SpatialAudioAssessment.h>
#include <khensu/recommendations/StereoRecommender.h>
#include <algorithm>
#include <cmath>
#include <iterator>

namespace khensu {

namespace {

float clamp01to100(float v) { return std::clamp(v, 0.0f, 100.0f); }

} // namespace

SpatialAudioAssessment assessSpatialAudioReadiness(const AnalysisResult& r)
{
    SpatialAudioAssessment a;

    // Mono material has no stereo image to judge (CORE-09): its width
    // criterion is not applicable rather than a 0 that drags the overall
    // score down for every mono stem.
    const bool mono = StereoRecommender::isMonoSource(r);

    // --- Stereo width: wider mixes give an object-based upmixer more room
    // to spread content beyond the L/R bed. ---
    a.stereoWidthScore = clamp01to100(r.stereo.width * 120.0f);

    // --- Mono/LFE compatibility: bass staying centred (low stereo %) is
    // what makes a clean, safe low end / height channel in an upmix; wide
    // bass tends to fold down badly and confuses object placement. ---
    const bool monoCompatMeasured = r.mono_compat.has_value();
    if (monoCompatMeasured) {
        a.monoCompatScore = clamp01to100(100.0f - r.mono_compat->low_end_stereo_pct);
    } else {
        // No deep-analysis pass yet - a neutral placeholder for display only.
        // It is NOT folded into the overall score below (CORE-09): a made-up
        // 60 would move the band for a measurement that never happened.
        a.monoCompatScore = 60.0f;
    }

    // --- Phase safety: anti-phase content breaks any multichannel
    // derivation, not just stereo fold-down. The polarity flag, the
    // anti-phase-energy flag and a negative correlation are three views of
    // ONE condition (CORE-09) - penalise it once, graded by how far below
    // zero the correlation sits, instead of subtracting 50 + 30 + 25. ---
    float phaseScore = 100.0f;
    if (r.phase.has_polarity_issue || r.phase.has_anti_phase_energy ||
        r.stereo.correlation < AnalysisResult::PhaseData::kPhaseIssueCorrelation) {
        const float corr = std::min(r.stereo.correlation, r.phase.overall_correlation);
        phaseScore = 50.0f - 50.0f * std::clamp(-corr, 0.0f, 1.0f);
    }
    a.phaseSafetyScore = clamp01to100(phaseScore);

    // --- Dynamic headroom: a mix that's already slammed to the ceiling has
    // no room left to carve height/surround separation into; DR14 (very
    // dynamic) maps near 100, DR2 (heavily limited) maps near 0. ---
    float drScore = (r.dynamics.dynamic_range_db - 2.0f) / 12.0f * 100.0f;
    if (r.artifacts.has_clipping) drScore -= 20.0f;
    a.dynamicHeadroomScore = clamp01to100(drScore);

    // Equal-weight mean over the criteria that actually apply.
    float sum = a.phaseSafetyScore + a.dynamicHeadroomScore;
    int n = 2;
    if (!mono) {
        sum += a.stereoWidthScore;
        ++n;
    }
    if (monoCompatMeasured) {
        sum += a.monoCompatScore;
        ++n;
    }
    a.overallScore = clamp01to100(sum / static_cast<float>(n));

    if (a.overallScore >= 75.0f)
        a.band = SpatialAudioAssessment::Band::StrongCandidate;
    else if (a.overallScore >= 50.0f)
        a.band = SpatialAudioAssessment::Band::Workable;
    else
        a.band = SpatialAudioAssessment::Band::NotRecommended;

    // Identify the weakest criterion for the plain-language summary.
    struct Factor {
        const char* name;
        float score;
    };
    const Factor factors[] = {
        {"stereo width", a.stereoWidthScore},
        {"bass/LFE centring", a.monoCompatScore},
        {"phase safety", a.phaseSafetyScore},
        {"dynamic headroom", a.dynamicHeadroomScore},
    };
    const bool applicable[] = {!mono, monoCompatMeasured, true, true};
    const Factor* weakest = nullptr;
    for (size_t i = 0; i < std::size(factors); ++i)
        if (applicable[i] && (weakest == nullptr || factors[i].score < weakest->score)) weakest = &factors[i];

    switch (a.band) {
    case SpatialAudioAssessment::Band::StrongCandidate:
        a.summary = "Wide, mono-compatible, and dynamically open enough to translate well "
                    "into an Atmos or other object-based spatial upmix.";
        break;
    case SpatialAudioAssessment::Band::Workable:
        a.summary = std::string("Workable for a spatial/Atmos upmix, but ") + weakest->name +
                    " is the limiting factor - address it before committing to an immersive master.";
        break;
    case SpatialAudioAssessment::Band::NotRecommended:
        a.summary =
            std::string("Not recommended for a spatial/Atmos upmix as-is - ") + weakest->name + " needs work first.";
        break;
    }

    // --- Illustrative virtual object placement (see header: approximate,
    // not a literal Atmos render). ---
    const float bassDrift = r.mono_compat ? (r.mono_compat->low_end_stereo_pct / 100.0f) * 0.35f : 0.0f;
    a.virtualObjects.push_back({"Bass / LFE", bassDrift, 0.0f, -0.85f});
    a.virtualObjects.push_back({"Vocal / Centre", 0.0f, -0.4f, -0.1f});

    const float spread = std::clamp(r.stereo.width, 0.15f, 1.0f);
    a.virtualObjects.push_back({"Wide L", -spread, 0.15f, 0.05f});
    a.virtualObjects.push_back({"Wide R", spread, 0.15f, 0.05f});

    // Brighter mixes (higher spectral centroid) are the more plausible
    // candidate for height-channel content - a rough proxy, not a measured
    // "this track has real height information" claim.
    const float heightZ = std::clamp((r.spectral.centroid_hz - 1500.0f) / 6000.0f, 0.0f, 1.0f) * 0.8f;
    a.virtualObjects.push_back({"Height / Air", 0.0f, 0.3f, 0.2f + heightZ});

    return a;
}

const char* toString(SpatialAudioAssessment::Band band)
{
    switch (band) {
    case SpatialAudioAssessment::Band::StrongCandidate:
        return "Strong candidate";
    case SpatialAudioAssessment::Band::Workable:
        return "Workable";
    case SpatialAudioAssessment::Band::NotRecommended:
        return "Not recommended";
    }
    return "Unknown";
}

} // namespace khensu
