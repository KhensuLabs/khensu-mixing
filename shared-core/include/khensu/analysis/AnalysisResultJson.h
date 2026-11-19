// shared-core/include/khensu/analysis/AnalysisResultJson.h
// WF-8: the measured part of an AnalysisResult as JSON - what the native
// analyzers produce (loudness, dynamics, spectrum, stereo, phase, artifacts,
// mono compatibility, layout, sample rate). The desktop stores it with each
// analysis (SessionManager::saveAnalysisResult, data_json["measured"]), so the
// plugin can grade the session the desktop analysed without decoding and
// analysing the file again. Recommendations and service-side data are not
// part of it: the engine re-derives findings from these measurements.
#pragma once
#include <juce_core/juce_core.h>
#include <khensu/analysis/AnalysisResult.h>

namespace khensu::analysis_json {

/// Bump when a field's meaning changes; a stored block of another version is
/// ignored (the reader re-analyses).
inline constexpr int kMeasuredVersion = 1;

juce::var measuredToVar(const AnalysisResult& r);
/// Fills the measured fields of `out` from `v`; false (and `out` untouched)
/// when `v` is not a measured block of kMeasuredVersion.
bool measuredFromVar(const juce::var& v, AnalysisResult& out);

/// PERF-3: a reopened session reuses its stored native pass instead of
/// analysing the file again - only for the same file bytes (size and
/// modification time) and the same native analysers. Bump kNativeVersion
/// whenever AudioAnalyzer's output changes for the same input.
inline constexpr int kNativeVersion = 1;
/// {size, mtime_ms, native_version} of `file`; void when it does not exist.
juce::var sourceStamp(const juce::File& file);
/// True when `measured` carries a stamp that still matches `file`.
bool sourceMatches(const juce::var& measured, const juce::File& file);

} // namespace khensu::analysis_json
