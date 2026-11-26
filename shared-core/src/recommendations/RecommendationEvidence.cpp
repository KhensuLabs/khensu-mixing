// shared-core/src/recommendations/RecommendationEvidence.cpp
// CE-TRU-3: the measurements behind each recommendation.
#include <khensu/recommendations/RecommendationEvidence.h>
#include <khensu/recommendations/DynamicsRecommender.h>
#include <khensu/recommendations/SpectralRecommender.h>
#include <cmath>

namespace khensu {

namespace {

EvidenceItem item(const char* measurement, float value, const char* unit, std::optional<float> threshold,
                  const char* comparison, const char* source, std::optional<float> low = std::nullopt)
{
    EvidenceItem e;
    e.measurement = measurement;
    e.value = value;
    e.unit = unit;
    e.threshold = threshold;
    e.threshold_low = low;
    e.comparison = comparison;
    e.source = source;
    return e;
}

constexpr const char* kGenre = "genre_norm";
constexpr const char* kAbsolute = "absolute";

// The same constant the recommender fires on (kept beside its use there). The
// spectral and dynamics thresholds below are read from the recommenders
// themselves, so the evidence cannot show a different edge from the card.
constexpr float kNoiseFloorLimitDb = -50.0f; // ArtifactRecommender
constexpr float kTiltToleranceDbPerOctave = SpectralRecommender::kTiltToleranceDbPerOctave;

} // namespace

std::vector<EvidenceItem> evidenceForCode(const std::string& code, const AnalysisResult& result,
                                          const GenreEntry& genre)
{
    const auto& l = result.loudness;
    const auto& s = result.spectral;
    const auto& d = result.dynamics;
    const auto& st = result.stereo;
    const auto& a = result.artifacts;
    std::vector<EvidenceItem> out;

    if (code == "lufs_too_loud")
        out.push_back(item("Integrated loudness", l.lufs_integrated, "LUFS", genre.lufs_range_max, "above", kGenre));
    else if (code == "lufs_too_quiet") {
        out.push_back(item("Integrated loudness", l.lufs_integrated, "LUFS", genre.lufs_range_min, "below", kGenre));
        out.push_back(item("True peak (headroom for gain)", l.true_peak_dbtp, "dBTP", -1.0f, "", kAbsolute));
    } else if (code == "strength_lufs_on_target")
        out.push_back(item("Integrated loudness", l.lufs_integrated, "LUFS", genre.lufs_range_max, "within", kGenre,
                           genre.lufs_range_min));
    else if (code == "lra_too_compressed")
        out.push_back(item("Loudness range", l.lra, "LU", genre.lra_range_min, "below", kGenre));
    else if (code == "lra_too_dynamic")
        out.push_back(item("Loudness range", l.lra, "LU", genre.lra_range_max, "above", kGenre));
    else if (code == "strength_dynamics_in_pocket")
        out.push_back(item("Loudness range", l.lra, "LU", genre.lra_range_max, "within", kGenre, genre.lra_range_min));
    else if (code == "under_compressed")
        out.push_back(item("Loudness range", l.lra, "LU", DynamicsRecommender::underCompressedLraThreshold(genre),
                           "above", kGenre));
    else if (code == "true_peak_over")
        out.push_back(item("True peak (4x oversampled)", l.true_peak_dbtp, "dBTP", genre.true_peak_max, "above", kGenre));
    else if (code == "strength_true_peak_safe")
        out.push_back(item("True peak (4x oversampled)", l.true_peak_dbtp, "dBTP", genre.true_peak_max, "below", kGenre));
    else if (code == "over_compressed")
        out.push_back(item("Crest factor", d.crest_factor_db, "dB", DynamicsRecommender::overCompressedFloorDb(genre),
                           "below", kGenre));
    else if (code == "crest_above_genre_target")
        out.push_back(item("Crest factor", d.crest_factor_db, "dB",
                           DynamicsRecommender::crestCeilingDb(genre), "above", kGenre));
    else if (code == "strength_transient_headroom")
        out.push_back(item("Crest factor", d.crest_factor_db, "dB", DynamicsRecommender::overCompressedFloorDb(genre),
                           "above", kGenre));
    else if (code == "centroid_too_bright")
        out.push_back(item("Spectral centroid", s.centroid_hz, "Hz", SpectralRecommender::centroidCeilingHz(genre),
                           "above", kGenre));
    else if (code == "centroid_too_dark")
        out.push_back(item("Spectral centroid", s.centroid_hz, "Hz", SpectralRecommender::centroidFloorHz(genre),
                           "below", kGenre));
    else if (code == "tilt_too_bright")
        out.push_back(item("Spectral tilt", s.tilt_db_per_octave, "dB/oct",
                           genre.spectral_tilt_target_db_per_octave + kTiltToleranceDbPerOctave, "above", kGenre));
    else if (code == "tilt_too_dark")
        out.push_back(item("Spectral tilt", s.tilt_db_per_octave, "dB/oct",
                           genre.spectral_tilt_target_db_per_octave - kTiltToleranceDbPerOctave, "below", kGenre));
    else if (code == "low_end_heavy")
        out.push_back(item("Energy share below 250 Hz", s.balance_low, "", SpectralRecommender::lowEndCeiling(genre),
                           "above", kGenre));
    else if (code == "low_end_light")
        out.push_back(item("Energy share below 250 Hz", s.balance_low, "", SpectralRecommender::lowEndFloor(genre),
                           "below", kGenre));
    else if (code == "stereo_too_narrow")
        out.push_back(item("Stereo width", st.width, "", genre.stereo_width_min, "below", kGenre));
    else if (code == "stereo_too_wide") {
        out.push_back(item("Stereo width", st.width, "", genre.stereo_width_max, "above", kGenre));
        // STU-16: the width reading clamps at 1 once the side is as loud as
        // the mid; the M/S balance (M^2 - S^2) / (M^2 + S^2) does not, so the
        // width fix can still solve how far over it is.
        out.push_back(item("Mid/side balance", st.ms_balance, "", std::nullopt, "", kAbsolute));
    }
    else if (code == "phase_issue")
        out.push_back(item("Stereo correlation", st.correlation, "", AnalysisResult::PhaseData::kPhaseIssueCorrelation,
                           "below", kAbsolute));
    else if (code == "strength_phase_clean")
        out.push_back(item("Stereo correlation", st.correlation, "", AnalysisResult::PhaseData::kPhaseIssueCorrelation,
                           "above", kAbsolute));
    else if (code == "clipping_detected")
        out.push_back(item("Clipped frames", (float)a.clipping_frame_count, "", 0.0f, "above", kAbsolute));
    else if (code == "noise_floor_high")
        out.push_back(item("Noise floor", a.noise_floor_db, "dB", kNoiseFloorLimitDb, "above", kAbsolute));
    else if (code == "dc_offset")
        out.push_back(item("DC offset (mean sample value)", std::abs(a.dc_offset), "",
                           AnalysisResult::ArtifactData::kDcOffsetThreshold, "above", kAbsolute));
    return out;
}

void attachEvidence(std::vector<Recommendation>& recs, const AnalysisResult& result, const GenreEntry& genre)
{
    for (auto& r : recs) {
        r.evidence = evidenceForCode(issueCodeOf(r, result.file_id), result, genre);
        if (r.reference_delta != 0.0f && std::isfinite(r.reference_delta))
            r.evidence.push_back(item("Difference from your reference track", r.reference_delta, "", 0.0f, "", "reference"));
    }
}

} // namespace khensu
