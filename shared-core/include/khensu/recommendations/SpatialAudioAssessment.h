#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <string>
#include <vector>

namespace khensu {

/// A stereo mix cannot literally "be" Dolby Atmos or another object-based
/// format - those require dedicated multichannel/object mastering this app
/// never receives. What IS honestly assessable from a stereo file is how
/// well it would *translate* into an immersive/spatial upmix: wide,
/// mono-compatible, dynamically open mixes with a centred low end upmix
/// cleanly; narrow, over-limited, phase-damaged ones don't. This struct is
/// that readiness assessment, not an Atmos certification.
struct SpatialAudioAssessment {
    enum class Band { StrongCandidate, Workable, NotRecommended };

    float overallScore = 0.0f; // 0..100
    Band band = Band::NotRecommended;

    // Per-criterion 0..100 sub-scores, surfaced so the UI can show *why*.
    float stereoWidthScore = 0.0f;
    float monoCompatScore = 0.0f;
    float phaseSafetyScore = 0.0f;
    float dynamicHeadroomScore = 0.0f;

    std::string summary; // one-line plain-language readout

    /// A single illustrative object placement for the reference visual -
    /// deliberately approximate (there is no way to derive true per-object
    /// spatial positions from a stereo file), positioned from real
    /// measurements where one exists (bass centring, stereo width) and a
    /// reasonable fixed role otherwise (vocal/centre, height/air).
    struct VirtualObject {
        std::string label;
        float x = 0.0f; // -1 (left) .. 1 (right)
        float y = 0.0f; // -1 (front) .. 1 (rear)
        float z = 0.0f; // -1 (floor) .. 1 (ceiling/height)
    };
    std::vector<VirtualObject> virtualObjects;
};

/// Pure function over already-computed analysis fields - no new audio
/// processing, no Python round trip. Every recommendation-engine style
/// module in this codebase does something similar (MixReportBuilder,
/// MixScorecard); this follows the same shape for the spatial-audio angle.
SpatialAudioAssessment assessSpatialAudioReadiness(const AnalysisResult& r);

const char* toString(SpatialAudioAssessment::Band band);

} // namespace khensu
