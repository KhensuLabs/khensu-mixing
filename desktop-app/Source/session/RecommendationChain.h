// desktop-app/Source/session/RecommendationChain.h
#pragma once
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/recommendations/Recommendation.h>
#include <vector>

namespace khensu {

/// The effect chain that renders `rec`'s suggested parameters (the Feedback
/// Decoder's keys mapped onto dsp.py's effect vocabulary). Empty when the
/// finding is advice-only - nothing concrete to render.
std::vector<ipc::HttpServiceBridge::EffectSpec> buildRecommendationChain(const Recommendation& rec);

/// NEW-4: the finding's suggested move at `amount` of its size (0.5 = half):
/// every gain / drive / boost in dB scales toward 0, a compressor ratio and a
/// stereo width toward 1 (no change); levels (thresholds) and frequencies stay.
Recommendation scaledRecommendation(const Recommendation& rec, float amount);

/// True when buildRecommendationChain(rec) would render something.
inline bool hasRenderableChain(const Recommendation& rec) { return !buildRecommendationChain(rec).empty(); }

} // namespace khensu
