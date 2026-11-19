#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <juce_core/juce_core.h>
#include <optional>
#include <string>

namespace khensu::ipc {

struct DeepAnalysisResponse {
    std::string mode;
    float bpm = 0.0f;
    float bpm_confidence = 0.0f;
    std::string key;
    float key_confidence = 0.0f;
    std::string beat_times_json;
    std::string downbeat_times_json;
    std::string sections_json;
    std::string pitch_notes_json;
    float analysis_duration_ms = 0.0f;
    std::string detected_genre;
    std::string detected_subgenre;
    float energy = 0.0f; // 0-100, perceptual energy heuristic
    std::string mood;    // valence/arousal heuristic label
    // Advanced mode only (see AnalysisResult::PythonData for meanings).
    std::string time_signature;
    float meter_confidence = 0.0f;
    float felt_bpm = 0.0f;
    std::string camelot;
    std::string relative_key;

    // SP-06 - zero/empty by default; populated only from advanced-mode responses
    std::vector<khensu::HookInstance> hook_instances;
    float hook_strength = 0.0f;
    float hook_density = 0.0f;
    int hook_count = 0;
    float memorability_score = 0.0f;
    float earworm_likelihood = 0.0f;
    float melody_salience = 0.0f;
    float arc_quality = 0.0f;
    float heaviness_distortion = 0.0f;
    float heaviness_weight = 0.0f;
    float heaviness_clarity = 0.0f;
    float heaviness_extremity = 0.0f;

    float skewness = 0.0f;
    float peak_asymmetry_ratio = 1.0f;
    bool is_asymmetric = false;

    // Music-theory identity beyond the fields above (advanced/extended only;
    // empty / -1 when absent).
    int circle_of_fifths_index = -1;
    std::string parallel_key;

    std::optional<khensu::AnalysisResult::MonoCompatData> mono_compat;
    std::optional<khensu::AnalysisResult::TranslationRiskData> translation_risk;
    std::optional<khensu::AnalysisResult::HarshnessData> harshness;
    std::optional<khensu::AnalysisResult::ResonanceData> resonance;
    std::optional<khensu::AnalysisResult::MidSideImbalanceData> mid_side_imbalance;

    // 2026-07-09 AI-analysis-deepening additions
    std::optional<khensu::AnalysisResult::EmotionalArcData> emotional_arc;
    std::vector<khensu::AnalysisResult::GenreBlendEntry> genre_blend;
    std::string genre_target_source;
    std::string genre_confirmation_status;
    std::string genre_abstained_reason;
    bool genre_relative_action_allowed = false;
    bool genre_is_hybrid = false;
    std::optional<khensu::AnalysisResult::TranslationPredictionData> translation_prediction;
    std::optional<khensu::AnalysisResult::BenchmarkData> benchmark;
    std::optional<khensu::AnalysisResult::StereoFieldTimeline> stereo_field_timeline;

    static std::optional<DeepAnalysisResponse> from_json(const juce::var& json);
    /// Parse a /analyze/chords body (also used by tests). nullopt when it is not an object.
    static std::optional<khensu::ChordAnalysis> chords_from_json(const juce::var& json);
    void apply_to(khensu::AnalysisResult::PythonData& dst) const noexcept;
};

} // namespace khensu::ipc
