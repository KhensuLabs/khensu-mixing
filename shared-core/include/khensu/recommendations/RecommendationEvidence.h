#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/recommendations/Recommendation.h>
#include <khensu/targets/GenreEntry.h>
#include <string>
#include <vector>

namespace khensu {

/// CE-TRU-3: attach the measurement evidence behind each recommendation -
/// measurement, value, unit, threshold and threshold source (genre norm,
/// reference track or an absolute standard) - keyed by the issue code
/// (`issueCodeOf`). The thresholds mirror the ones each recommender fires on,
/// so the evidence says exactly why the card exists. A recommendation with a
/// non-zero `reference_delta` also gets a "reference" item. Codes that are a
/// judgement across several analyses (hooks, memorability, flow) get none.
void attachEvidence(std::vector<Recommendation>& recs, const AnalysisResult& result, const GenreEntry& genre);

/// The evidence for one issue code (exposed for tests).
std::vector<EvidenceItem> evidenceForCode(const std::string& code, const AnalysisResult& result,
                                          const GenreEntry& genre);

} // namespace khensu
