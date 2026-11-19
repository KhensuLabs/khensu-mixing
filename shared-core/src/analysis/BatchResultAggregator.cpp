#include <khensu/analysis/BatchResultAggregator.h>
#include <khensu/analysis/MusicTheory.h>
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace khensu {

AnalysisResult aggregateBatchResults(const std::vector<AnalysisResult>& batchResults,
                                     const std::string& sessionId)
{
    AnalysisResult finalResult;
    if (batchResults.empty()) return finalResult; // caller-checked precondition; defensive no-op

    if (batchResults.size() == 1) {
        finalResult = batchResults[0];
        return finalResult;
    }

    // A fresh id (CORE-01): SessionManager falls back to generating one, but
    // an aggregate with an empty id could not be told apart from "unsaved".
    finalResult.id = juce::Uuid().toString().toStdString();
    finalResult.session_id = sessionId;
    finalResult.file_id = "batch";
    finalResult.computed_at = batchResults[0].computed_at;
    finalResult.is_partial = false;
    finalResult.python_analysis_ran =
        std::all_of(batchResults.begin(), batchResults.end(), [](const AnalysisResult& r) { return r.python_analysis_ran; });
    // Every input any file asked the engine for and did not get (FIX-2): a
    // re-run once the engine is back can add it.
    for (const auto& r : batchResults)
        finalResult.inputs_failed |= r.inputs_failed;
    // Doesn't average meaningfully across files that may differ - carry the
    // first file's rate, same representative-file policy as section_analysis
    // etc. below.
    finalResult.sample_rate = batchResults[0].sample_rate;
    // Same policy for the per-measurement trust map (pipeline #5).
    finalResult.measurement_trust = batchResults[0].measurement_trust;

    double sumLufsPower = 0.0;
    float maxTp = -99.0f, sumLra = 0.0f;
    int countLra = 0;
    float sumCrest = 0.0f, sumPeakToRms = 0.0f, sumDr = 0.0f;
    float sumWidth = 0.0f, sumMsBalance = 0.0f;
    float minCorr = 1.0f, minPhaseCorr = 1.0f;
    bool anyPolarity = false, anyAntiPhase = false, anyClipping = false;
    long long clipFramesWide = 0;
    float maxNoiseFloor = -200.0f, sumReversal = 0.0f;
    float noiseSeconds = 0.0f, noiseSpread = 0.0f;
    float worstDc = 0.0f; // largest |DC offset|, sign kept (MEAS-4)
    bool worstDcSteady = true;
    float sumHookStr = 0.0f, sumMem = 0.0f, sumHeavy = 0.0f;
    float sumBalLow = 0.0f, sumBalMid = 0.0f, sumBalHigh = 0.0f, sumCentroid = 0.0f;
    std::vector<float> bpms; // index-aligned with batchResults; 0 = undetected
    bpms.reserve(batchResults.size());

    for (const auto& r : batchResults) {
        // Loudness sums in the POWER domain (BS.1770 mean-square), not dB.
        sumLufsPower += std::pow(10.0, (static_cast<double>(r.loudness.lufs_integrated) + 0.691) / 10.0);
        maxTp = std::max(maxTp, r.loudness.true_peak_dbtp);
        if (r.loudness.lra_measured) {
            sumLra += r.loudness.lra;
            ++countLra;
        }
        sumCrest += r.dynamics.crest_factor_db;
        sumPeakToRms += r.dynamics.peak_to_rms_db;
        sumDr += r.dynamics.dynamic_range_db;
        sumWidth += r.stereo.width;
        sumMsBalance += r.stereo.ms_balance;
        minCorr = std::min(minCorr, r.stereo.correlation);
        minPhaseCorr = std::min(minPhaseCorr, r.phase.overall_correlation);
        anyPolarity = anyPolarity || r.phase.has_polarity_issue;
        anyAntiPhase = anyAntiPhase || r.phase.has_anti_phase_energy || r.phase.has_comb_filtering;
        anyClipping = anyClipping || r.artifacts.has_clipping;
        clipFramesWide += std::max(0, r.artifacts.clipping_frame_count);
        if (r.artifacts.noise_floor_db > maxNoiseFloor) {
            maxNoiseFloor = r.artifacts.noise_floor_db;
            noiseSeconds = r.artifacts.noise_quiet_seconds;
            noiseSpread = r.artifacts.noise_floor_spread_db;
        }
        if (std::abs(r.artifacts.dc_offset) > std::abs(worstDc)) {
            worstDc = r.artifacts.dc_offset;
            worstDcSteady = r.artifacts.dc_offset_steady;
        }
        sumReversal += r.artifacts.sample_reversal_rate;
        sumBalLow += r.spectral.balance_low;
        sumBalMid += r.spectral.balance_mid;
        sumBalHigh += r.spectral.balance_high;
        sumCentroid += r.spectral.centroid_hz;

        bpms.push_back(r.python_data.bpm_detected);
        sumHookStr += r.python_data.hooks.hook_strength;
        sumMem += r.python_data.memorability.score;
        sumHeavy += r.python_data.heaviness.distortion;
    }

    const float scale = 1.0f / static_cast<float>(batchResults.size());
    const int clipFrames = static_cast<int>(std::min<long long>(clipFramesWide, std::numeric_limits<int>::max()));
    // Album-style integrated loudness: power mean of the files (equal-duration
    // weighting - durations aren't carried here), not an arithmetic mean of
    // dB values. PLR is derived from the reported TP and LUFS so the three
    // numbers are mutually consistent (averaging PLR separately was not).
    const double meanPower = sumLufsPower * static_cast<double>(scale);
    finalResult.loudness.lufs_integrated =
        meanPower > 0.0 ? static_cast<float>(-0.691 + 10.0 * std::log10(meanPower)) : -99.0f;
    finalResult.loudness.true_peak_dbtp = maxTp;
    finalResult.loudness.lra_measured = countLra > 0;
    finalResult.loudness.lra = countLra > 0 ? sumLra / static_cast<float>(countLra) : 0.0f;
    finalResult.loudness.plr = finalResult.loudness.true_peak_dbtp - finalResult.loudness.lufs_integrated;

    finalResult.dynamics.crest_factor_db = sumCrest * scale;
    finalResult.dynamics.dynamic_range_db = sumDr * scale;
    finalResult.dynamics.peak_to_rms_db = sumPeakToRms * scale;

    finalResult.stereo.width = sumWidth * scale;
    // Worst case, not the mean (CORE-01): one anti-phase file in a batch is a
    // phase problem for that release - averaging it with clean files hid it.
    finalResult.stereo.correlation = minCorr;
    finalResult.stereo.ms_balance = sumMsBalance * scale;

    // --- Phase + artifacts: worst case across the batch (CORE-01). These
    // were never set, so every aggregate read "no clipping, phase clean" and
    // the Dashboard, readiness score and saved history all believed it. ---
    finalResult.phase.overall_correlation = minPhaseCorr;
    finalResult.phase.has_polarity_issue = anyPolarity;
    finalResult.phase.has_anti_phase_energy = anyAntiPhase;
    finalResult.phase.has_comb_filtering = anyAntiPhase;
    finalResult.artifacts.has_clipping = anyClipping;
    finalResult.artifacts.clipping_frame_count = clipFrames;
    finalResult.artifacts.noise_floor_db = maxNoiseFloor;
    finalResult.artifacts.noise_quiet_seconds = noiseSeconds;
    finalResult.artifacts.noise_floor_spread_db = noiseSpread;
    finalResult.artifacts.dc_offset = worstDc;
    finalResult.artifacts.dc_offset_steady = worstDcSteady;
    finalResult.artifacts.sample_reversal_rate = sumReversal * scale;

    // --- Channel layout (CORE-01): channel_count 0 means "not measured", so
    // a batch of mono files got "stereo too narrow". All files agree -> that
    // layout; otherwise the widest one, with lfe_excluded if any file had an
    // LFE excluded. ---
    {
        const auto* widest = &batchResults[0].channel_layout;
        bool allSame = true;
        bool anyLfeExcluded = false;
        for (const auto& r : batchResults) {
            const auto& cl = r.channel_layout;
            if (cl.channel_count != batchResults[0].channel_layout.channel_count ||
                cl.layout_name != batchResults[0].channel_layout.layout_name)
                allSame = false;
            if (cl.channel_count > widest->channel_count) widest = &cl;
            anyLfeExcluded = anyLfeExcluded || cl.lfe_excluded;
        }
        finalResult.channel_layout = allSame ? batchResults[0].channel_layout : *widest;
        finalResult.channel_layout.lfe_excluded = anyLfeExcluded;
    }

    finalResult.spectral.balance_low = sumBalLow * scale;
    finalResult.spectral.balance_mid = sumBalMid * scale;
    finalResult.spectral.balance_high = sumBalHigh * scale;
    finalResult.spectral.centroid_hz = sumCentroid * scale;

    // Per-band/per-bin spectral arrays don't average meaningfully either (bin
    // N in one file's FFT isn't the same Hz as bin N in another's once
    // sample_rate differs, same reasoning as sample_rate above) - representative
    // first-file carry-over. Previously dropped entirely, which silently
    // starved SpectralPanel/TonalBalancePanel of the data they render for any
    // multi-file (batch) session - they only ever worked for single-file runs.
    finalResult.spectral.rms_per_octave_db = batchResults[0].spectral.rms_per_octave_db;
    finalResult.spectral.rms_mid_per_octave_db = batchResults[0].spectral.rms_mid_per_octave_db;
    finalResult.spectral.rms_side_per_octave_db = batchResults[0].spectral.rms_side_per_octave_db;
    finalResult.spectral.fft_mid_magnitudes = batchResults[0].spectral.fft_mid_magnitudes;
    finalResult.spectral.fft_side_magnitudes = batchResults[0].spectral.fft_side_magnitudes;
    finalResult.spectral.tilt_db_per_octave = batchResults[0].spectral.tilt_db_per_octave;

    // Sections / drums / chords / extended metrics don't average
    // meaningfully across different files - carry the first file's
    // results as the representative set (per-file data remains
    // available in batch_results).
    finalResult.section_analysis = batchResults[0].section_analysis;
    finalResult.drums = batchResults[0].drums;
    finalResult.chord_analysis = batchResults[0].chord_analysis;
    finalResult.mono_compat = batchResults[0].mono_compat;
    finalResult.translation_risk = batchResults[0].translation_risk;
    finalResult.harshness = batchResults[0].harshness;
    finalResult.resonance = batchResults[0].resonance;
    finalResult.mid_side_imbalance = batchResults[0].mid_side_imbalance;
    finalResult.reference_deltas = batchResults[0].reference_deltas;
    finalResult.emotional_arc = batchResults[0].emotional_arc;
    finalResult.genre_blend = batchResults[0].genre_blend;
    finalResult.translation_prediction = batchResults[0].translation_prediction;
    finalResult.benchmark = batchResults[0].benchmark;
    finalResult.stereo_field_timeline = batchResults[0].stereo_field_timeline;

    // Tempo (AUDIT-050): an arithmetic mean of octave-mismatched detections
    // (70 + 140 -> 105) is a tempo no file has. Use the octave-folded median,
    // and derive the confidence from the files' REAL confidences, scaled by
    // how many of them agree with the consensus - never a made-up constant.
    if (const auto tempo = theory::foldedTempoConsensus(bpms)) {
        finalResult.python_data.bpm_detected = tempo->bpm;
        float sumConf = 0.0f;
        for (size_t i : tempo->agreeing)
            sumConf += batchResults[i].python_data.bpm_confidence;
        finalResult.python_data.bpm_confidence =
            tempo->agreeing.empty() ? 0.0f : (sumConf / static_cast<float>(tempo->agreeing.size())) * tempo->agreement;
    }
    finalResult.python_data.hooks.hook_strength = sumHookStr * scale;
    finalResult.python_data.memorability.score = sumMem * scale;
    finalResult.python_data.heaviness.distortion = sumHeavy * scale;
    // Key, meter and the beat/bar grids are per-file facts: representative
    // first-file carry-over, WITH their confidences (the key confidence used
    // to be dropped, so every batch key read as 0% sure).
    const auto& rep = batchResults[0].python_data;
    finalResult.python_data.pitch_key = rep.pitch_key;
    finalResult.python_data.pitch_confidence = rep.pitch_confidence;
    finalResult.python_data.camelot = rep.camelot;
    finalResult.python_data.relative_key = rep.relative_key;
    finalResult.python_data.time_signature = rep.time_signature;
    finalResult.python_data.meter_confidence = rep.meter_confidence;
    finalResult.python_data.felt_bpm = rep.felt_bpm;
    finalResult.python_data.beat_times_json = rep.beat_times_json;
    finalResult.python_data.downbeat_times_json = rep.downbeat_times_json;
    finalResult.python_data.sections_json = rep.sections_json;
    finalResult.python_data.pitch_notes_json = rep.pitch_notes_json;
    finalResult.python_data.genre_detected = batchResults[0].python_data.genre_detected;

    // Merge all recommendations, tagging each with the file it came from
    // (audit CQ-07) so a merged batch result doesn't lose which file a
    // suggestion applies to.
    for (const auto& r : batchResults) {
        for (auto rec : r.recommendations) {
            rec.source_file_id = r.file_id;
            finalResult.recommendations.push_back(std::move(rec));
        }
    }
    // Same ordering contract as RecommendationEngine::generate: most severe
    // first, so the merged list can be displayed/scored as-is (CORE-01 -
    // RecommendationEngine::recommendationsFor returns it for batch results
    // instead of recomputing from the aggregate).
    std::stable_sort(finalResult.recommendations.begin(), finalResult.recommendations.end(),
                     [](const Recommendation& a, const Recommendation& b) { return a.severity > b.severity; });
    finalResult.batch_results = batchResults;

    return finalResult;
}

} // namespace khensu
