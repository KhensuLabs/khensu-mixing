#pragma once
#include <string>
#include <array>
#include <vector>
#include <utility>
#include <algorithm>
#include <unordered_map>

namespace khensu {

/// Production norms for a single genre, loaded from genre_targets.json.
/// Used by TargetInterpolator and RecommendationEngine to derive genre-specific thresholds.
/// THREAD_CONTEXT(AnyThread) - plain data struct.
struct GenreEntry {
    // --- Identity ---
    std::string id;     // Machine key, e.g. "trap"
    std::string name;   // Display name, e.g. "Trap"
    std::string parent; // Parent genre id, e.g. "hip-hop"
    std::string notes;  // Human-readable production notes

    // --- Loudness ---
    float lufs_target = -14.0f;
    float lufs_range_min = -16.0f;
    float lufs_range_max = -11.0f;
    float lra_range_min = 5.0f;
    float lra_range_max = 10.0f;
    float true_peak_max = -1.0f;

    // --- Spectral ---
    float spectral_centroid_hz = 3000.0f;
    float low_end_weight = 0.5f; // 0-1: relative prominence of sub/bass
    // Target tonal-balance shape (dB/octave slope) and dynamics shape (crest
    // factor), scored against AnalysisResult::SpectralData::tilt_db_per_octave
    // and DynamicsData::crest_factor_db - a fuller genre curve than a single
    // centroid scalar, e.g. distinguishing a wall-of-sound shoegaze target
    // (bright-ish tilt, low crest - dense, compressed) from a transient-
    // forward hyperpop target (very bright tilt, very low crest - hyper-
    // limited) or an ambient target (dark tilt, high crest - dynamic, airy).
    // Populated in GenreDatabase from optional JSON fields, falling back to
    // a formula derived from spectral_centroid_hz/compression_ratio_typical
    // for genres that don't specify them explicitly.
    // Octave-band-power convention (pink noise = 0 dB/oct): the old -4.5
    // density-convention default + 3.01 (AUDIT-009, see GenreDatabase.cpp).
    float spectral_tilt_target_db_per_octave = -1.49f;
    float crest_factor_target_db = 10.0f;
    /// TUNE-4: the 90th percentile of this genre's released tracks' whole-
    /// track crest factor (measure_genre_norms.py apply_crest); 0 = not
    /// measured. crest_above_genre_target fires above the higher of this and
    /// target + 3 dB.
    float crest_factor_ceiling_db = 0.0f;

    // --- Stereo ---
    float stereo_width_min = 0.4f;
    float stereo_width_max = 0.8f;

    // --- Dynamics ---
    float compression_ratio_typical = 3.0f;

    // --- Heaviness (all 0-1) ---
    float heaviness_distortion_norm = 0.2f;
    float heaviness_weight_norm = 0.4f;
    float heaviness_clarity_norm = 0.6f;
    float heaviness_extremity_norm = 0.2f;

    // --- Hooks & Structure ---
    float hook_density_norm = 0.5f;    // 0-1: expected hook density
    float chorus_contrast_norm = 0.5f; // 0-1: expected verse→chorus loudness/energy delta

    // --- Tempo & Vocal ---
    float typical_bpm_min = 90.0f;
    float typical_bpm_max = 140.0f;
    float vocal_presence_db = 0.0f; // dB above mix: positive = forward, negative = buried

    // --- Stem-energy balance (canonical 9 buckets, 0-100%) ---
    // Optional per-genre target shares for the stem taxonomy panels.
    // Missing buckets are treated as 0 by consumers; the Python analyzer
    // normalizes leaf buckets to ~100% when an explicit block is present.
    std::unordered_map<std::string, float> stem_energy_balance_pct;

    bool is_valid() const noexcept { return !id.empty(); }
};

/// Weight-averages every numeric target field across a genre blend (see
/// AnalysisResult::GenreBlendEntry) - used when a track sits between
/// genres (e.g. 68% hyperpop / 24% shoegaze) so downstream scoring can
/// compare against a blended curve instead of forcing the single primary
/// genre's targets onto a track that's meaningfully a hybrid.
///
/// Identity fields (id/name/parent/notes) are taken from the
/// highest-weighted entry, since those aren't numeric and don't blend.
/// Weights are normalized internally, so callers can pass raw
/// probabilities without summing to 1. Returns a default-constructed
/// (invalid) GenreEntry if entries is empty.
///
/// First slice: this only blends the target *values* - it does not yet
/// change which recommenders/scorers request a blended entry instead of a
/// single-genre one (SpectralRecommender, DynamicsRecommender, mixer-chain
/// generation still look up a single genre). Wiring every consumer to be
/// blend-aware is a follow-up; today this is used for display (the
/// genre-blend UI augmentation) and is available for any future caller
/// that wants a blended target curve.
inline GenreEntry blendGenreEntries(const std::vector<std::pair<GenreEntry, float>>& entries)
{
    if (entries.empty()) return GenreEntry{};
    if (entries.size() == 1) return entries.front().first;

    float totalWeight = 0.0f;
    for (const auto& [entry, weight] : entries)
        totalWeight += weight;
    if (totalWeight <= 0.0f) return entries.front().first;

    const auto& primary = std::max_element(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                              return a.second < b.second;
                          })->first;

    GenreEntry blended;
    blended.id = primary.id;
    blended.name = primary.name;
    blended.parent = primary.parent;
    blended.notes = primary.notes;

    auto mix = [&](auto GenreEntry::* field) {
        float sum = 0.0f;
        for (const auto& [entry, weight] : entries)
            sum += (entry.*field) * weight;
        return sum / totalWeight;
    };

    blended.lufs_target = mix(&GenreEntry::lufs_target);
    blended.lufs_range_min = mix(&GenreEntry::lufs_range_min);
    blended.lufs_range_max = mix(&GenreEntry::lufs_range_max);
    blended.lra_range_min = mix(&GenreEntry::lra_range_min);
    blended.lra_range_max = mix(&GenreEntry::lra_range_max);
    blended.true_peak_max = mix(&GenreEntry::true_peak_max);
    blended.spectral_centroid_hz = mix(&GenreEntry::spectral_centroid_hz);
    blended.low_end_weight = mix(&GenreEntry::low_end_weight);
    blended.spectral_tilt_target_db_per_octave = mix(&GenreEntry::spectral_tilt_target_db_per_octave);
    blended.crest_factor_target_db = mix(&GenreEntry::crest_factor_target_db);
    // One unmeasured (0) genre in the blend: fall back to target + 3.
    const bool allCrestMeasured = std::all_of(entries.begin(), entries.end(), [](const auto& e) {
        return e.first.crest_factor_ceiling_db > 0.0f;
    });
    blended.crest_factor_ceiling_db = allCrestMeasured ? mix(&GenreEntry::crest_factor_ceiling_db) : 0.0f;
    blended.stereo_width_min = mix(&GenreEntry::stereo_width_min);
    blended.stereo_width_max = mix(&GenreEntry::stereo_width_max);
    blended.compression_ratio_typical = mix(&GenreEntry::compression_ratio_typical);
    blended.heaviness_distortion_norm = mix(&GenreEntry::heaviness_distortion_norm);
    blended.heaviness_weight_norm = mix(&GenreEntry::heaviness_weight_norm);
    blended.heaviness_clarity_norm = mix(&GenreEntry::heaviness_clarity_norm);
    blended.heaviness_extremity_norm = mix(&GenreEntry::heaviness_extremity_norm);
    blended.hook_density_norm = mix(&GenreEntry::hook_density_norm);
    blended.chorus_contrast_norm = mix(&GenreEntry::chorus_contrast_norm);
    blended.typical_bpm_min = mix(&GenreEntry::typical_bpm_min);
    blended.typical_bpm_max = mix(&GenreEntry::typical_bpm_max);
    blended.vocal_presence_db = mix(&GenreEntry::vocal_presence_db);

    // Blend stem-energy buckets. A bucket an entry doesn't define counts as
    // 0 for that entry (the documented "missing = 0" rule), so divide by the
    // weight of ALL entries - averaging only over the definers let blended
    // shares sum past 100%.
    std::unordered_map<std::string, float> bucketWeights;
    float allWeight = 0.0f;
    for (const auto& [entry, weight] : entries) {
        allWeight += weight;
        for (const auto& [bucket, value] : entry.stem_energy_balance_pct) bucketWeights[bucket] += value * weight;
    }
    if (allWeight > 0.0f)
        for (const auto& [bucket, weighted] : bucketWeights)
            blended.stem_energy_balance_pct[bucket] = weighted / allWeight;

    return blended;
}

} // namespace khensu
