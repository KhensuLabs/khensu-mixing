#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <string>
#include <vector>

namespace khensu {

/// Combines per-file AnalysisResults from a multi-file batch analysis into
/// one representative result (audit CQ-07). Extracted from
/// AnalysisController's inline aggregation loop so the field-by-field
/// mean/max/representative-file policy is a pure, independently testable
/// unit instead of buried in an async analysis pipeline.
///
/// Field-level strategy:
///  - Loudness/dynamics/stereo/spectral scalars: arithmetic mean across
///    files, except true_peak_dbtp which takes the max (the loudest single
///    peak across the batch is the one that matters for clipping risk).
///  - BPM: octave-folded median of the files where detection succeeded
///    (bpm_detected > 0), reported in the octave most files were detected
///    in, so 70 and 140 agree on 70 instead of averaging to 105.
///    bpm_confidence is the mean real confidence of the agreeing files
///    scaled by the fraction that agree (theory::foldedTempoConsensus).
///  - Key (+ its confidence, Camelot), meter, felt tempo and the beat /
///    downbeat / section grids: the first file's, as the representative set.
///  - Section/drum/chord/extended-metric analyses don't average
///    meaningfully across different files - the first file's values are
///    carried as the representative set; the full per-file data remains
///    available via AnalysisResult::batch_results.
///  - Stereo correlation, phase and artifacts are WORST CASE (CORE-01):
///    minimum correlation, any polarity / anti-phase flag, any clipping with
///    the summed clipped-frame count, the highest noise floor. Downstream
///    scores built on the aggregate must not read "clean" when a file clips.
///  - channel_layout: the shared layout when every file agrees, else the
///    widest; id: a fresh UUID.
///  - Recommendations are merged from every file, each tagged with
///    Recommendation::source_file_id so the UI/report can attribute which
///    file a suggestion came from instead of losing that on merge, and
///    sorted most-severe first. Use RecommendationEngine::recommendationsFor
///    to get them back rather than re-running the engine on the aggregate.
///
/// THREAD_CONTEXT(AnyThread) - pure function, no shared state.
///
/// Precondition: `batchResults` must be non-empty (callers already handle
/// the empty-batch case as an analysis failure before reaching this point).
AnalysisResult aggregateBatchResults(const std::vector<AnalysisResult>& batchResults,
                                     const std::string& sessionId);

} // namespace khensu
